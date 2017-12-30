
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
	#ifdef _WIN32
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

	// This function should be called at last and the tcp ***MUST*** be no closing.
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
			if (!tcp->isClosing())
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
		else if (!tcp->isClosing())
		{
			if (0 == tcp->getStream()->write_queue_size && pool->m_waitingClose.find(tcp) != pool->m_waitingClose.end())
				pool->shutdownTcpConnection_set_nullptr(tcp);
			else
				reset_tcp_idle_timeout_may_set_nullptr(tcp);
		}
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
		char ip[64];
		switch (peer.ss_family)
		{
		case AF_INET:
			on_error_goto_ec(
				uv_ip4_name((sockaddr_in *)&peer, ip, sizeof(ip)),
				(stderr, "New incoming connection tcp ipv4 name error.\n"));
			clientTcp->getNode().set(CnetworkNode::protocol_tcp, ip, ntohs(((sockaddr_in *)&peer)->sin_port));
			break;
		case AF_INET6:
			on_error_goto_ec(
				uv_ip6_name((sockaddr_in6 *)&peer, ip, sizeof(ip)),
				(stderr, "New incoming connection tcp ipv6 name error.\n"));
			clientTcp->getNode().set(CnetworkNode::protocol_tcp, ip, ntohs(((sockaddr_in6 *)&peer)->sin6_port));
			break;
		default:
			goto_ec((stderr, "New incoming connection tcp unknown peer ss_family.\n"));
		}
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
		if (status < 0 || tcp->isClosing())
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
		server->getNode().set(node);
		if (node.isIPv6())
		{
			sockaddr_in6 addr;
			on_error_goto_ec(
				uv_ip6_addr(node.getIp().c_str(), node.getPort(), &addr),
				(stderr, "Bind and listen tcp ipv6 addr error.\n"));
			on_error_goto_ec(
				uv_tcp_bind(server->getTcp(), (const sockaddr *)&addr, 0),
				(stderr, "Bind and listen tcp bind error.\n"));
		}
		else
		{
			sockaddr_in addr;
			on_error_goto_ec(
				uv_ip4_addr(node.getIp().c_str(), node.getPort(), &addr),
				(stderr, "Bind and listen tcp ipv4 addr error.\n"));
			on_error_goto_ec(
				uv_tcp_bind(server->getTcp(), (const sockaddr *)&addr, 0),
				(stderr, "Bind and listen tcp bind error.\n"));
		}
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
		tcp->getNode().set(node);
		// Set timeout.
		on_error_goto_ec(
			uv_timer_start(tcp->getTimer(), on_tcp_timeout, pool->getSettings().tcp_connect_timeout_in_seconds * 1000, 0),
			(stderr, "Connect tcp timer start error.\n"));
		// Connect.
		if (node.isIPv6())
		{
			sockaddr_in6 addr;
			on_error_goto_ec(
				uv_ip6_addr(node.getIp().c_str(), node.getPort(), &addr),
				(stderr, "Connect tcp ipv6 addr error.\n"));
			on_error_goto_ec(
				uv_tcp_connect(connect, tcp->getTcp(), (const sockaddr *)&addr, on_connect_done),
				(stderr, "Connect tcp connect error.\n"));
		}
		else
		{
			sockaddr_in addr;
			on_error_goto_ec(
				uv_ip4_addr(node.getIp().c_str(), node.getPort(), &addr),
				(stderr, "Connect tcp ipv4 addr error.\n"));
			on_error_goto_ec(
				uv_tcp_connect(connect, tcp->getTcp(), (const sockaddr *)&addr, on_connect_done),
				(stderr, "Connect tcp connect error.\n"));
		}
		return tcp;
	_ec:
		pool->getMemoryTrace()._free_set_nullptr(connect);
		Ctcp::close_set_nullptr(tcp);
		return nullptr;
	}

	void on_wakeup(uv_async_t *async)
	{
		CnetworkPool *pool = Casync::obtain(async)->getPool();
		// Copy pending to local first.
		std::unordered_map<CnetworkNode, bool, __network_hash> bindCopy;
		std::list<CnetworkPool::__pending_send> sendCopy;
		std::unordered_map<CnetworkNode, bool, __network_hash> closeCopy;
		pool->m_lock.lock();
		bindCopy = std::move(pool->m_pendingBind);
		sendCopy = std::move(pool->m_pendingSend);
		closeCopy = std::move(pool->m_pendingClose);
		pool->m_pendingBind.clear();
		pool->m_pendingSend.clear();
		pool->m_pendingClose.clear();
		pool->m_lock.unlock();
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
			// Clear close flag.
			pool->m_waitingClose.clear();
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
					// Todo. Code here to implement UDP server.

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
					// Todo. Code here to implement UDP send.

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
				Ctcp *tcp = pool->getStreamByNode(node);
				if (tcp != nullptr)
				{
					if (bForceClose || 0 == tcp->getStream()->write_queue_size)
						pool->shutdownTcpConnection_set_nullptr(tcp);
					else
						pool->m_waitingClose.insert(tcp);
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
		{
			std::vector<uv_buf_t> vec(1, buf);
			m_waitingSend.insert(std::make_pair(node, vec));
		}
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
		CnetworkPool::__write_with_info *writeInfo = (CnetworkPool::__write_with_info *)m_memoryTrace._malloc_no_throw(sizeof(CnetworkPool::__write_with_info) + sizeof(uv_buf_t)*(it->second.size() - 1));
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
		if (tcp->getNode().getIp().empty())
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
	inline void CnetworkPool::shutdownTcpConnection_set_nullptr(Ctcp *& tcp, bool bAlwaysNotify)
	{
		// Clean flag set & map.
		m_waitingClose.erase(tcp);
		auto sz = m_node2stream.erase(tcp->getNode());
		if (sz > 0 || bAlwaysNotify)
			m_callback.connectionStatus(tcp->getNode(), false); // Report connection down.
		// Drop message notify.
		dropWaiting(tcp->getNode());
		// Close connection.
		Ctcp::close_set_nullptr(tcp);
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
