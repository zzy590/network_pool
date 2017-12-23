
#include <atomic>
#include <cstring>

#include "network_pool.h"

#define DBG 0

#if DBG
	#include <stdio.h>
	#define _FPRINTF(_x) { fprintf _x; }
#else
	#define _FPRINTF(_x) {}
#endif

#define TCP_KEEPALIVE_DELAY 30 // 30s
#define TCP_DEFAULT_BACKLOG 128

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

namespace NETWORK_POOL
{
	//
	// Resource logger.
	//

	static std::atomic<int64_t> s_memorySize(0);
	static std::atomic<int32_t> s_memoryCount(0);
	static std::atomic<int32_t> s_objectCount(0);

	// Throw(-1) when fail and throwOnFail.
	static void *my_malloc(size_t sz, bool throwOnFail = true)
	{
		if (sz > 0x40000000) // Alloc more than 1GB is not allowed.
		{
			_FPRINTF((stderr, "malloc denied 0x%x(%u) error too large\n", sz, sz));
			if (throwOnFail)
				throw(-1);
			return nullptr;
		}
		void *ptr = malloc(sizeof(uint32_t) + sz);
		if (nullptr == ptr)
		{
			_FPRINTF((stderr, "malloc failed to alloc 0x%x(%u)\n", sz, sz));
			if (throwOnFail)
				throw(-1);
			return nullptr;
		}
		*(uint32_t *)ptr = (uint32_t)sz;
		s_memorySize += sz;
		++s_memoryCount;
	#if DBG
		memset((uint32_t *)ptr + 1, -1, sz);
	#endif
		return (uint32_t *)ptr + 1;
	}

	static void my_free(void *ptr)
	{
		if (nullptr == ptr)
			return;
		void *org = (uint32_t *)ptr - 1;
		uint32_t sz = *(uint32_t *)org;
		if (sz > 0x40000000)
		{
			_FPRINTF((stderr, "malloc trace bad data 0x%x(%u)\n", sz, sz));
			throw(-1);
		}
		else
		{
			s_memorySize -= sz;
			--s_memoryCount;
		}
	#if DBG
		memset(org, -1, sizeof(uint32_t) + sz);
	#endif
		free(org);
	}

	void getMemoryTrace(int64_t& memTotal, int32_t& memCnt, int32_t& objCnt)
	{
		memTotal = s_memorySize;
		memCnt = s_memoryCount;
		objCnt = s_objectCount;
	}

	#define malloc my_malloc
	#define free my_free
	#define obj_add { ++s_objectCount; }
	#define obj_sub { --s_objectCount; }

	//
	// Cbuffer
	//

	inline void Cbuffer::transfer(uv_buf_t& buf)
	{
		buf.base = (char *)m_data;
	#ifdef _WIN32
		buf.len = (ULONG)m_length;
	#else
		buf.len = m_length;
	#endif
		m_data = nullptr;
		m_maxLength = m_length = 0;
	}

	Cbuffer::Cbuffer()
	{
		m_data = nullptr;
		m_maxLength = m_length = 0;
	}

	Cbuffer::Cbuffer(const size_t length)
	{
		if (0 == length)
		{
			m_data = nullptr;
			m_maxLength = m_length = 0;
		}
		else
		{
			m_data = malloc(length);
			m_maxLength = m_length = length;
		}
	}

	Cbuffer::Cbuffer(const void *data, const size_t length)
	{
		m_data = malloc(length);
		memcpy(m_data, data, length);
		m_maxLength = m_length = length;
	}

	Cbuffer::Cbuffer(const Cbuffer& another)
	{
		m_data = malloc(another.m_length);
		memcpy(m_data, another.m_data, another.m_length);
		m_maxLength = m_length = another.m_length;
	}

	Cbuffer::~Cbuffer()
	{
		if (m_data != nullptr)
			free(m_data);
	}

	const Cbuffer& Cbuffer::operator=(const Cbuffer& another)
	{
		if (another.m_length <= m_maxLength)
		{
			memcpy(m_data, another.m_data, another.m_length);
			m_length = another.m_length;
		}
		else
		{
			if (m_data != nullptr)
				free(m_data);
			m_data = malloc(another.m_length);
			memcpy(m_data, another.m_data, another.m_length);
			m_maxLength = m_length = another.m_length;
		}
		return *this;
	}

