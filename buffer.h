
#pragma once

#include <cstring>

#include "uv.h"

#include "memory_trace.h"

namespace NETWORK_POOL
{
	class Cbuffer
	{
	private:
		CmemoryTrace& m_trace;

		void *m_data;
		size_t m_length;
		size_t m_maxLength;

		inline void transfer(uv_buf_t& buf) // Internal use only.
		{
			buf.base = (char *)m_data;
		#ifdef _WIN32
			buf.len = (ULONG)m_length;
		#else
			buf.len = m_length;
		#endif
			m_data = nullptr;
			m_maxLength = m_length = 0;
		}

		friend void on_wakeup(uv_async_t *async);
		friend class CnetworkPool;

	public:
		Cbuffer(CmemoryTrace& trace)
			:m_trace(trace), m_data(nullptr), m_length(0), m_maxLength(0) {}
		Cbuffer(CmemoryTrace& trace, const size_t length)
			:m_trace(trace)
		{
			if (0 == length)
			{
				m_data = nullptr;
				m_maxLength = m_length = 0;
			}
			else
			{
				m_data = m_trace._malloc_throw(length);
				m_maxLength = m_length = length;
			}
		}
		Cbuffer(CmemoryTrace& trace, const void *data, const size_t length)
			:m_trace(trace)
		{
			m_data = m_trace._malloc_throw(length);
			memcpy(m_data, data, length);
			m_maxLength = m_length = length;
		}
		Cbuffer(const Cbuffer& another)
			:m_trace(another.m_trace)
		{
			m_data = m_trace._malloc_throw(another.m_length);
			memcpy(m_data, another.m_data, another.m_length);
			m_maxLength = m_length = another.m_length;
		}
		~Cbuffer()
		{
			if (m_data != nullptr)
				m_trace._free_set_nullptr(m_data);
		}

		const Cbuffer& operator=(const Cbuffer& another)
		{
			if (another.m_length <= m_maxLength)
			{
				memcpy(m_data, another.m_data, another.m_length);
				m_length = another.m_length;
			}
			else
			{
				if (m_data != nullptr)
					m_trace._free_set_nullptr(m_data);
				m_data = m_trace._malloc_throw(another.m_length);
				memcpy(m_data, another.m_data, another.m_length);
				m_maxLength = m_length = another.m_length;
			}
			return *this;
		}

		inline void set(const void *data, const size_t length)
		{
			if (length <= m_maxLength)
			{
				memcpy(m_data, data, length);
				m_length = length;
			}
			else
			{
				if (m_data != nullptr)
					m_trace._free_set_nullptr(m_data);
				m_data = m_trace._malloc_throw(length);
				memcpy(m_data, data, length);
				m_maxLength = m_length = length;
			}
		}

		inline void resize(const size_t preferLength, const size_t validLength = 0)
		{
			if (preferLength <= m_maxLength)
			{
				m_length = preferLength;
				return;
			}
			size_t copy = validLength > m_length ? m_length : validLength;
			if (copy > 0)
			{
				void *newBuffer = m_trace._malloc_throw(preferLength);
				memcpy(newBuffer, m_data, copy);
				m_trace._free_set_nullptr(m_data);
				m_data = newBuffer;
				m_maxLength = m_length = preferLength;
			}
			else
			{
				if (m_data != nullptr)
					m_trace._free_set_nullptr(m_data);
				m_data = m_trace._malloc_throw(preferLength);
				m_maxLength = m_length = preferLength;
			}
		}

		inline void *getData() const
		{
			return m_data;
		}
		inline size_t getLength() const
		{
			return m_length;
		}
		inline size_t getMaxLength() const
		{
			return m_maxLength;
		}
	};
}