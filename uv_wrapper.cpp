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

#include "uv_wrapper.h"
#include "network_pool.h"
#include "np_dbg.h"

namespace NETWORK_POOL
{
	//
	// Casync
	//

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
				uv_close((uv_handle_t *)&async->m_async,
					[](uv_handle_t *handle)
				{
					Casync *async = Casync::obtain(handle);
					async->m_pool->getMemoryTrace()._delete_set_nullptr<Casync>(async);
				});
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
		tcp->m_shutdown = false;
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
					uv_close((uv_handle_t *)&tcp->m_tcp,
					[](uv_handle_t *handle)
				{
					Ctcp *tcp = Ctcp::obtainFromTcp(handle);
					tcp->m_tcpInited = false;
					if (!tcp->m_timerInited)
						tcp->m_pool->getMemoryTrace()._delete_set_nullptr<Ctcp>(tcp);
				});
				if (tcp->m_timerInited)
					uv_close((uv_handle_t *)&tcp->m_timer,
					[](uv_handle_t *handle)
				{
					Ctcp *tcp = Ctcp::obtainFromTimer(handle);
					tcp->m_timerInited = false;
					if (!tcp->m_tcpInited)
						tcp->m_pool->getMemoryTrace()._delete_set_nullptr<Ctcp>(tcp);
				});
				tcp->m_closing = true;
			}
			tcp = nullptr;
		}
		else
			tcp->m_pool->getMemoryTrace()._delete_set_nullptr<Ctcp>(tcp);
	}

	void Ctcp::shutdown_and_close_set_nullptr(Ctcp *& tcp)
	{
		if (!tcp->m_tcpInited)
			goto _ec;
		if (!tcp->m_closing && !tcp->m_shutdown)
		{
			uv_shutdown_t *shutdown = (uv_shutdown_t *)tcp->m_pool->getMemoryTrace()._malloc_no_throw(sizeof(uv_shutdown_t));
			if (nullptr == shutdown)
				goto _ec;
			if (uv_shutdown(shutdown, (uv_stream_t *)&tcp->m_tcp,
				[](uv_shutdown_t *req, int status)
			{
				Ctcp *tcp = Ctcp::obtain(req->handle);
				tcp->m_pool->getMemoryTrace()._free_set_nullptr(req);
				close_set_nullptr(tcp);
			}) != 0)
			{
				tcp->m_pool->getMemoryTrace()._free_set_nullptr(shutdown);
				goto _ec;
			}
			tcp->m_shutdown = true;
		}
		tcp = nullptr;
		return;
	_ec:
		close_set_nullptr(tcp);
	}

	//
	// Cudp
	//

	static inline bool setUdp(uv_udp_t *udp, const __preferred_network_settings& settings)
	{
		uv_udp_set_ttl(udp, settings.udp_ttl); // It's just prefer, so ignore the return.
		return true;
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
		if (!setUdp(&udp->m_udp, pool->getSettings()))
		{
			close_set_nullptr(udp);
			return nullptr;
		}
		return udp;
	}

	void Cudp::close_set_nullptr(Cudp *& udp)
	{
		if (udp->m_inited)
		{
			if (!udp->m_closing)
			{
				uv_close((uv_handle_t *)&udp->m_udp,
					[](uv_handle_t *handle)
				{
					Cudp *udp = Cudp::obtain(handle);
					udp->m_pool->getMemoryTrace()._delete_set_nullptr<Cudp>(udp);
				});
				udp->m_closing = true;
			}
			udp = nullptr;
		}
		else
			udp->m_pool->getMemoryTrace()._delete_set_nullptr<Cudp>(udp);
	}
}
