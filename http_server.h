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

#include <unordered_map>
#include <mutex>

#include "work_queue.h"
#include "network_callback.h"
#include "memory_trace.h"
#include "http_context.h"
#include "network_pool.h"

#include "np_dbg.h"

/*
#if !NP_DBG
	#include <stdio.h>
	#undef NP_FPRINTF
	#define NP_FPRINTF(_x) { fprintf _x; }
#endif
*/

namespace NETWORK_POOL
{
	class ChttpServer;

	class ChttpTask : public Ctask
	{
	private:
		ChttpServer& m_server;
		bool m_canceled;

		CnetworkNode m_node;
		ChttpContext m_context;

	public:
		ChttpTask(CmemoryTrace& memoryTrace, ChttpServer& server, const CnetworkNode& node);
		~ChttpTask();

		const CnetworkNode& getNode() const
		{
			return m_node;
		}
		ChttpContext& getContext()
		{
			return m_context;
		}

		void cancel()
		{
			m_canceled = true;
		}

		void run();
	};

	class ChttpServer : public CnetworkPoolCallback
	{
	private:
		CmemoryTrace& m_memoryTrace;

		std::unordered_map<CnetworkNode, ChttpContext, __network_hash> m_context;
		CnetworkPool *m_pool;

		std::mutex m_taskLock;
		std::unordered_multimap<CnetworkNode, ChttpTask *, __network_hash> m_tasks;

		CworkQueue m_workQueue;

	public:
		ChttpServer(CmemoryTrace& memoryTrace, const size_t nThread)
			:m_memoryTrace(memoryTrace), m_pool(nullptr), m_workQueue(nThread) {}

		void setNetworkPool(CnetworkPool *pool)
		{
			m_pool = pool;
		}
		CnetworkPool *getNetworkPool() const
		{
			return m_pool;
		}

		void addReferenceTask(ChttpTask *task)
		{
			std::lock_guard<std::mutex> guard(m_taskLock);
			m_tasks.insert(std::make_pair(task->getNode(), task));
		}
		void deleteReferenceTask(ChttpTask *task)
		{
			std::lock_guard<std::mutex> guard(m_taskLock);
			auto range = m_tasks.equal_range(task->getNode());
			for (auto it = range.first; it != range.second; ++it)
			{
				if (it->second == task)
				{
					m_tasks.erase(it);
					break;
				}
			}
		}

		void cancelTask(const CnetworkNode& node)
		{
			std::lock_guard<std::mutex> guard(m_taskLock);
			for (auto it = m_tasks.lower_bound(node); it != m_tasks.upper_bound(node); ++it)
				it->second->cancel();
		}

		void allocateMemoryForMessage(const CnetworkNode& node, size_t suggestedSize, void *& buffer, size_t& lenght)
		{
			auto it = m_context.find(node);
			if (it != m_context.end())
				it->second.prepareBuffer(buffer, lenght);
		}
		void deallocateMemoryForMessage(const CnetworkNode& node, void *buffer, size_t lenght)
		{
		}

		void message(const CnetworkNode& node, const void *data, const size_t length)
		{
			auto it = m_context.find(node);
			if (it != m_context.end())
			{
				ChttpContext& ctx = it->second;
				ctx.recvPush(length);
			_again:
				if (ctx.analysis())
				{
					if (ctx.isGood() && m_pool != nullptr)
					{
						ChttpTask *task = m_memoryTrace._new_no_throw<ChttpTask>(m_memoryTrace, *this, node);
						if (nullptr == task)
							m_pool->close(node);
						else
						{
							ctx.reinitForNext(task->getContext());
							m_workQueue.pushTask(task,
								[this](Ctask *task)
							{
								ChttpTask *httpTask = (ChttpTask *)task; // We should use same T to delete or we may get error on memory size trace.
								this->m_memoryTrace._delete_set_nullptr<ChttpTask>(httpTask);
							});
							goto _again;
						}
					}
					else if (m_pool != nullptr)
						m_pool->close(node);
				}
			}
		}

		void drop(const CnetworkNode& node, const void *data, const size_t length)
		{
			NP_FPRINTF((stdout, "pkt drop: [%s]:%u.\n", node.getSockaddr().getIp().c_str(), node.getSockaddr().getPort()));
		}

		void bindStatus(const CnetworkNode& node, const bool bSuccess)
		{
			NP_FPRINTF((stdout, "bind: [%s]:%u %s.\n", node.getSockaddr().getIp().c_str(), node.getSockaddr().getPort(), bSuccess ? "success" : "fail"));
		}
		void connectionStatus(const CnetworkNode& node, const bool bSuccess)
		{
			NP_FPRINTF((stdout, "connection: from-[%s]:%u %s.\n", node.getSockaddr().getIp().c_str(), node.getSockaddr().getPort(), bSuccess ? "success" : "fail"));
			if (bSuccess)
				m_context.insert(std::make_pair(node, ChttpContext(m_memoryTrace)));
			else
			{
				m_context.erase(node);
				cancelTask(node);
			}
		}
	};
}
