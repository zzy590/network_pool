
#pragma once

#include "uv.h"

#include "network_node.h"

namespace NETWORK_POOL
{
	#define PRIVATE_CLASS(_class) \
		private: \
			_class() {} \
			~_class() {} \
			_class(const _class& another) = delete; \
			const _class& operator=(const _class& another) = delete;
	#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

	class CnetworkPool;

	class Casync
	{
		PRIVATE_CLASS(Casync)
	private:
		uv_async_t m_async;
		bool m_inited;
		bool m_closing;
		CnetworkPool *m_pool;

		friend class CmemoryTrace;

	public:
		static Casync *alloc(CnetworkPool *pool, uv_loop_t *loop, uv_async_cb cb);
		static void close_set_nullptr(Casync *& async);

		static inline Casync *obtain(uv_handle_t *handle)
		{
			return container_of(handle, Casync, m_async);
		}
		static inline Casync *obtain(uv_async_t *async)
		{
			return container_of(async, Casync, m_async);
		}

		inline uv_async_t *getAsync()
		{
			return &m_async;
		}
		inline CnetworkPool *getPool() const
		{
			return m_pool;
		}
	};

	class Ctcp
	{
		PRIVATE_CLASS(Ctcp)
	private:
		uv_tcp_t m_tcp;
		uv_timer_t m_timer;
		bool m_tcpInited;
		bool m_timerInited;
		bool m_closing;
		CnetworkPool *m_pool;
		CnetworkNode m_node;

		friend void on_close_tcp(uv_handle_t *handle);
		friend void on_close_tcp_timer(uv_handle_t *handle);
		friend class CmemoryTrace;

	public:
		static Ctcp *alloc(CnetworkPool *pool, uv_loop_t *loop, bool initTimer = true);
		static void close_set_nullptr(Ctcp *& tcp);

		static inline Ctcp *obtainFromTcp(uv_handle_t *handle)
		{
			return container_of(handle, Ctcp, m_tcp);
		}
		static inline Ctcp *obtainFromTimer(uv_handle_t *handle)
		{
			return container_of(handle, Ctcp, m_timer);
		}
		static inline Ctcp *obtain(uv_stream_t *stream)
		{
			return container_of(stream, Ctcp, m_tcp);
		}
		static inline Ctcp *obtain(uv_tcp_t *tcp)
		{
			return container_of(tcp, Ctcp, m_tcp);
		}
		static inline Ctcp *obtain(uv_timer_t *timer)
		{
			return container_of(timer, Ctcp, m_timer);
		}

		inline uv_tcp_t *getTcp()
		{
			return &m_tcp;
		}
		inline uv_stream_t *getStream()
		{
			return (uv_stream_t *)&m_tcp;
		}
		inline uv_timer_t *getTimer()
		{
			return &m_timer;
		}
		inline CnetworkPool *getPool() const
		{
			return m_pool;
		}
		inline CnetworkNode& getNode()
		{
			return m_node;
		}
	};

	class Cudp
	{
		PRIVATE_CLASS(Cudp)
	private:
		uv_udp_t m_udp;
		bool m_inited;
		bool m_closing;
		CnetworkPool *m_pool;
		CnetworkNode m_node;

		friend class CmemoryTrace;

	public:
		static Cudp *alloc(CnetworkPool *pool, uv_loop_t *loop);
		static void close_set_nullptr(Cudp *& udp);

		static inline Cudp *obtain(uv_handle_t *handle)
		{
			return container_of(handle, Cudp, m_udp);
		}
		static inline Cudp *obtain(uv_udp_t *udp)
		{
			return container_of(udp, Cudp, m_udp);
		}

		inline uv_udp_t *getUdp()
		{
			return &m_udp;
		}
		inline CnetworkPool *getPool() const
		{
			return m_pool;
		}
		inline CnetworkNode& getNode()
		{
			return m_node;
		}
	};

	#undef container_of
	#undef PRIVATE_CLASS
}
