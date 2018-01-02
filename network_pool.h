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

#pragma once

#include <vector>
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <thread>
#include <mutex>

#include "uv.h"

#include "network_callback.h"
#include "memory_trace.h"
#include "network_node.h"
#include "uv_wrapper.h"
#include "buffer.h"

namespace NETWORK_POOL
{
	//
	// Caution! Program may cash when fail to allocate memory in critical step.
	// So be careful to check memory usage before pushing packet to network pool.
	//
	// TCP port reuse may cause some problem.
	// Currently, we just reject the connection reuse the same ip and port which connect to same pool.
	//

	struct __preferred_network_settings
	{
		// Tcp settings.
		int tcp_enable_nodelay;
		int tcp_enable_keepalive;
		unsigned int tcp_keepalive_time_in_seconds;
		int tcp_enable_simultaneous_accepts;
		int tcp_backlog;
		// Set 0 means use the system default value.
		// Note: Linux will set double the size of the original set value.
		int tcp_send_buffer_size;
		int tcp_recv_buffer_size;
		// Tcp timeouts.
		unsigned int tcp_connect_timeout_in_seconds;
		unsigned int tcp_idle_timeout_in_seconds;
		unsigned int tcp_send_timeout_in_seconds;

		__preferred_network_settings()
		{
			tcp_enable_nodelay = 1;
			tcp_enable_keepalive = 1;
			tcp_keepalive_time_in_seconds = 30;
			tcp_enable_simultaneous_accepts = 1;
			tcp_backlog = 128;
			tcp_send_buffer_size = 0;
			tcp_recv_buffer_size = 0;
			tcp_connect_timeout_in_seconds = 10;
			tcp_idle_timeout_in_seconds = 30;
			tcp_send_timeout_in_seconds = 30;
		}
	};

	class CnetworkPool
	{
	private:
		struct __write_with_info
		{
			uv_write_t write;
			size_t num;
			uv_buf_t buf[1]; // Need free when complete request.
		};

		enum __internal_state
		{
			initializing = 0,
			good,
			bad
		};
		// Status of internal thread.
		volatile __internal_state m_state;

		__preferred_network_settings m_settings;
		CmemoryTrace& m_memoryTrace;
		CnetworkPoolCallback& m_callback;
		bool m_bWantExit;

		// Internal thread.
		std::thread *m_thread;

		// Data which exchanged between internal and external.
		std::mutex m_lock;
		std::unordered_map<CnetworkNode, bool, __network_hash> m_pendingBind;
		struct __pending_send
		{
			CnetworkNode m_node;
			Cbuffer m_data;
			bool m_bAutoConnect;

			__pending_send(CmemoryTrace& trace)
				:m_data(&trace) {}
			__pending_send(CmemoryTrace& trace, const CnetworkNode& node, const void *data, const size_t length, const bool bAutoConnect)
				:m_node(node), m_data(&trace, data, length), m_bAutoConnect(bAutoConnect) {}

			__pending_send(const __pending_send& another) = delete;
			__pending_send(__pending_send&& another)
				:m_node(std::move(another.m_node)), m_data(std::move(another.m_data)), m_bAutoConnect(another.m_bAutoConnect) {}

			const __pending_send& operator=(const __pending_send& another) = delete;
			const __pending_send& operator=(__pending_send&& another)
			{
				m_node = std::move(another.m_node);
				m_data = std::move(another.m_data);
				m_bAutoConnect = another.m_bAutoConnect;
				return *this;
			}
		};
		std::deque<__pending_send> m_pendingSend;
		std::unordered_map<CnetworkNode, bool, __network_hash> m_pendingClose;
		
		//
		// Following data must be accessed by internal thread.
		//

		// Use round robin to send message on UDP.
		int m_udpIndex;

