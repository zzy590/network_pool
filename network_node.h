
#pragma once

#include <string>

namespace NETWORK_POOL
{
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

		inline void set(const CnetworkNode& another)
		{
			m_protocol = another.m_protocol;
			m_ip = another.m_ip;
			m_port = another.m_port;
			m_hash = another.m_hash;
		}
		inline void set(const protocol_type protocol, const std::string& ip, const unsigned short port)
		{
			m_protocol = protocol;
			m_ip = ip;
			m_port = port;
			rehash();
		}
		inline void setProtocol(const protocol_type protocol)
		{
			m_protocol = protocol;
			rehash();
		}
		inline void setIp(const std::string& ip)
		{
			m_ip = ip;
			rehash();
		}
		inline void setPort(const unsigned short port)
		{
			m_port = port;
			rehash();
		}

		inline protocol_type getProtocol() const
		{
			return m_protocol;
		}
		inline const std::string& getIp() const
		{
			return m_ip;
		}
		inline unsigned short getPort() const
		{
			return m_port;
		}

		inline size_t getHash() const
		{
			return m_hash;
		}

		inline bool isIPv6() const
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

		inline void rehash()
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

		inline void set(const CnetworkPair& another)
		{
			m_local = another.m_local;
			m_remote = another.m_remote;
			m_hash = another.m_hash;
		}

		inline void setLocal(const CnetworkNode& local)
		{
			m_local = local;
			rehash();
		}
		inline void setRemote(const CnetworkNode& remote)
		{
			m_remote = remote;
			rehash();
		}

		inline const CnetworkNode& getLocal() const
		{
			return m_local;
		}
		inline const CnetworkNode& getRemote() const
		{
			return m_remote;
		}

		inline size_t getHash() const
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
}
