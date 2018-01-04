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
	class ChttpServer : public CnetworkPoolCallback
	{
	private:
		CmemoryTrace& m_memoryTrace;

		std::unordered_map<CnetworkNode, ChttpContext, __network_hash> m_context;
		CnetworkPool *m_pool;

	public:
		ChttpServer(CmemoryTrace& memoryTrace) :m_memoryTrace(memoryTrace), m_pool(nullptr) {}

		void setNetworkPool(CnetworkPool *pool)
		{
			m_pool = pool;
		}

		void allocateMemoryForMessage(const CnetworkNode& node, size_t suggestedSize, void *& buffer, size_t& lenght)
		{
			auto it = m_context.find(node);
			if (it != m_context.end())
				it->second.getBuffer(buffer, lenght);
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
					if (ctx.isGood())
					{
						std::string method, uri, version;
						ctx.getInfo(method, uri, version);
						NP_FPRINTF((stdout, "http req: \'%s\' \'%s\'.\n", method.c_str(), uri.c_str()));
						if (m_pool != nullptr)
						{
							static const std::string resp("HTTP/1.1 200 OK\r\nContent-Length: 600\r\n\r\n"
								"0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
								"0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
								"0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
								"0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
								"0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
								"0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789");
							m_pool->send(node, resp.c_str(), resp.length());
							if (ctx.reinitForNext())
								goto _again;
							else
								m_pool->close(node);
						}
					}
					else if (m_pool != nullptr)
						m_pool->close(node);
				}
			}
		}

		void drop(const CnetworkNode& node, const void *data, const size_t length)
		{
			NP_FPRINTF((stdout, "pkt drop: [%s]:%u.\n", node.getIp().c_str(), node.getPort()));
		}

		void bindStatus(const CnetworkNode& node, const bool bSuccess)
		{
			NP_FPRINTF((stdout, "bind: [%s]:%u %s.\n", node.getIp().c_str(), node.getPort(), bSuccess ? "success" : "fail"));
		}
		void connectionStatus(const CnetworkNode& node, const bool bSuccess)
		{
			NP_FPRINTF((stdout, "connection: from-[%s]:%u %s.\n", node.getIp().c_str(), node.getPort(), bSuccess ? "success" : "fail"));
			if (bSuccess)
			{
				auto ib = m_context.insert(std::make_pair(node, ChttpContext(m_memoryTrace)));
				ib.first->second.init();
			}
			else
				m_context.erase(node);
		}
	};
}
