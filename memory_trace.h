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

#include "np_dbg.h"

namespace NETWORK_POOL
{
	class CmemoryTrace
	{
	private:
		std::atomic<int64_t> m_size;
		std::atomic<int32_t> m_count;

	public:
		CmemoryTrace() :m_size(0), m_count(0) {}

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

		inline void *_malloc_throw(size_t sz)
		{
			size_t allocSize = sizeof(size_t) + sz;
			if (allocSize < sz) // In case of overflow.
			{
				NP_FPRINTF((stderr, "malloc_throw size overflow.\n"));
				throw(-1);
			}
			void *ptr = new unsigned char[allocSize]; // May throw.
			*(size_t *)ptr = allocSize;
			m_size += allocSize;
			++m_count;
		#if NP_DBG
			memset((size_t *)ptr + 1, -1, sz);
		#endif
			return (size_t *)ptr + 1;
		}

		inline void *_malloc_no_throw(size_t sz)
		{
			size_t allocSize = sizeof(size_t) + sz;
			if (allocSize < sz) // In case of overflow.
			{
				NP_FPRINTF((stderr, "malloc_no_throw size overflow.\n"));
				throw(-1);
			}
			void *ptr = new (std::nothrow) unsigned char[allocSize]; // May nullptr.
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
			delete[](unsigned char *)org;
			ptr = nullptr;
		}

		template<class T, class... Args>
		inline T *_new_throw(Args... args)
		{
			T *obj = new T(args...); // May throw.
			m_size += sizeof(T);
			++m_count;
			return obj;
		}

		template<class T, class... Args>
		inline T *_new_no_throw(Args... args)
		{
			T *obj = new (std::nothrow) T(args...); // May nullptr;
			if (nullptr == obj)
				return nullptr;
			m_size += sizeof(T);
			++m_count;
			return obj;
		}

		template<class T>
		inline void _delete_set_nullptr(T *& ptr)
		{
			if (nullptr == ptr)
				return;
			delete ptr;
			m_size -= sizeof(T);
			--m_count;
			ptr = nullptr;
		}
	};
}
