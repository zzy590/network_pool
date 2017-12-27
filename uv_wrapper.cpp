
#include "uv_wrapper.h"
#include "network_pool.h"
#include "np_dbg.h"

namespace NETWORK_POOL
{
	//
	// Casync
	//

	static void on_close_async(uv_handle_t *handle)
	{
		Casync *async = Casync::obtain(handle);
		async->getPool()->getMemoryTrace()._delete_set_nullptr<Casync>(async);
	}

	Casync *Casync::alloc(CnetworkPool *pool, uv_loop_t *loop, uv_async_cb cb)
	{
		Casync *async = pool->getMemoryTrace()._new_no_throw<Casync>();
		if (nullptr == async)
			return nullptr;
		async->m_inited = false;
		async->m_closing = false;
		async->m_pool = pool;
		if (uv_async_init(loop, &async->m_async, cb) != 0)
		{
			pool->getMemoryTrace()._delete_set_nullptr<Casync>(async);
			return nullptr;
		}
		async->m_inited = true;
		return async;
	}

	void Casync::close_set_nullptr(Casync *& async)
	{
		if (async->m_inited)
		{
			if (!async->m_closing)
			{
				uv_close((uv_handle_t *)&async->m_async, on_close_async);
				async->m_closing = true;
			}
			async = nullptr;
		}
		else
			async->m_pool->getMemoryTrace()._delete_set_nullptr<Casync>(async);
	}

	//
	// Ctcp
	//

	void on_close_tcp(uv_handle_t *handle)
	{
		Ctcp *tcp = Ctcp::obtainFromTcp(handle);
		tcp->m_tcpInited = false;
		if (!tcp->m_timerInited)
			tcp->getPool()->getMemoryTrace()._delete_set_nullptr<Ctcp>(tcp);
	}

	void on_close_tcp_timer(uv_handle_t *handle)
	{
		Ctcp *tcp = Ctcp::obtainFromTimer(handle);
		tcp->m_timerInited = false;
		if (!tcp->m_tcpInited)
			tcp->getPool()->getMemoryTrace()._delete_set_nullptr<Ctcp>(tcp);
	}

	static inline bool setTcp(uv_tcp_t *tcp, const __preferred_network_settings& settings)
	{
		if (uv_tcp_nodelay(tcp, settings.tcp_enable_nodelay) != 0)
		{
			NP_FPRINTF((stderr, "Error set tcp nodelay.\n"));
			return false;
		}
		if (uv_tcp_keepalive(tcp, settings.tcp_enable_keepalive, settings.tcp_keepalive_time_in_seconds) != 0)
		{
			NP_FPRINTF((stderr, "Error set tcp keepalive.\n"));
			return false;
		}
		if (uv_tcp_simultaneous_accepts(tcp, settings.tcp_enable_simultaneous_accepts) != 0)
		{
			NP_FPRINTF((stderr, "Error set tcp simultaneous accepts.\n"));
			return false;
		}
		// Set Tx & Rx buffer size.
		int tmpSz = settings.tcp_send_buffer_size;
		if (tmpSz != 0)
			uv_send_buffer_size((uv_handle_t *)tcp, &tmpSz); // It's just prefer, so ignore the return.
		tmpSz = settings.tcp_recv_buffer_size;
		if (tmpSz != 0)
			uv_recv_buffer_size((uv_handle_t *)tcp, &tmpSz); // It's just prefer, so ignore the return.
		return true;
	}

	Ctcp *Ctcp::alloc(CnetworkPool *pool, uv_loop_t *loop, bool initTimer)
	{
		Ctcp *tcp = pool->getMemoryTrace()._new_no_throw<Ctcp>();
		if (nullptr == tcp)
			return nullptr;
		tcp->m_tcpInited = false;
		tcp->m_timerInited = false;
		tcp->m_closing = false;
		tcp->m_pool = pool;
		if (uv_tcp_init(loop, &tcp->m_tcp) != 0)
			goto _ec;
		tcp->m_tcpInited = true;
		if (initTimer)
		{
			if (uv_timer_init(loop, &tcp->m_timer) != 0)
				goto _ec;
			tcp->m_timerInited = true;
		}
		if (!setTcp(&tcp->m_tcp, pool->getSettings()))
			goto _ec;
		return tcp;
	_ec:
		close_set_nullptr(tcp);
		return nullptr;
	}

	void Ctcp::close_set_nullptr(Ctcp *& tcp)
	{
		if (tcp->m_tcpInited || tcp->m_timerInited)
		{
			if (!tcp->m_closing)
			{
				// Both close callback should be set if initialized.
				if (tcp->m_tcpInited)
					uv_close((uv_handle_t *)&tcp->m_tcp, on_close_tcp);
				if (tcp->m_timerInited)
					uv_close((uv_handle_t *)&tcp->m_timer, on_close_tcp_timer);
				tcp->m_closing = true;
			}
			tcp = nullptr;
		}
		else
			tcp->m_pool->getMemoryTrace()._delete_set_nullptr<Ctcp>(tcp);
	}

	//
	// Cudp
	//

	static void on_close_udp(uv_handle_t *handle)
	{
		Cudp *udp = Cudp::obtain(handle);
		udp->getPool()->getMemoryTrace()._delete_set_nullptr<Cudp>(udp);
	}

	Cudp *Cudp::alloc(CnetworkPool *pool, uv_loop_t *loop)
	{
		Cudp *udp = pool->getMemoryTrace()._new_no_throw<Cudp>();
		if (nullptr == udp)
			return nullptr;
		udp->m_inited = false;
		udp->m_closing = false;
		udp->m_pool = pool;
		if (uv_udp_init(loop, &udp->m_udp) != 0)
		{
			pool->getMemoryTrace()._delete_set_nullptr<Cudp>(udp);
			return nullptr;
		}
		udp->m_inited = true;
		return udp;
	}

	void Cudp::close_set_nullptr(Cudp *& udp)
	{
		if (udp->m_inited)
		{
			if (!udp->m_closing)
			{
				uv_close((uv_handle_t *)&udp->m_udp, on_close_udp);
				udp->m_closing = true;
			}
			udp = nullptr;
		}
		else
			udp->m_pool->getMemoryTrace()._delete_set_nullptr<Cudp>(udp);
	}
}
