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

#include <atomic>
#include <cstdlib>

#include "fast_allocator.h"
#include "np_dbg.h"

#if NP_DBG
	#include <cstring>
#endif

namespace NETWORK_POOL
{
	class CmemoryTrace
	{
	private:
		std::atomic<int64_t> m_size;
		std::atomic<int32_t> m_count;

	public:
		CmemoryTrace() :m_size(0), m_count(0) {}

		// No copy, no move.
		CmemoryTrace(const CmemoryTrace& another) = delete;
		CmemoryTrace(CmemoryTrace&& another) = delete;
		const CmemoryTrace& operator=(const CmemoryTrace& another) = delete;
		const CmemoryTrace& operator=(CmemoryTrace&& another) = delete;

		inline uint32_t getObjectCount() const
		{
			return m_count;
		}
		inline uint64_t getMemoryUsage() const
		{
			return m_size;
		}

		//
		// Following function(s) are internal used only.
		//

		inline void *_malloc_throw(const size_t sz)
		{
			size_t allocSize = sizeof(size_t) + sz;
			if (allocSize < sz) // In case of overflow.
			{
				NP_FPRINTF((stderr, "malloc_throw size overflow.\n"));
				std::terminate();
			}
			void *ptr = __alloc(allocSize);
			if (nullptr == ptr)
				throw std::bad_alloc();
			*(size_t *)ptr = allocSize;
			m_size += allocSize;
			++m_count;
		#if NP_DBG
			memset((size_t *)ptr + 1, -1, sz);
		#endif
			return (size_t *)ptr + 1;
		}

		inline void *_malloc_no_throw(const size_t sz)
		{
			size_t allocSize = sizeof(size_t) + sz;
			if (allocSize < sz) // In case of overflow.
			{
				NP_FPRINTF((stderr, "malloc_no_throw size overflow.\n"));
				std::terminate();
			}
			void *ptr = __alloc(allocSize);
			if (nullptr == ptr)
				return nullptr;
			*(size_t *)ptr = allocSize;
			m_size += allocSize;
			++m_count;
		#if NP_DBG
			memset((size_t *)ptr + 1, -1, sz);
		#endif
			return (size_t *)ptr + 1;
		}

		template<class T>
		inline void _free_set_nullptr(T *& ptr)
		{
			if (nullptr == ptr)
				return;
			void *org = (size_t *)ptr - 1;
			size_t allocSize = *(size_t *)org;
			m_size -= allocSize;
			--m_count;
		#if NP_DBG
			memset(org, -1, allocSize);
		#endif
			__free(org, allocSize);
			ptr = nullptr;
		}

		template<class T, class... Args>
		inline T *_new_throw(Args&&... args)
		{
			T *ptr = (T *)_malloc_throw(sizeof(T));
			try
			{
				return new (ptr)T(std::forward<Args>(args)...);
			}
			catch (...)
			{
				_free_set_nullptr(ptr);
				throw;
			}
		}

		template<class T, class... Args>
		inline T *_new_no_throw(Args&&... args)
		{
			T *ptr = (T *)_malloc_no_throw(sizeof(T));
			if (nullptr == ptr)
				return nullptr;
			try
			{
				return new (ptr)T(std::forward<Args>(args)...);
			}
			catch (...)
			{
				_free_set_nullptr(ptr);
				throw; // Still throw when constructor throw.
			}
		}

		template<class T>
		inline void _delete_set_nullptr(T *& ptr)
		{
			if (nullptr == ptr)
				return;
			try
			{
				ptr->~T();
				_free_set_nullptr(ptr);
			}
			catch (...)
			{
				_free_set_nullptr(ptr);
				throw;
			}
		}
	};
}
