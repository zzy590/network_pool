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
#include <thread>
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
	static const size_t s_maxAllocatorSlot = 4096;
	#define set_max_store_number(_s, _n) { if ((_s) < s_maxAllocatorSlot) s_maxAllocatorStoreNumber[(_s)] = (_n); }

	static std::mutex s_globalLock;
	static void *s_allocatorStore[s_maxAllocatorSlot] = { 0 };
	static size_t s_allocatorStoreCount[s_maxAllocatorSlot] = { 0 };
	static size_t s_maxAllocatorStoreNumber[s_maxAllocatorSlot] = { 0 };

	static std::once_flag s_storeNumberInit;

	static inline void initStoreNumber()
	{
		std::call_once(s_storeNumberInit, []()
		{
			set_max_store_number(sizeof(CnetworkNode), 512);
			set_max_store_number(sizeof(Cbuffer), 512);
			set_max_store_number(sizeof(uv_shutdown_t) + sizeof(size_t), 1024);
			set_max_store_number(sizeof(uv_connect_t) + sizeof(size_t), 1024);
			set_max_store_number(sizeof(CnetworkPool::__write_with_info) + sizeof(size_t), 4096);
			set_max_store_number(sizeof(CnetworkPool::__udp_send_with_info) + sizeof(size_t), 4096);
			set_max_store_number(sizeof(Casync) + sizeof(size_t), 0);
			set_max_store_number(sizeof(Ctcp) + sizeof(size_t), 16384);
			set_max_store_number(sizeof(Cudp) + sizeof(size_t), 0);
		});
	}

	void *__alloc(std::size_t size)
	{
		initStoreNumber();
		FA_FPRINTF((stderr, "fa alloc %u.\n", size));
		if (size >= s_maxAllocatorSlot || 0 == s_maxAllocatorStoreNumber[size])
			return malloc(size);
		FA_FPRINTF((stderr, "fa use store.\n"));
		void *take = nullptr;
		s_globalLock.lock();
		if (s_allocatorStore[size] != nullptr)
		{
			take = s_allocatorStore[size];
			s_allocatorStore[size] = *(void **)take;
			--s_allocatorStoreCount[size];
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
		if (size >= s_maxAllocatorSlot || 0 == s_maxAllocatorStoreNumber[size])
			return free(ptr);
		FA_FPRINTF((stderr, "fa use store.\n"));
		s_globalLock.lock();
		if (s_allocatorStoreCount[size] < s_maxAllocatorStoreNumber[size])
		{
			*(void **)ptr = s_allocatorStore[size];
			s_allocatorStore[size] = ptr;
			++s_allocatorStoreCount[size];
			ptr = nullptr;
		}
		s_globalLock.unlock();
		if (ptr != nullptr)
			free(ptr);
	}
}