	void Cbuffer::set(const void *data, const size_t length)
	{
		if (length <= m_maxLength)
		{
			memcpy(m_data, data, length);
			m_length = length;
		}
		else
		{
			if (m_data != nullptr)
				free(m_data);
			m_data = malloc(length);
			memcpy(m_data, data, length);
			m_maxLength = m_length = length;
		}
	}

	void Cbuffer::resize(const size_t preferLength, const size_t validLength)
	{
		if (preferLength <= m_maxLength)
		{
			m_length = preferLength;
			return;
		}
		size_t copy = validLength > m_length ? m_length : validLength;
		if (copy > 0)
		{
			void *newBuffer = malloc(preferLength);
			memcpy(newBuffer, m_data, copy);
			free(m_data);
			m_data = newBuffer;
			m_maxLength = m_length = preferLength;
		}
		else
		{
			if (m_data != nullptr)
				free(m_data);
			m_data = malloc(preferLength);
			m_maxLength = m_length = preferLength;
		}
	}

	//
	// CnetworkPool
	//

	static void on_close_no_free(uv_handle_t *handle)
	{
	}

	static void on_close_free_handle(uv_handle_t *handle)
	{
		free(handle);
	}

	static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
	{
		buf->base = (char *)malloc(suggested_size);
	#ifdef _WIN32
		buf->len = (ULONG)suggested_size;
	#else
		buf->len = suggested_size;
	#endif
	}

