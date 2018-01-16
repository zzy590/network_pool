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

#include <mutex>
#include <cstdlib>

#include "uv.h"

#include "fast_allocator.h"
#include "network_node.h"
#include "buffer.h"
#include "network_pool.h"
#include "uv_wrapper.h"

#define FA_DBG 0
#if FA_DBG
	#include <stdio.h>
	#define FA_FPRINTF(_x) { fprintf _x; }
#else
	#define FA_FPRINTF(_x) {}
#endif

namespace NETWORK_POOL
{
	// Check.
	#define C_ASSERT(e) typedef char __C_ASSERT__[(e)?1:-1]
	C_ASSERT(sizeof(uv_shutdown_t) == sizeof(uv_connect_t));

	static std::mutex s_globalLock;
	enum __allocator_index
	{
		unknown_index = 0,             // Ignore this slot.
		network_node_index,            // sizeof(CnetworkNode)
		buffer_index,                  // sizeof(Cbuffer)
		uv_connect_shutdown_index,     // sizeof(uv_shutdown_t) + sizeof(size_t) == sizeof(uv_connect_t) + sizeof(size_t) Check before.
		tcp_write_index,               // sizeof(CnetworkPool::__write_with_info) + sizeof(size_t)
		udp_send_index,                // sizeof(CnetworkPool::__udp_send_with_info) + sizeof(size_t)
		casync_index,                  // sizeof(Casync) + sizeof(size_t)
		ctcp_index,                    // sizeof(Ctcp) + sizeof(size_t)
		cudp_index,                    // sizeof(Cudp) + sizeof(size_t)
		allocator_index_max
	};
	static void *s_allocatorStore[allocator_index_max] = { 0 };
	static size_t s_allocatorStoreCount[allocator_index_max] = { 0 };
	static const size_t s_maxStoreNumber[allocator_index_max] = {
		0,
		512,
		512,
		1024,
		4096,
		4096,
		0,
		16384,
		0
	};

	static inline __allocator_index size2index(std::size_t size)
	{
		switch (size)
		{
		case sizeof(CnetworkNode):
			return network_node_index;

		case sizeof(Cbuffer):
			return buffer_index;

		case sizeof(uv_connect_t) + sizeof(size_t) :
			return uv_connect_shutdown_index;

		case sizeof(CnetworkPool::__write_with_info) + sizeof(size_t):
			return tcp_write_index;

		case sizeof(CnetworkPool::__udp_send_with_info) + sizeof(size_t):
			return udp_send_index;

		case sizeof(Casync) + sizeof(size_t):
			return casync_index;

		case sizeof(Ctcp) + sizeof(size_t):
			return ctcp_index;

		case sizeof(Cudp) + sizeof(size_t):
			return cudp_index;

		default:
			return unknown_index;
		}
	}

	void *__alloc(std::size_t size)
	{
		FA_FPRINTF((stderr, "fa alloc %u.\n", size));
		__allocator_index index = size2index(size);
		if (unknown_index == index)
			return malloc(size);
		FA_FPRINTF((stderr, "fa use store.\n"));
		void *take = nullptr;
		s_globalLock.lock();
		if (s_allocatorStore[index] != nullptr)
		{
			take = s_allocatorStore[index];
			s_allocatorStore[index] = *(void **)take;
			--s_allocatorStoreCount[index];
		}
		s_globalLock.unlock();
		if (nullptr == take)
			return malloc(size < sizeof(void *) ? sizeof(void *) : size); // At least a pointer for linked list.
		return take;
	}

	void __free(void *ptr, std::size_t size)
	{
		if (nullptr == ptr)
			return;
		FA_FPRINTF((stderr, "fa free %u.\n", size));
		__allocator_index index = size2index(size);
		if (unknown_index == index)
			return free(ptr);
		FA_FPRINTF((stderr, "fa use store.\n"));
		s_globalLock.lock();
		if (s_allocatorStoreCount[index] < s_maxStoreNumber[index])
		{
			*(void **)ptr = s_allocatorStore[index];
			s_allocatorStore[index] = ptr;
			++s_allocatorStoreCount[index];
			ptr = nullptr;
		}
		s_globalLock.unlock();
		if (ptr != nullptr)
			free(ptr);
	}
}