		// Loop must be initialized in internal work thread.
		uv_loop_t m_loop;
		Casync *m_wakeup;
		std::unordered_map<CnetworkNode, Ctcp *, __network_hash> m_tcpServers;
		std::vector<Cudp *> m_udpServers;
		std::unordered_map<CnetworkNode, Ctcp *, __network_hash> m_node2stream;
		std::unordered_set<Ctcp *> m_connecting;
		std::unordered_map<CnetworkNode, std::vector<uv_buf_t>, __network_hash> m_waitingSend; // Waiting for connection complete.
		std::unordered_set<Ctcp *> m_waitingClose; // Waiting for sending complete and close.

		friend void tcp_alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
		friend void on_tcp_timeout(uv_timer_t *handle);
		friend void reset_tcp_idle_timeout_may_set_nullptr(Ctcp *& tcp);
		friend void on_tcp_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf);
		friend void on_tcp_write_done(uv_write_t *req, int status);
		friend void on_new_connection(uv_stream_t *server, int status);
		friend void on_connect_done(uv_connect_t *req, int status);
		friend void on_wakeup(uv_async_t *async);

		inline Ctcp *getStreamByNode(const CnetworkNode& node);
		inline void dropWaiting(const CnetworkNode& node);
		inline void pushWaiting(const CnetworkNode& node, Cbuffer& data);
		inline void dropWriteAndFree_set_nullptr(const CnetworkNode& node, __write_with_info *& writeInfo);
		inline __write_with_info *getWriteFromWaitingByNode(const CnetworkNode& node);

		// Caution! Call following function(s) may cause iterator of m_node2stream and m_waitingSend invalid.
		//          Following function(s) may set nullptr to tcp.
		inline void startupTcpConnection_may_set_nullptr(Ctcp *& tcp);
		inline void shutdownTcpConnection_set_nullptr(Ctcp *& tcp, bool bAlwaysNotify = false);

		void internalThread();

	public:
		// throw when fail.
		CnetworkPool(const __preferred_network_settings& settings, CmemoryTrace& memoryTrace, CnetworkPoolCallback& callback)
			:m_state(initializing), m_settings(settings), m_memoryTrace(memoryTrace), m_callback(callback), m_bWantExit(false), m_udpIndex(0), m_wakeup(nullptr)
		{
			m_thread = m_memoryTrace._new_throw<std::thread>(&CnetworkPool::internalThread, this); // May throw.
			while (initializing == m_state)
				std::this_thread::yield();
			if (m_state != good)
				throw(-1);
		}
		~CnetworkPool()
		{
			m_bWantExit = true;
			uv_async_send(m_wakeup->getAsync());
			m_thread->join();
			m_memoryTrace._delete_set_nullptr<std::thread>(m_thread);
		}

		// No copy, no move.
		CnetworkPool(const CnetworkPool& another) = delete;
		CnetworkPool(CnetworkPool&& another) = delete;
		const CnetworkPool& operator=(const CnetworkPool& another) = delete;
		const CnetworkPool& operator=(CnetworkPool&& another) = delete;

		inline const __preferred_network_settings& getSettings() const
		{
			return m_settings;
		}

		inline CmemoryTrace& getMemoryTrace()
		{
			return m_memoryTrace;
		}
		
		void bind(const CnetworkNode& node, const bool bBind = true)
		{
			{
				std::lock_guard<std::mutex> guard(m_lock);
				m_pendingBind.insert(std::make_pair(node, bBind));
			}
			uv_async_send(m_wakeup->getAsync());
		}

		void send(const CnetworkNode& node, const void *data, const size_t length, const bool bAutoConnect = false)
		{
			if (0 == length || nullptr == data)
				return;
			__pending_send temp(m_memoryTrace, node, data, length, bAutoConnect);
			{
				std::lock_guard<std::mutex> guard(m_lock);
				m_pendingSend.push_back(__pending_send(m_memoryTrace));
				m_pendingSend.back() = std::move(temp);
			}
			uv_async_send(m_wakeup->getAsync());
		}

		void close(const CnetworkNode& node, const bool bForceClose = false)
		{
			{
				std::lock_guard<std::mutex> guard(m_lock);
				m_pendingClose.insert(std::make_pair(node, bForceClose));
			}
			uv_async_send(m_wakeup->getAsync());
		}
	};
}
