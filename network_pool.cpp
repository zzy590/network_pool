/* Copyright (c) 2018 Zhenyu Zhang. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "network_pool.h"
#include "np_dbg.h"

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define on_error_goto_ec(_expr, _str) if ((_expr) != 0) { NP_FPRINTF(_str); goto _ec; }
#define goto_ec(_str) { NP_FPRINTF(_str); goto _ec; }

namespace NETWORK_POOL
{
	//
	// CnetworkPool
	//

	void tcp_alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
	{
		Ctcp *tcp = Ctcp::obtainFromTcp(handle);
		// Every tcp_alloc_buffer will follow a on_tcp_read, so we don't care about the closing.
		void *buffer = nullptr;
		size_t length = 0;
		tcp->getPool()->m_callback.allocateMemoryForMessage(tcp->getNode(), suggested_size, buffer, length);
		buf->base = (char *)buffer;
	#ifdef _MSC_VER
		buf->len = (ULONG)length;
	#else
		buf->len = length;
	#endif
	}

	void on_tcp_timeout(uv_timer_t *handle)
	{
		Ctcp *tcp = Ctcp::obtain(handle);
		tcp->getPool()->shutdownTcpConnection_set_nullptr(tcp);
	}

	// This function should be called at last and the tcp ***MUST*** be no closing and no shutdown.
	// Note: It will shutdown tcp if set timer fail.
	void reset_tcp_idle_timeout_may_set_nullptr(Ctcp *& tcp)
	{
		// Reset idle timeout if needed.
		if (0 == tcp->getStream()->write_queue_size) // Use uv_stream_get_write_queue_size in libuv 1.19.0.
		{
			// No pending send, reset the timer.
			if (uv_timer_start(tcp->getTimer(), on_tcp_timeout, tcp->getPool()->getSettings().tcp_idle_timeout_in_seconds * 1000, 0) != 0)
				tcp->getPool()->shutdownTcpConnection_set_nullptr(tcp);
		}
	}

	void on_tcp_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf)
	{
		Ctcp *tcp = Ctcp::obtain(client);
		CnetworkPool *pool = tcp->getPool();
		if (nread > 0)
		{
			// Report message.
			pool->m_callback.message(tcp->getNode(), buf->base, nread);
			pool->m_callback.deallocateMemoryForMessage(tcp->getNode(), buf->base, buf->len);
			// Reset idle close.
			if (!tcp->isClosing() && !tcp->isShutdown())
				reset_tcp_idle_timeout_may_set_nullptr(tcp);
		}
		else
		{
			pool->m_callback.deallocateMemoryForMessage(tcp->getNode(), buf->base, buf->len);
			if (nread < 0)
			{
				if (nread != UV_EOF)
					NP_FPRINTF((stderr, "Read error %s.\n", uv_err_name((int)nread)));
				// Shutdown connection.
				pool->shutdownTcpConnection_set_nullptr(tcp);
			}
		}
	}

	void on_tcp_write_done(uv_write_t *req, int status)
	{
		CnetworkPool::__write_with_info *writeInfo = container_of(req, CnetworkPool::__write_with_info, write);
		Ctcp *tcp = Ctcp::obtain(req->handle);
		CnetworkPool *pool = tcp->getPool();
		if (status != 0)
		{
			NP_FPRINTF((stderr, "Tcp write error %s.\n", uv_strerror(status)));
			// Drop message notify.
			for (size_t i = 0; i < writeInfo->num; ++i)
				pool->m_callback.drop(tcp->getNode(), writeInfo->buf[i].base, writeInfo->buf[i].len);
			// Shutdown connection.
			pool->shutdownTcpConnection_set_nullptr(tcp);
		}
		else if (!tcp->isClosing() && !tcp->isShutdown())
			reset_tcp_idle_timeout_may_set_nullptr(tcp);
		// Free write buffer.
		for (size_t i = 0; i < writeInfo->num; ++i)
			pool->getMemoryTrace()._free_set_nullptr(writeInfo->buf[i].base);
		pool->getMemoryTrace()._free_set_nullptr(writeInfo);
	}

	void on_new_connection(uv_stream_t *server, int status)
	{
		CnetworkPool *pool = Ctcp::obtain(server)->getPool();
		if (status != 0)
		{
			// WTF? Listen fail?
			NP_FPRINTF((stderr, "Tcp listen error %s.\n", uv_strerror(status)));
			// Report this error and close the server.
			Ctcp *serverTcp = Ctcp::obtain(server);
			// Clean set.
			auto sz = pool->m_tcpServers.erase(serverTcp->getNode());
			if (sz > 0)
				pool->m_callback.bindStatus(serverTcp->getNode(), false);// Report bind down.
			// Close.
			Ctcp::close_set_nullptr(serverTcp);
			return;
		}
		// Prepare for the new connection.
		Ctcp *clientTcp = Ctcp::alloc(pool, &pool->m_loop);
		if (nullptr == clientTcp)
		{
			// Just return.
			NP_FPRINTF((stderr, "New incoming connection tcp allocation error.\n"));
			return;
		}
		on_error_goto_ec(
			uv_accept(server, clientTcp->getStream()),
			(stderr, "New incoming connection tcp accept error.\n"));
		sockaddr_storage peer;
		int len;
		len = sizeof(peer);
		on_error_goto_ec(
			uv_tcp_getpeername(clientTcp->getTcp(), (sockaddr *)&peer, &len),
			(stderr, "New incoming connection tcp getpeername error.\n"));
		if (!clientTcp->getNode().set(CnetworkNode::protocol_tcp, (const sockaddr *)&peer, len))
			goto_ec((stderr, "New incoming connection tcp set node error.\n"));
		// Do the port reuse check.
		if (pool->getStreamByNode(clientTcp->getNode()) != nullptr)
			goto_ec((stderr, "New incoming connection tcp remote port reuse.\n"));
		// Set idle timeout.
		on_error_goto_ec(
			uv_timer_start(clientTcp->getTimer(), on_tcp_timeout, pool->getSettings().tcp_idle_timeout_in_seconds * 1000, 0),
			(stderr, "New incoming connection tcp timer start error.\n"));
		// Start read.
		on_error_goto_ec(
			uv_read_start(clientTcp->getStream(), tcp_alloc_buffer, on_tcp_read),
			(stderr, "New incoming connection tcp read start error.\n"));
		// Startup connection.
		pool->startupTcpConnection_may_set_nullptr(clientTcp);
		return;
	_ec:
		Ctcp::close_set_nullptr(clientTcp);
	}

	void on_connect_done(uv_connect_t *req, int status)
	{
		Ctcp *tcp = Ctcp::obtain(req->handle);
		CnetworkPool *pool = tcp->getPool();
		// Remove from connecting and free request.
		pool->m_connecting.erase(tcp);
		pool->getMemoryTrace()._free_set_nullptr(req);
		// Error?
		if (status < 0 || tcp->isClosing()) // Closing may happen when deleting the pool with the connecting not completed.
			goto_ec((stderr, "Connect tcp error %s.\n", uv_strerror(status)));
		// Set timeout.
		on_error_goto_ec(
			uv_timer_start(tcp->getTimer(), on_tcp_timeout, tcp->getPool()->getSettings().tcp_idle_timeout_in_seconds * 1000, 0),
			(stderr, "Connect tcp timer start error.\n"));
		// Start read.
		on_error_goto_ec(
			uv_read_start(tcp->getStream(), tcp_alloc_buffer, on_tcp_read),
			(stderr, "Connect tcp read start error.\n"));
		// Startup connection.
		pool->startupTcpConnection_may_set_nullptr(tcp);
		return;
	_ec:
		// Shutdown connection(Always notify the connect fail).
		pool->shutdownTcpConnection_set_nullptr(tcp, true);
	}

	static Ctcp *bindAndListenTcp(CnetworkPool *pool, uv_loop_t *loop, const CnetworkNode& node)
	{
		if (node.getProtocol() != CnetworkNode::protocol_tcp)
			return nullptr;
		Ctcp *server = Ctcp::alloc(pool, loop, false);
		if (nullptr == server)
		{
			// Insufficient memory.
			NP_FPRINTF((stderr, "Bind and listen tcp error with insufficient memory.\n"));
			return nullptr;
		}
		server->getNode() = node;
		on_error_goto_ec(
			uv_tcp_bind(server->getTcp(), server->getNode().getSockaddr().getSockaddr(), 0),
			(stderr, "Bind and listen tcp bind error.\n"));
		on_error_goto_ec(
			uv_listen(server->getStream(), pool->getSettings().tcp_backlog, on_new_connection),
			(stderr, "Bind and listen tcp listen error.\n"));
		return server;
	_ec:
		Ctcp::close_set_nullptr(server);
		return nullptr;
	}

	static Ctcp *connectTcp(CnetworkPool *pool, uv_loop_t *loop, const CnetworkNode& node)
	{
		if (node.getProtocol() != CnetworkNode::protocol_tcp)
			return nullptr;
		uv_connect_t *connect = (uv_connect_t *)pool->getMemoryTrace()._malloc_no_throw(sizeof(uv_connect_t));
		if (nullptr == connect)
		{
			// Insufficient memory.
			NP_FPRINTF((stderr, "Connect tcp error with insufficient memory.\n"));
			return nullptr;
		}
		Ctcp *tcp = Ctcp::alloc(pool, loop);
		if (nullptr == tcp)
		{
			// Insufficient memory.
			// Just free & return.
			NP_FPRINTF((stderr, "Connect tcp error with insufficient memory.\n"));
			pool->getMemoryTrace()._free_set_nullptr(connect);
			return nullptr;
		}
		tcp->getNode() = node;
		// Set timeout.
		on_error_goto_ec(
			uv_timer_start(tcp->getTimer(), on_tcp_timeout, pool->getSettings().tcp_connect_timeout_in_seconds * 1000, 0),
			(stderr, "Connect tcp timer start error.\n"));
		// Connect.
		on_error_goto_ec(
			uv_tcp_connect(connect, tcp->getTcp(), tcp->getNode().getSockaddr().getSockaddr(), on_connect_done),
			(stderr, "Connect tcp connect error.\n"));
		return tcp;
	_ec:
		pool->getMemoryTrace()._free_set_nullptr(connect);
		Ctcp::close_set_nullptr(tcp);
		return nullptr;
	}

	void udp_alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
	{
		Cudp *udp = Cudp::obtain(handle);
		// Every udp_alloc_buffer will follow a on_udp_read, so we don't care about the closing.
		void *buffer = nullptr;
		size_t length = 0;
		udp->getPool()->m_callback.allocateMemoryForMessage(udp->getNode(), suggested_size, buffer, length);
		buf->base = (char *)buffer;
	#ifdef _MSC_VER
		buf->len = (ULONG)length;
	#else
		buf->len = length;
	#endif
	}

	void on_udp_recv(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf, const struct sockaddr *addr, unsigned flags)
	{
		Cudp *udp = Cudp::obtain(handle);
		CnetworkPool *pool = udp->getPool();
		if (nread < 0)
		{
			pool->m_callback.deallocateMemoryForMessage(udp->getNode(), buf->base, buf->len);
			NP_FPRINTF((stderr, "Recv udp error %s.\n", uv_err_name((int)nread)));
			// Stop and close udp.
			pool->stopAndCloseUdp_set_nullptr(udp);
		}
		else if (addr != nullptr)
		{
			// Report message.
			pool->m_callback.message(CnetworkNode(CnetworkNode::protocol_udp, addr, sizeof(sockaddr_storage)), buf->base, nread);
			pool->m_callback.deallocateMemoryForMessage(udp->getNode(), buf->base, buf->len);
		}
		else
			pool->m_callback.deallocateMemoryForMessage(udp->getNode(), buf->base, buf->len);
	}

	void on_udp_send_done(uv_udp_send_t *req, int status)
	{
		CnetworkPool::__udp_send_with_info *udpSendInfo = container_of(req, CnetworkPool::__udp_send_with_info, udpSend);
		Cudp *udp = Cudp::obtain(req->handle);
		CnetworkPool *pool = udp->getPool();
		if (status != 0)
		{
			NP_FPRINTF((stderr, "Udp write error %s.\n", uv_strerror(status)));
			// Udp don't have drop message notify.
			// Shutdown connection.
			pool->stopAndCloseUdp_set_nullptr(udp);
		}
		// Free udp send buffer.
		for (size_t i = 0; i < udpSendInfo->num; ++i)
			pool->getMemoryTrace()._free_set_nullptr(udpSendInfo->buf[i].base);
		pool->getMemoryTrace()._free_set_nullptr(udpSendInfo);
	}

	static Cudp *bindAndListenUdp(CnetworkPool *pool, uv_loop_t *loop, const CnetworkNode& node)
	{
		if (node.getProtocol() != CnetworkNode::protocol_udp)
			return nullptr;
		Cudp *server = Cudp::alloc(pool, loop);
		if (nullptr == server)
		{
			// Insufficient memory.
			NP_FPRINTF((stderr, "Bind and listen udp error with insufficient memory.\n"));
			return nullptr;
		}
		server->getNode() = node;
		on_error_goto_ec(
			uv_udp_bind(server->getUdp(), server->getNode().getSockaddr().getSockaddr(), 0),
			(stderr, "Bind and listen udp bind error.\n"));
		on_error_goto_ec(
			uv_udp_recv_start(server->getUdp(), udp_alloc_buffer, on_udp_recv),
			(stderr, "Bind and listen udp listen error.\n"));
		return server;
	_ec:
		Cudp::close_set_nullptr(server);
		return nullptr;
	}

	void on_wakeup(uv_async_t *async)
	{
		CnetworkPool *pool = Casync::obtain(async)->getPool();
		// Copy pending to local first.
		pool->m_lock.lock(); // Just use lock and unlock, because we never get exception here(fatal error).
		std::deque<std::pair<CnetworkNode, bool>> bindCopy(std::move(pool->m_pendingBind));
		std::deque<CnetworkPool::__pending_send> sendCopy(std::move(pool->m_pendingSend));
		std::deque<std::pair<CnetworkNode, bool>> closeCopy(std::move(pool->m_pendingClose));
		pool->m_pendingBind.clear();
		pool->m_pendingSend.clear();
		pool->m_pendingClose.clear();
		pool->m_lock.unlock();
		// Deal with request(s).
		if (pool->m_bWantExit)
		{
			//
			// Stop and free all resources.
			//
			// Async.
			Casync::close_set_nullptr(pool->m_wakeup);
			// TCP servers.
			std::unordered_map<CnetworkNode, Ctcp *, __network_hash> tmpTcpServers(std::move(pool->m_tcpServers));
			pool->m_tcpServers.clear();
			for (auto& pair : tmpTcpServers)
			{
				// Report bind down.
				pool->m_callback.bindStatus(pair.first, false);
				// Close.
				Ctcp::close_set_nullptr(pair.second);
			}
			tmpTcpServers.clear();
			// UDP servers.
			std::vector<Cudp *> tmpUdpServers(std::move(pool->m_udpServers));
			pool->m_udpServers.clear();
			for (auto& server : tmpUdpServers)
			{
				// Report bind down.
				pool->m_callback.bindStatus(server->getNode(), false);
				// Close.
				Cudp *tmp = server;
				uv_udp_recv_stop(tmp->getUdp()); // Ignore the result.
				Cudp::close_set_nullptr(tmp);
			}
			tmpUdpServers.clear();
			// TCP connections.
			std::unordered_map<CnetworkNode, Ctcp *, __network_hash> tmpNode2stream(std::move(pool->m_node2stream));
			pool->m_node2stream.clear();
			for (auto& pair : tmpNode2stream)
			{
				// Report connection down.
				pool->m_callback.connectionStatus(pair.second->getNode(), false);
				// Close.
				Ctcp::close_set_nullptr(pair.second);
			}
			tmpNode2stream.clear();
			// TCP connecting.
			std::unordered_set<Ctcp *> tmpConnecting(std::move(pool->m_connecting));
			pool->m_connecting.clear();
			for (auto& connect : tmpConnecting)
			{
				// Report connection down.
				pool->m_callback.connectionStatus(connect->getNode(), false);
				// Close.
				Ctcp *tmp = connect;
				Ctcp::close_set_nullptr(tmp);
			}
			tmpConnecting.clear();
			// Drop all waiting message.
			for (auto& pair : pool->m_waitingSend)
			{
				const CnetworkNode& node = pair.first;
				for (auto& buf : pair.second)
				{
					pool->m_callback.drop(node, buf.base, buf.len);
					pool->getMemoryTrace()._free_set_nullptr(buf.base);
				}
			}
			pool->m_waitingSend.clear();
			// Drop all pending bind & message.
			for (const auto& pair : bindCopy)
				pool->m_callback.bindStatus(pair.first, false);
			for (const auto& req : sendCopy)
				pool->m_callback.drop(req.m_node, req.m_data.getData(), req.m_data.getLength());
		}
		else
		{
			//
			// Bind, send & close.
			//
			// Bind.
			for (const auto& pair : bindCopy)
			{
				const CnetworkNode& node = pair.first;
				const bool& bBind = pair.second;
				switch (node.getProtocol())
				{
				case CnetworkNode::protocol_tcp:
				{
					auto it = pool->m_tcpServers.find(node);
					if (it != pool->m_tcpServers.end())
					{
						if (bBind)
							pool->m_callback.bindStatus(node, true);
						else
						{
							// Unbind.
							Ctcp *tcp = it->second;
							pool->m_tcpServers.erase(it);
							pool->m_callback.bindStatus(node, false);
							Ctcp::close_set_nullptr(tcp);
						}
					}
					else
					{
						if (bBind)
						{
							// Bind.
							Ctcp *tcpServer = bindAndListenTcp(pool, &pool->m_loop, node);
							if (tcpServer != nullptr)
								pool->m_tcpServers.insert(std::make_pair(node, tcpServer));
							pool->m_callback.bindStatus(node, tcpServer != nullptr);
						}
						else
							pool->m_callback.bindStatus(node, false);
					}
				}
					break;
				case CnetworkNode::protocol_udp:
				{
					auto udpServerIt = pool->m_udpServers.begin();
					for (; udpServerIt != pool->m_udpServers.end(); ++udpServerIt)
					{
						if ((*udpServerIt)->getNode() == node)
							break;
					}
					if (udpServerIt != pool->m_udpServers.end())
					{
						// Found.
						if (bBind)
							pool->m_callback.bindStatus(node, true);
						else
						{
							// Unbind.
							Cudp *udp = *udpServerIt;
							pool->m_udpServers.erase(udpServerIt);
							pool->m_callback.bindStatus(node, false);
							uv_udp_recv_stop(udp->getUdp()); // Ignore the result.
							Cudp::close_set_nullptr(udp);
						}
					}
					else
					{
						// Not found.
						if (bBind)
						{
							// Bind.
							Cudp *udpServer = bindAndListenUdp(pool, &pool->m_loop, node);
							if (udpServer != nullptr)
								pool->m_udpServers.push_back(udpServer);
							pool->m_callback.bindStatus(node, udpServer != nullptr);
						}
						else
							pool->m_callback.bindStatus(node, false);
					}
				}
				break;

				default:
					pool->m_callback.bindStatus(node, false);
					break;
				}
			}
			// Send.
			for (auto& req : sendCopy)
			{
				const CnetworkNode& node = req.m_node;
				Cbuffer& data = req.m_data;
				const bool& bAutoConnect = req.m_bAutoConnect;
				switch (node.getProtocol())
				{
				case CnetworkNode::protocol_tcp:
				{
					Ctcp *tcp = pool->getStreamByNode(node);
					if (nullptr == tcp)
					{
						// Check if any waiting data, start connect if no waiting.
						bool bNeedConnect = pool->m_waitingSend.find(node) == pool->m_waitingSend.end();
						if (bNeedConnect && !bAutoConnect)
							pool->m_callback.drop(node, data.getData(), data.getLength()); // Just drop.
						else
						{
							pool->pushWaiting(node, data);
							if (bNeedConnect)
							{
								tcp = connectTcp(pool, &pool->m_loop, node);
								if (nullptr == tcp)
								{
									// Connect fail.
									pool->m_callback.connectionStatus(node, false);
									pool->dropWaiting(node);
								}
								else // Put in connecting set.
									pool->m_connecting.insert(tcp);
							}
						}
					}
					else
					{
						// Just use write.
						CnetworkPool::__write_with_info *writeInfo = (CnetworkPool::__write_with_info *)pool->getMemoryTrace()._malloc_no_throw(sizeof(CnetworkPool::__write_with_info)); // Only one buf.
						if (nullptr == writeInfo)
						{
							// Insufficient memory.
							// Just drop.
							NP_FPRINTF((stderr, "Send tcp error with insufficient memory.\n"));
							pool->m_callback.drop(node, data.getData(), data.getLength());
						}
						else
						{
							writeInfo->num = 1;
							data.transfer(writeInfo->buf[0]);
							// First reset timer and then send.
							if (uv_timer_start(tcp->getTimer(), on_tcp_timeout, pool->getSettings().tcp_send_timeout_in_seconds * 1000, 0) != 0 ||
								uv_write(&writeInfo->write, tcp->getStream(), writeInfo->buf, (unsigned int)writeInfo->num, on_tcp_write_done) != 0)
							{
								pool->dropWriteAndFree_set_nullptr(node, writeInfo);
								// Shutdown connection.
								pool->shutdownTcpConnection_set_nullptr(tcp);
							}
						}
					}
				}
				break;

				case CnetworkNode::protocol_udp:
				{
					if (pool->m_udpServers.size() > 0)
					{
						CnetworkPool::__udp_send_with_info *udpSendInfo = (CnetworkPool::__udp_send_with_info *)pool->getMemoryTrace()._malloc_no_throw(sizeof(CnetworkPool::__udp_send_with_info)); // Only one buf.
						if (udpSendInfo != nullptr)
						{
							udpSendInfo->num = 1;
							data.transfer(udpSendInfo->buf[0]);
							pool->m_udpIndex %= pool->m_udpServers.size();
							size_t selIndex = pool->m_udpIndex;
							++pool->m_udpIndex;
							Cudp *sender = pool->m_udpServers[selIndex];
							if (uv_udp_send(&udpSendInfo->udpSend, sender->getUdp(), udpSendInfo->buf, (unsigned int)udpSendInfo->num, node.getSockaddr().getSockaddr(), on_udp_send_done) != 0)
							{
								// Free udp send buffer.
								for (size_t i = 0; i < udpSendInfo->num; ++i)
									pool->getMemoryTrace()._free_set_nullptr(udpSendInfo->buf[i].base);
								pool->getMemoryTrace()._free_set_nullptr(udpSendInfo);
								// Stop and close server.
								pool->m_udpServers.erase(pool->m_udpServers.begin() + selIndex);
								pool->m_callback.bindStatus(node, false);
								uv_udp_recv_stop(sender->getUdp()); // Ignore the result.
								Cudp::close_set_nullptr(sender);
							}
						}
					} // Ignore the fail, and udp don't send drop.
				}
				break;

				default:
					// Unknown protocol.
					if (bAutoConnect)
						pool->m_callback.connectionStatus(node, false);
					pool->m_callback.drop(node, data.getData(), data.getLength());
					break;
				}
			}
			// Close.
			for (const auto& pair : closeCopy)
			{
				const CnetworkNode& node = pair.first;
				const bool& bForceClose = pair.second;
				Ctcp *tcp = pool->getStreamByNode(node); // Tcp connections(checked before insert).
				if (tcp != nullptr)
				{
					// No force close means shutdown, and it's a type of send.
					if (!bForceClose && uv_timer_start(tcp->getTimer(), on_tcp_timeout, pool->getSettings().tcp_send_timeout_in_seconds * 1000, 0) != 0)
						pool->shutdownTcpConnection_set_nullptr(tcp);
					else // Timer still working until close, so timeout when shutdown will force close the connection.
						pool->shutdownTcpConnection_set_nullptr(tcp, false, !bForceClose);
				}
			}
		}
	}

	inline Ctcp *CnetworkPool::getStreamByNode(const CnetworkNode& node)
	{
		auto it = m_node2stream.find(node);
		if (it == m_node2stream.end())
			return nullptr;
		return it->second;
	}

	inline void CnetworkPool::dropWaiting(const CnetworkNode& node)
	{
		auto waitingIt = m_waitingSend.find(node);
		if (waitingIt != m_waitingSend.end())
		{
			for (auto& buf : waitingIt->second)
			{
				m_callback.drop(node, buf.base, buf.len);
				m_memoryTrace._free_set_nullptr(buf.base);
			}
			m_waitingSend.erase(waitingIt);
		}
	}

	inline void CnetworkPool::pushWaiting(const CnetworkNode& node, Cbuffer& data)
	{
		uv_buf_t buf;
		data.transfer(buf);
		auto it = m_waitingSend.find(node);
		if (it == m_waitingSend.end())
			m_waitingSend.insert(std::make_pair(node, std::vector<uv_buf_t>(1, buf)));
		else
			it->second.push_back(buf);
	}

	inline void CnetworkPool::dropWriteAndFree_set_nullptr(const CnetworkNode& node, __write_with_info *& writeInfo)
	{
		// Notify message drop and delete it.
		for (size_t i = 0; i < writeInfo->num; ++i)
		{
			m_callback.drop(node, writeInfo->buf[i].base, writeInfo->buf[i].len);
			m_memoryTrace._free_set_nullptr(writeInfo->buf[i].base);
		}
		m_memoryTrace._free_set_nullptr(writeInfo);
	}

	inline CnetworkPool::__write_with_info *CnetworkPool::getWriteFromWaitingByNode(const CnetworkNode& node)
	{
		auto it = m_waitingSend.find(node);
		if (it == m_waitingSend.end())
			return nullptr;
		__write_with_info *writeInfo = (__write_with_info *)m_memoryTrace._malloc_no_throw(sizeof(__write_with_info) + sizeof(uv_buf_t)*(it->second.size() - 1));
		if (nullptr == writeInfo)
		{
			// Insufficient memory.
			// Just drop.
			NP_FPRINTF((stderr, "Send tcp error with insufficient memory.\n"));
			dropWaiting(node);
			return nullptr;
		}
		writeInfo->num = it->second.size();
		for (size_t i = 0; i < writeInfo->num; ++i)
			writeInfo->buf[i] = it->second[i];
		m_waitingSend.erase(it);
		return writeInfo;
	}

	inline void CnetworkPool::startupTcpConnection_may_set_nullptr(Ctcp *& tcp)
	{
		if (!tcp->getNode().getSockaddr().valid())
		{
			// WTF to get here? The only thing we can do is just close it.
			NP_FPRINTF((stderr, "Fatal error startup a connection whithout node.\n"));
			Ctcp::close_set_nullptr(tcp);
			return;
		}
		// Add map.
		auto ib = m_node2stream.insert(std::make_pair(tcp->getNode(), tcp));
		if (!ib.second)
		{
			// Remote port reuse?
			// If a connection is startup, no data will be written to waiting queue, so just reject.
			NP_FPRINTF((stderr, "Error startup a connection with remote port reuse.\n"));
			// Close connection.
			Ctcp::close_set_nullptr(tcp);
			return;
		}
		// Report new connection.
		m_callback.connectionStatus(tcp->getNode(), true);
		// Send message waiting.
		__write_with_info *writeInfo = getWriteFromWaitingByNode(tcp->getNode());
		if (writeInfo != nullptr)
		{
			// Something need to send. First reset timer and then send.
			if (uv_timer_start(tcp->getTimer(), on_tcp_timeout, m_settings.tcp_send_timeout_in_seconds * 1000, 0) != 0 ||
				uv_write(&writeInfo->write, tcp->getStream(), writeInfo->buf, (unsigned int)writeInfo->num, on_tcp_write_done) != 0)
			{
				dropWriteAndFree_set_nullptr(tcp->getNode(), writeInfo);
				// Shutdown connection.
				shutdownTcpConnection_set_nullptr(tcp);
			}
		}
	}

	// This function is idempotent, and can be called any time when tcp is valid(closing is also ok).
	inline void CnetworkPool::shutdownTcpConnection_set_nullptr(Ctcp *& tcp, bool bAlwaysNotify, bool bShutdown)
	{
		// Clean map.
		auto sz = m_node2stream.erase(tcp->getNode());
		if (sz > 0 || bAlwaysNotify)
			m_callback.connectionStatus(tcp->getNode(), false); // Report connection down.
		// Drop message notify.
		dropWaiting(tcp->getNode());
		// Close connection.
		if (bShutdown)
			Ctcp::shutdown_and_close_set_nullptr(tcp);
		else
			Ctcp::close_set_nullptr(tcp);
	}

	inline void CnetworkPool::stopAndCloseUdp_set_nullptr(Cudp *& udp)
	{
		// Find in vector.
		bool bFound = false;
		for (auto it = m_udpServers.begin(); it != m_udpServers.end(); ++it)
		{
			if (*it == udp)
			{
				m_udpServers.erase(it);
				bFound = true;
				break;
			}
		}
		if (bFound)
			m_callback.bindStatus(udp->getNode(), false); // Notify.
		uv_udp_recv_stop(udp->getUdp()); // Ignore the result.
		Cudp::close_set_nullptr(udp);
	}

	void CnetworkPool::internalThread()
	{
		// Init loop.
		if (uv_loop_init(&m_loop) != 0)
		{
			m_state = bad;
			return;
		}
		m_wakeup = Casync::alloc(this, &m_loop, on_wakeup);
		if (nullptr == m_wakeup)
		{
			uv_loop_close(&m_loop);
			m_state = bad;
			return;
		}
		m_state = good;
		uv_run(&m_loop, UV_RUN_DEFAULT);
		uv_loop_close(&m_loop);
	}
}
