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

#include "http_server.h"

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
	ChttpTask::ChttpTask(CmemoryTrace& memoryTrace, ChttpServer& server, const CnetworkNode& node)
		:m_server(server), m_canceled(false), m_node(node), m_context(memoryTrace)
	{
		m_server.addReferenceTask(this);
	}

	ChttpTask::~ChttpTask()
	{
		m_server.deleteReferenceTask(this);
	}

	void ChttpTask::run()
	{
		if (m_canceled)
			return;
		std::string method, uri, version;
		m_context.getInfo(method, uri, version);
		NP_FPRINTF((stdout, "http req: \'%s\' \'%s\'.\n", method.c_str(), uri.c_str()));
		CnetworkPool *pool = m_server.getNetworkPool();
		if (pool != nullptr)
		{
			static const std::string resp("HTTP/1.1 200 OK\r\nConnection:Keep-Alive\r\nContent-Length: 600\r\n\r\n"
				"0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
				"0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
				"0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
				"0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
				"0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789"
				"0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789");
			pool->send(m_node, resp.c_str(), resp.length());
			if (!m_context.isKeepAlive())
				pool->close(m_node);
		}
	}
}
