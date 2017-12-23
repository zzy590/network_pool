
#pragma once

#include <string>
#include <vector>
#include <list>
#include <unordered_set>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <atomic>

#include "uv.h"

namespace NETWORK_POOL
{
	//
	// Caution! Program may cash when fail to malloc in critical step.
	// So be careful to check memory usage before pushing packet to network pool.
	//
	// TCP port reuse may cause some problem.
	// Currently, we just reject the connection reuse the same ip and port which connect to same pool.
	//

	void getMemoryTrace(int64_t& memTotal, int32_t& memCnt, int32_t& objCnt);

	class CnetworkNode
	{
	public:
		enum protocol_type
		{
			protocol_tcp = 0,
			protocol_udp
		};

	private:
		protocol_type m_protocol;
		std::string m_ip;
		unsigned short m_port;

		size_t m_hash;

		inline void rehash()
		{
			m_hash = m_protocol == protocol_tcp ? 0 : 1;
			for (const auto& val : m_ip)
				m_hash = 31 * m_hash + val;
			m_hash = (m_hash << 16) + m_port;
		}

	public:
		CnetworkNode()
			:m_protocol(protocol_tcp), m_port(0) { rehash(); }
		CnetworkNode(const protocol_type protocol, const std::string& ip, const unsigned short port)
			:m_protocol(protocol), m_ip(ip), m_port(port) { rehash(); }
		CnetworkNode(const CnetworkNode& another)
			:m_protocol(another.m_protocol), m_ip(another.m_ip), m_port(another.m_port), m_hash(another.m_hash) {}

		const CnetworkNode& operator=(const CnetworkNode& another)
		{
			m_protocol = another.m_protocol;
			m_ip = another.m_ip;
			m_port = another.m_port;
			m_hash = another.m_hash;
			return *this;
		}
		bool operator<(const CnetworkNode& another) const
		{
			if (m_hash != another.m_hash)
				return m_hash < another.m_hash;
			if (m_protocol != another.m_protocol)
				return m_protocol < another.m_protocol;
			if (m_port != another.m_port)
				return m_port < another.m_port;
			return m_ip < another.m_ip; // For performance, compare IP at end.
		}
		bool operator==(const CnetworkNode& another) const
		{
			return m_hash == another.m_hash && m_protocol == another.m_protocol && m_port == another.m_port && m_ip == another.m_ip; // For performance, compare IP at end.
		}
		bool operator!=(const CnetworkNode& another) const
		{
			return m_hash != another.m_hash || m_protocol != another.m_protocol || m_port != another.m_port || m_ip != another.m_ip; // For performance, compare IP at end.
		}

		void set(const CnetworkNode& another)
		{
			m_protocol = another.m_protocol;
			m_ip = another.m_ip;
			m_port = another.m_port;
			m_hash = another.m_hash;
		}
		void set(const protocol_type protocol, const std::string& ip, const unsigned short port)
		{
			m_protocol = protocol;
			m_ip = ip;
			m_port = port;
			rehash();
		}
		void setProtocol(const protocol_type protocol)
		{
			m_protocol = protocol;
			rehash();
		}
		void setIp(const std::string& ip)
		{
			m_ip = ip;
			rehash();
		}
		void setPort(const unsigned short port)
		{
			m_port = port;
			rehash();
		}

		protocol_type getProtocol() const
		{
			return m_protocol;
		}
		const std::string& getIp() const
		{
			return m_ip;
		}
		unsigned short getPort() const
		{
			return m_port;
		}

		size_t getHash() const
		{
			return m_hash;
		}

		bool isIPv6() const
		{
			return m_ip.find(':') != std::string::npos;
		}
	};

	class CnetworkPair
	{
	private:
		CnetworkNode m_local;
		CnetworkNode m_remote;

		size_t m_hash;

		void rehash()
		{
			m_hash = (m_local.getHash() << sizeof(size_t) * 4) + m_remote.getHash();
		}