	void on_tcp_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf)
	{
		__tcp_with_info *clientInfo = container_of(client, __tcp_with_info, tcp);
		if (nread > 0)
		{
			// Report message.
			if (clientInfo->node != nullptr)
				clientInfo->pool->m_callback.message(*clientInfo->node, buf->base, nread);
		}
		else if (nread < 0)
		{
			if (nread != UV_EOF)
				_FPRINTF((stderr, "Read error %s\n", uv_err_name((int)nread)));
			// Shutdown connection.
			clientInfo->pool->shutdownTcpConnection(clientInfo);
		}
		free(buf->base);
	}

	void on_tcp_write_done(uv_write_t *req, int status)
	{
		__write_with_info *writeInfo = container_of(req, __write_with_info, write);
		if (status != 0)
		{
			_FPRINTF((stderr, "Tcp write error %s\n", uv_strerror(status)));
			// Drop message notify.
			__tcp_with_info *tcpInfo = container_of(req->handle, __tcp_with_info, tcp);
			if (tcpInfo->node != nullptr)
			{
				const CnetworkNode& node = *tcpInfo->node;
				for (size_t i = 0; i < writeInfo->num; ++i)
					tcpInfo->pool->m_callback.drop(node, writeInfo->buf[i].base, writeInfo->buf[i].len);
			}
			// Shutdown connection.
			tcpInfo->pool->shutdownTcpConnection(tcpInfo);
		}
		for (size_t i = 0; i < writeInfo->num; ++i)
			free(writeInfo->buf[i].base);
		free(writeInfo);
	}

	void on_new_connection(uv_stream_t *server, int status)
	{
		if (status != 0)
		{
			// WTF? Listen fail?
			_FPRINTF((stderr, "New connection error %s\n", uv_strerror(status)));
			// Report this error and close the server.
			__tcp_with_info *serverInfo = container_of(server, __tcp_with_info, tcp);
			if (serverInfo->node != nullptr)
			{
				// Clean set.
				serverInfo->pool->m_tcpServers.erase(serverInfo);
				// Report bind down.
				serverInfo->pool->m_callback.bindStatus(*serverInfo->node, false);
				// Delete node data.
				delete serverInfo->node;
				serverInfo->node = nullptr;
				obj_sub;
			}
			uv_close((uv_handle_t *)serverInfo, on_close_free_handle);
			return;
		}
		// Prepare for the new connection.
		__tcp_with_info *client = (__tcp_with_info *)malloc(sizeof(__tcp_with_info), false);
		if (nullptr == client)
		{
			// Insufficient memory.
			// Just ignore.
			_FPRINTF((stderr, "New connection error with insufficient memory.\n"));
			return;
		}
		CnetworkPool *pool = container_of(server, __tcp_with_info, tcp)->pool;
		client->pool = pool;
		client->node = nullptr;
		if (uv_tcp_init(&pool->m_loop, &client->tcp) != 0)
		{
			free(client);
			_FPRINTF((stderr, "New connection TCP init error\n"));
			return;
		}
		if (0 == uv_accept(server, (uv_stream_t *)client))
		{
			sockaddr_storage peer;
			int len = sizeof(peer);
			if (0 == uv_tcp_getpeername(&client->tcp, (sockaddr *)&peer, &len))
			{
				char ip[64];
				switch (peer.ss_family)
				{
				case AF_INET:
					if (0 == uv_ip4_name((sockaddr_in *)&peer, ip, sizeof(ip)))
					{
						client->node = new CnetworkNode(CnetworkNode::protocol_tcp, ip, ntohs(((sockaddr_in *)&peer)->sin_port));
						obj_add;
					}
					else
					{
						_FPRINTF((stderr, "New connection IPv4 name error\n"));
						goto _ec;
					}
					break;
				case AF_INET6:
					if (0 == uv_ip6_name((sockaddr_in6 *)&peer, ip, sizeof(ip)))
					{
						client->node = new CnetworkNode(CnetworkNode::protocol_tcp, ip, ntohs(((sockaddr_in6 *)&peer)->sin6_port));
						obj_add;
					}
					else
					{
						_FPRINTF((stderr, "New connection IPv6 name error\n"));
						goto _ec;
					}
					break;
				default:
					_FPRINTF((stderr, "New connection unknown peer ss_family\n"));
					goto _ec;
				}
			}
			else
			{
				_FPRINTF((stderr, "New connection TCP getpeername error\n"));
				goto _ec;
			}
			// Do the port reuse check.
			if (pool->getStreamByNode(*client->node) != nullptr)
				goto _ec;
			if (uv_read_start((uv_stream_t *)client, alloc_buffer, on_tcp_read) != 0)
			{
				_FPRINTF((stderr, "New connection TCP read start error\n"));
				goto _ec;
			}
			// Startup connection.
			pool->startupTcpConnection(client);
			return;
		}
	_ec:
		if (client->node != nullptr)
		{
			delete client->node;
			client->node = nullptr;
			obj_sub;
		}
		uv_close((uv_handle_t *)client, on_close_free_handle);
	}

	void on_connect_done(uv_connect_t *req, int status)
	{
		__tcp_with_info *tcpInfo = container_of(req->handle, __tcp_with_info, tcp);
		// Remove from connecting and free request.
		tcpInfo->pool->m_connecting.erase(tcpInfo);
		free(req);
		// Error?
		if (status < 0)
		{
			_FPRINTF((stderr, "Connect TCP error %s\n", uv_strerror(status)));
			goto _ec;
		}
		// Prepare for read.
		if (uv_read_start((uv_stream_t *)tcpInfo, alloc_buffer, on_tcp_read) != 0)
		{
			_FPRINTF((stderr, "Connect TCP read start error\n"));
			goto _ec;
		}
		// Startup connection.
		tcpInfo->pool->startupTcpConnection(tcpInfo);
		return;
	_ec:
		// Shutdown connection.
		tcpInfo->pool->shutdownTcpConnection(tcpInfo);
	}

	static inline bool setTcp(uv_tcp_t *tcp)
	{
		if (uv_tcp_nodelay(tcp, 1) != 0)
		{
			_FPRINTF((stderr, "Error TCP nodelay.\n"));
			return false;
		}
		if (uv_tcp_keepalive(tcp, 1, TCP_KEEPALIVE_DELAY) != 0)
		{
			_FPRINTF((stderr, "Error TCP keepalive.\n"));
			return false;
		}
		if (uv_tcp_simultaneous_accepts(tcp, 1) != 0)
		{
			_FPRINTF((stderr, "Error TCP simultaneous accepts.\n"));
			return false;
		}
		return true;
	}

	static __tcp_with_info *bindAndListenTcp(CnetworkPool *pool, uv_loop_t *loop, const CnetworkNode& node)
	{
		if (node.getProtocol() != CnetworkNode::protocol_tcp)
			return nullptr;
		__tcp_with_info *server = (__tcp_with_info *)malloc(sizeof(__tcp_with_info), false);
		if (nullptr == server)
		{
			// Insufficient memory.
			_FPRINTF((stderr, "Bind and listen TCP error with insufficient memory.\n"));
			return nullptr;
		}
		server->pool = pool;
		server->node = nullptr;
		if (uv_tcp_init(loop, &server->tcp) != 0)
		{
			_FPRINTF((stderr, "Error TCP init.\n"));
			free(server);
			return nullptr;
		}
		server->node = new CnetworkNode(node);
		obj_add;
		if (!setTcp(&server->tcp))
			goto _ec;
		if (node.isIPv6())
		{
			sockaddr_in6 addr;
			if (uv_ip6_addr(node.getIp().c_str(), node.getPort(), &addr) != 0)
			{
				_FPRINTF((stderr, "Error IPv6 addr.\n"));
				goto _ec;
			}
			if (uv_tcp_bind(&server->tcp, (const sockaddr *)&addr, 0) != 0)
			{
				_FPRINTF((stderr, "Error TCP bind.\n"));
				goto _ec;
			}
		}
		else
		{
			sockaddr_in addr;
			if (uv_ip4_addr(node.getIp().c_str(), node.getPort(), &addr) != 0)
			{
				_FPRINTF((stderr, "Error IPv4 addr.\n"));
				goto _ec;
			}
			if (uv_tcp_bind(&server->tcp, (const sockaddr *)&addr, 0) != 0)
			{
				_FPRINTF((stderr, "Error TCP bind.\n"));
				goto _ec;
			}
		}
		if (uv_listen((uv_stream_t *)server, TCP_DEFAULT_BACKLOG, on_new_connection) != 0)
		{
			_FPRINTF((stderr, "Error TCP listen.\n"));
			goto _ec;
		}
		return server;
	_ec:
		if (server->node != nullptr)
		{
			delete server->node;
			server->node = nullptr;
			obj_sub;
		}
		uv_close((uv_handle_t *)server, on_close_free_handle);
		return nullptr;
	}

	static __tcp_with_info *connectTcp(CnetworkPool *pool, uv_loop_t *loop, const CnetworkNode& node)
	{
		if (node.getProtocol() != CnetworkNode::protocol_tcp)
			return nullptr;
		uv_connect_t *connect = (uv_connect_t *)malloc(sizeof(uv_connect_t), false);
		if (nullptr == connect)
		{
			// Insufficient memory.
			_FPRINTF((stderr, "Connect TCP error with insufficient memory.\n"));
			return nullptr;
		}
		__tcp_with_info *tcpInfo = (__tcp_with_info *)malloc(sizeof(__tcp_with_info), false);
		if (nullptr == tcpInfo)
		{
			// Insufficient memory.
			_FPRINTF((stderr, "Connect TCP error with insufficient memory.\n"));
			free(connect);
			return nullptr;
		}
		tcpInfo->pool = pool;
		tcpInfo->node = nullptr;
		if (uv_tcp_init(loop, &tcpInfo->tcp) != 0)
		{
			_FPRINTF((stderr, "Error TCP init.\n"));
			free(tcpInfo);
			free(connect);
			return nullptr;
		}
		tcpInfo->node = new CnetworkNode(node);
		obj_add;
		if (!setTcp(&tcpInfo->tcp))
			goto _ec;
		if (node.isIPv6())
		{
			sockaddr_in6 addr;
			if (uv_ip6_addr(node.getIp().c_str(), node.getPort(), &addr) != 0)
			{
				_FPRINTF((stderr, "Error IPv6 addr.\n"));
				goto _ec;
			}
			if (uv_tcp_connect(connect, &tcpInfo->tcp, (const sockaddr *)&addr, on_connect_done) != 0)
			{
				_FPRINTF((stderr, "Error TCP connect.\n"));
				goto _ec;
			}
		}
		else
		{
			sockaddr_in addr;
			if (uv_ip4_addr(node.getIp().c_str(), node.getPort(), &addr) != 0)
			{
				_FPRINTF((stderr, "Error IPv4 addr.\n"));
				goto _ec;
			}
			if (uv_tcp_connect(connect, &tcpInfo->tcp, (const sockaddr *)&addr, on_connect_done) != 0)
			{
				_FPRINTF((stderr, "Error TCP connect.\n"));
				goto _ec;
			}
		}
		return tcpInfo;
	_ec:
		if (tcpInfo->node != nullptr)
		{
			delete tcpInfo->node;
			tcpInfo->node = nullptr;
			obj_sub;
		}
		uv_close((uv_handle_t *)tcpInfo, on_close_free_handle);
		free(connect);
		return nullptr;
	}

	void on_wakeup(uv_async_t *async)
	{
		CnetworkPool *pool = container_of(async, __async_with_info, async)->pool;
		// Copy pending to local first.
		std::unordered_set<CnetworkNode, __network_hash> bindCopy;
		std::list<CnetworkPool::__pending_send> sendCopy;
		pool->m_lock.lock();
		std::swap(bindCopy, pool->m_pendingBind);
		std::swap(sendCopy, pool->m_pendingSend);
		pool->m_lock.unlock();
		if (pool->m_bWantExit)
		{
			//
			// Stop and free all resources.
			//
			// Async.
			uv_close((uv_handle_t *)&pool->m_wakeup, on_close_no_free);
			// TCP servers.
			for (auto it = pool->m_tcpServers.begin(); it != pool->m_tcpServers.end(); ++it)
			{
				if ((*it)->node != nullptr)
				{
					// Report bind down.
					pool->m_callback.bindStatus(*(*it)->node, false);
					// Delete node data.
					delete (*it)->node;
					(*it)->node = nullptr;
					obj_sub;
				}
				uv_close((uv_handle_t *)*it, on_close_free_handle);
			}
			pool->m_tcpServers.clear();
			// UDP servers.
			for (auto it = pool->m_udpServers.begin(); it != pool->m_udpServers.end(); ++it)
			{
				if ((*it)->node != nullptr)
				{
					// Report bind down.
					pool->m_callback.bindStatus(*(*it)->node, false);
					// Delete node data.
					delete (*it)->node;
					(*it)->node = nullptr;
					obj_sub;
				}
				uv_close((uv_handle_t *)*it, on_close_free_handle);
			}
			pool->m_udpServers.clear();
			// TCP connections.
			for (auto it = pool->m_node2stream.begin(); it != pool->m_node2stream.end(); ++it)
			{
				if (it->second->node != nullptr)
				{
					// Report connection down.
					pool->m_callback.connectionStatus(*it->second->node, false);
					// Delete node data.
					delete it->second->node;
					it->second->node = nullptr;
					obj_sub;
				}
				uv_close((uv_handle_t *)it->second, on_close_free_handle);
			}
			pool->m_node2stream.clear();
			// TCP connecting.
			for (auto it = pool->m_connecting.begin(); it != pool->m_connecting.end(); ++it)
			{
				if ((*it)->node != nullptr)
				{
					// Report connection down.
					pool->m_callback.connectionStatus(*(*it)->node, false);
					// Delete node data.
					delete (*it)->node;
					(*it)->node = nullptr;
					obj_sub;
				}
				uv_close((uv_handle_t *)*it, on_close_free_handle);
			}
			pool->m_connecting.clear();
			// Drop all waiting message.
			for (auto it = pool->m_waitingSend.begin(); it != pool->m_waitingSend.end(); ++it)
			{
				const CnetworkNode& node = it->first;
				for (auto it1 = it->second.begin(); it1 != it->second.end(); ++it1)
				{
					pool->m_callback.drop(node, it1->base, it1->len);
					free(it1->base);
				}
			}
			pool->m_waitingSend.clear();
			// Drop all pending bind & message.
			for (auto it = bindCopy.begin(); it != bindCopy.end(); ++it)
				pool->m_callback.bindStatus(*it, false);
			for (auto it = sendCopy.begin(); it != sendCopy.end(); ++it)
				pool->m_callback.drop(it->node, it->data.getData(), it->data.getLength());
		}
		else
		{
			//
			// Bind & send.
			//
			// Bind.
			for (auto it = bindCopy.begin(); it != bindCopy.end(); ++it)
			{
				switch (it->getProtocol())
				{
				case CnetworkNode::protocol_tcp:
				{
					__tcp_with_info *tcpServer = bindAndListenTcp(pool, &pool->m_loop, *it);
					if (tcpServer != nullptr)
						pool->m_tcpServers.insert(tcpServer);
					pool->m_callback.bindStatus(*it, tcpServer != nullptr);
				}
					break;
				case CnetworkNode::protocol_udp:
					// Todo. Code here to implement UDP server.

				default:
					pool->m_callback.bindStatus(*it, false);
					break;
				}
			}
			// Send.
			for (auto it = sendCopy.begin(); it != sendCopy.end(); ++it)
			{
				const CnetworkNode& node = it->node;
				Cbuffer& data = it->data;
				const bool& bAutoConnect = it->auto_connect;
				switch (node.getProtocol())
				{
				case CnetworkNode::protocol_tcp:
				{
					__tcp_with_info *tcpInfo = pool->getStreamByNode(node);
					if (nullptr == tcpInfo)
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
								tcpInfo = connectTcp(pool, &pool->m_loop, node);
								if (nullptr == tcpInfo)
								{
									// Connect fail.
									pool->m_callback.connectionStatus(node, false);
									pool->dropWaiting(node);
								}
								else // Put in connecting set.
									pool->m_connecting.insert(tcpInfo);
							}
						}
					}
					else
					{
						// Just use write.
						__write_with_info *writeInfo = (__write_with_info *)malloc(sizeof(__write_with_info), false); // Only one buf.
						if (nullptr == writeInfo)
						{
							// Insufficient memory.
							// Just drop.
							_FPRINTF((stderr, "Send TCP error with insufficient memory.\n"));
							pool->m_callback.drop(node, data.getData(), data.getLength());
						}
						else
						{
							writeInfo->num = 1;
							data.transfer(writeInfo->buf[0]);
							if (uv_write(&writeInfo->write, (uv_stream_t *)tcpInfo, writeInfo->buf, (unsigned int)writeInfo->num, on_tcp_write_done) != 0)
							{
								pool->dropWriteAndFree(node, writeInfo);
								// Shutdown connection.
								pool->shutdownTcpConnection(tcpInfo);
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
		}
	}

	inline void CnetworkPool::dropWaiting(const CnetworkNode& node)
	{
		auto waitingIt = m_waitingSend.find(node);
		if (waitingIt != m_waitingSend.end())
		{
			for (auto it = waitingIt->second.begin(); it != waitingIt->second.end(); ++it)
			{
				m_callback.drop(node, it->base, it->len);
				free(it->base);
			}
			m_waitingSend.erase(waitingIt);
		}
	}

	inline void CnetworkPool::pushWaiting(const CnetworkNode& node, Cbuffer& data)
	{
		uv_buf_t buf;
		data.transfer(buf);
		auto it = m_waitingSend.find(node);
		if (m_waitingSend.end() == it)
		{
			std::vector<uv_buf_t> vec(1, buf);
			m_waitingSend.insert(std::make_pair(node, vec));
		}
		else
			it->second.push_back(buf);
	}

	inline void CnetworkPool::dropWriteAndFree(const CnetworkNode& node, __write_with_info *writeInfo)
	{
		// Notify message drop and delete it.
		for (size_t i = 0; i < writeInfo->num; ++i)
		{
			m_callback.drop(node, writeInfo->buf[i].base, writeInfo->buf[i].len);
			free(writeInfo->buf[i].base);
		}
		free(writeInfo);
	}

	inline __tcp_with_info *CnetworkPool::getStreamByNode(const CnetworkNode& node)
	{
		auto it = m_node2stream.find(node);
		if (m_node2stream.end() == it)
			return nullptr;
		return it->second;
	}

	inline __write_with_info *CnetworkPool::getWriteFromWaitingByNode(const CnetworkNode& node)
	{
		auto it = m_waitingSend.find(node);
		if (m_waitingSend.end() == it)
			return nullptr;
		__write_with_info *writeInfo = (__write_with_info *)malloc(sizeof(__write_with_info) + sizeof(uv_buf_t)*(it->second.size() - 1), false);
		if (nullptr == writeInfo)
		{
			// Insufficient memory.
			// Just drop.
			_FPRINTF((stderr, "Send waiting TCP error with insufficient memory.\n"));
			dropWaiting(node);
			return nullptr;
		}
		writeInfo->num = it->second.size();
		for (size_t i = 0; i < writeInfo->num; ++i)
			writeInfo->buf[i] = it->second[i];
		m_waitingSend.erase(it);
		return writeInfo;
	}

	inline void CnetworkPool::startupTcpConnection(__tcp_with_info *tcpInfo)
	{
		if (tcpInfo->node != nullptr)
		{
			const CnetworkNode& node = *tcpInfo->node;
			// Add map.
			auto ib = m_node2stream.insert(std::make_pair(node, tcpInfo));
			if (!ib.second)
			{
				// Remote port reuse?
				// If a connection is startup, no data will be written to waiting queue, so just reject.
				_FPRINTF((stderr, "Error startup a connection with remote port reuse.\n"));
				// Delete node data.
				delete tcpInfo->node;
				tcpInfo->node = nullptr;
				obj_sub;
				// Close connection.
				uv_close((uv_handle_t *)tcpInfo, on_close_free_handle);
				return;
			}
			// Report new connection.
			m_callback.connectionStatus(node, true);
			// Send message waiting.
			__write_with_info *writeInfo = getWriteFromWaitingByNode(node);
			if (writeInfo != nullptr)
			{
				if (uv_write(&writeInfo->write, (uv_stream_t *)tcpInfo, writeInfo->buf, (unsigned int)writeInfo->num, on_tcp_write_done) != 0)
				{
					dropWriteAndFree(node, writeInfo);
					// Shutdown connection.
					shutdownTcpConnection(tcpInfo);
				}
			}
		}
		else
		{
			// WTF to get here? The only thing we can do is just close it.
			_FPRINTF((stderr, "Fatal error startup a connection whithout node.\n"));
			uv_close((uv_handle_t *)tcpInfo, on_close_free_handle);
		}
	}

	inline void CnetworkPool::shutdownTcpConnection(__tcp_with_info *tcpInfo)
	{
		if (tcpInfo->node != nullptr)
		{
			const CnetworkNode& node = *tcpInfo->node;
			// Clean map.
			m_node2stream.erase(node);
			// Report connection down.
			m_callback.connectionStatus(node, false);
			// Drop message notify.
			dropWaiting(node);
			// Delete node data.
			delete tcpInfo->node;
			tcpInfo->node = nullptr;
			obj_sub;
		}
		// Close connection.
		uv_close((uv_handle_t *)tcpInfo, on_close_free_handle);
	}

	void CnetworkPool::internalThread()
	{
		// Init loop.
		if (uv_loop_init(&m_loop) != 0)
		{
			m_state = bad;
			return;
		}
		m_wakeup.pool = this;
		if (uv_async_init(&m_loop, &m_wakeup.async, on_wakeup) != 0)
		{
			uv_loop_close(&m_loop);
			m_state = bad;
			return;
		}
		m_state = good;
		uv_run(&m_loop, UV_RUN_DEFAULT);
		uv_loop_close(&m_loop);
	}

	CnetworkPool::CnetworkPool(CnetworkPoolCallback& callback)
		:m_state(initializing), m_callback(callback), m_bWantExit(false), m_udpIndex(0)
	{
		m_thread = new std::thread(&CnetworkPool::internalThread, this); // May throw.
		obj_add;
		while (initializing == m_state)
			std::this_thread::yield();
		if (m_state != good)
			throw(-1);
	}

	CnetworkPool::~CnetworkPool()
	{
		m_bWantExit = true;
		uv_async_send(&m_wakeup.async);
		m_thread->join();
		delete m_thread;
		m_thread = nullptr;
		obj_sub;
	}
}