	public:
		CnetworkPair() { rehash(); }
		CnetworkPair(const CnetworkNode& local, const CnetworkNode& remote)
			:m_local(local), m_remote(remote) { rehash(); }
		CnetworkPair(const CnetworkPair& another)
			:m_local(another.m_local), m_remote(another.m_remote), m_hash(another.m_hash) {}

		const CnetworkPair& operator=(const CnetworkPair& another)
		{
			m_local = another.m_local;
			m_remote = another.m_remote;
			m_hash = another.m_hash;
			return *this;
		}
		bool operator<(const CnetworkPair& another) const
		{
			if (m_hash != another.m_hash)
				return m_hash < another.m_hash;
			if (m_remote != another.m_remote) // Compare remote address(usually different).
				return m_remote < another.m_remote;
			return m_local < another.m_local;
		}
		bool operator==(const CnetworkPair& another) const
		{
			return m_hash == another.m_hash && m_remote == another.m_remote && m_local == another.m_local; // Compare remote address(usually different).
		}
		bool operator!=(const CnetworkPair& another) const
		{
			return m_hash != another.m_hash || m_remote != another.m_remote || m_local != another.m_local; // Compare remote address(usually different).
		}

		void set(const CnetworkPair& another)
		{
			m_local = another.m_local;
			m_remote = another.m_remote;
			m_hash = another.m_hash;
		}

		void setLocal(const CnetworkNode& local)
		{
			m_local = local;
			rehash();
		}
		void setRemote(const CnetworkNode& remote)
		{
			m_remote = remote;
			rehash();
		}

		const CnetworkNode& getLocal() const
		{
			return m_local;
		}
		const CnetworkNode& getRemote() const
		{
			return m_remote;
		}

		size_t getHash() const
		{
			return m_hash;
		}
	};

	struct __network_hash
	{
		size_t operator()(const CnetworkNode& k) const
		{
			return k.getHash();
		}
		size_t operator()(const CnetworkPair& k) const
		{
			return k.getHash();
		}
	};

	class Cbuffer
	{
	private:
		void *m_data;
		size_t m_length;
		size_t m_maxLength;

		inline void transfer(uv_buf_t& buf); // Internal use only.

		friend void on_wakeup(uv_async_t *async);
		friend class CnetworkPool;

	public:
		Cbuffer();
		Cbuffer(const size_t length);
		Cbuffer(const void *data, const size_t length);
		Cbuffer(const Cbuffer& another);
		~Cbuffer();

		const Cbuffer& operator=(const Cbuffer& another);

		void set(const void *data, const size_t length);
		void resize(const size_t preferLength, const size_t validLength = 0);

		void *getData() const
		{
			return m_data;
		}
		size_t getLength() const
		{
			return m_length;
		}
		size_t getMaxLength() const
		{
			return m_maxLength;
		}
	};

	class CnetworkPoolCallback
	{
	public:
		virtual ~CnetworkPoolCallback() {}

		// Message received.
		virtual void message(const CnetworkNode& node, const void *data, const size_t length) = 0;

		// Message which dropped due to the connection failed.
		// Note: Drop before connection down notify means failed to send(maybe other reason),
		//       and drop after connection down notify means failed by the down of the connection.
		virtual void drop(const CnetworkNode& node, const void *data, const size_t length) = 0;

		// Local bind and remote connection notify.
		virtual void bindStatus(const CnetworkNode& node, const bool bSuccess) = 0;
		// Note: No connection down notify if you send a message with no auto connect when no connection.
		virtual void connectionStatus(const CnetworkNode& node, const bool bSuccess) = 0;
	};

	class CnetworkPool;

	struct __async_with_info
	{
		uv_async_t async;
		CnetworkPool *pool;
	};

	struct __tcp_with_info
	{
		uv_tcp_t tcp;
		CnetworkPool *pool;
		CnetworkNode *node; // Need delete when free.
	};

	struct __udp_with_info
	{
		uv_udp_t udp;
		CnetworkPool *pool;
		CnetworkNode *node; // Need delete when free.
	};

	struct __write_with_info
	{
		uv_write_t write;
		size_t num;
		uv_buf_t buf[1]; // Need free when complete request.
	};

	class CnetworkPool
	{
	private:
		enum __internal_state
		{
			initializing = 0,
			good,
			bad
		};
		// Status of internal thread.
		volatile __internal_state m_state;

		CnetworkPoolCallback& m_callback;
		bool m_bWantExit;

		// Internal thread.
		std::thread *m_thread;

		// Data which exchanged between internal and external.
		std::mutex m_lock;
		std::unordered_set<CnetworkNode, __network_hash> m_pendingBind;
		struct __pending_send
		{
			CnetworkNode node;
			Cbuffer data;
			bool auto_connect;
		};
		std::list<__pending_send> m_pendingSend;

		// Tx & Rx buffer preferred.
		std::atomic<int> m_sendBufferSize;
		std::atomic<int> m_recvBufferSize;

		// Use round robin to send message on UDP.
		int m_udpIndex;

		// Loop must be initialized in internal work thread.
		// Following data must be accessed by internal thread.
		uv_loop_t m_loop;
		__async_with_info m_wakeup;
		std::unordered_set<__tcp_with_info *> m_tcpServers;
		std::vector<__udp_with_info *> m_udpServers;
		std::unordered_map<CnetworkNode, __tcp_with_info *, __network_hash> m_node2stream;
		std::unordered_set<__tcp_with_info *> m_connecting;
		std::unordered_map<CnetworkNode, std::vector<uv_buf_t>, __network_hash> m_waitingSend; // Waiting for connection complete.

		friend void on_tcp_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf);
		friend void on_tcp_write_done(uv_write_t *req, int status);
		friend void on_new_connection(uv_stream_t *server, int status);
		friend void on_connect_done(uv_connect_t *req, int status);
		friend void on_wakeup(uv_async_t *async);

		inline void dropWaiting(const CnetworkNode& node);
		inline void pushWaiting(const CnetworkNode& node, Cbuffer& data);
		inline void dropWriteAndFree(const CnetworkNode& node, __write_with_info *writeInfo);
		inline __tcp_with_info *getStreamByNode(const CnetworkNode& node);
		inline __write_with_info *getWriteFromWaitingByNode(const CnetworkNode& node);

		// Caution! Call following function(s) may cause iterator of m_node2stream and m_waitingSend invalid.
		inline void startupTcpConnection(__tcp_with_info *tcpInfo);
		inline void shutdownTcpConnection(__tcp_with_info *tcpInfo);

		void internalThread();

	public:
		CnetworkPool(CnetworkPoolCallback& callback); // throw when fail.
		~CnetworkPool();

		// Set the system socket Tx & Rx buffer size.
		// Set 0 means use the system default value.
		// Note: Linux will set double the size of the original set value.
		void setSendAndRecvBufferSize(const int sendBufferSize, const int recvBufferSize)
		{
			m_sendBufferSize = sendBufferSize;
			m_recvBufferSize = recvBufferSize;
		}

		void bind(const CnetworkNode& node)
		{
			m_lock.lock();
			m_pendingBind.insert(node);
			m_lock.unlock();
			uv_async_send(&m_wakeup.async);
		}

		bool send(const CnetworkNode& node, const void *data, const size_t length, const bool bAutoConnect = false)
		{
			__pending_send temp;
			temp.node.set(node);
			try
			{
				temp.data.set(data, length); // May throw(-1) if malloc fail.
			}
			catch (...)
			{
				return false;
			}
			temp.auto_connect = bAutoConnect;
			m_lock.lock();
			m_pendingSend.push_back(__pending_send());
			std::swap(m_pendingSend.back(), temp);
			m_lock.unlock();
			uv_async_send(&m_wakeup.async);
			return true;
		}
	};
}
