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

#include "network_node.h"
#include "network_pool.h"

namespace NETWORK_POOL
{
	// Data is in form of ([length:4bytes][data:length])+
	// Context is only for tcp, and udp can use static decode function.
	class CpeerContext
	{
	private:
		size_t m_maxBufferSize;

		Cbuffer m_buffer;
		size_t m_nowIndex;

		void init()
		{
			if (0 == m_buffer.getMaxLength()) // Only init at first time.
				m_buffer.resize(0x1000); // 4KB
		}

	public:
		CpeerContext(CmemoryTrace& memoryTrace, const size_t maxBufferSize = 0x1000000) // 16MB
			:m_maxBufferSize(maxBufferSize), m_buffer(memoryTrace), m_nowIndex(0) {}

		void prepareBuffer(void *& buffer, size_t& lenght)
		{
			init();
			if (m_buffer.getLength() - m_nowIndex < 0x800) // 2KB
			{
				if (m_buffer.getLength() * 2 > m_maxBufferSize)
					m_buffer.resize(m_maxBufferSize, m_nowIndex);
				else
					m_buffer.resize(m_buffer.getLength() * 2, m_nowIndex);
			}
			length = m_buffer.getLength() - m_nowIndex;
			if (0 == length)
				buffer = nullptr;
			else
				buffer = (char *)m_buffer.getData() + m_nowIndex;
		}

		void pushBuffer(size_t length)
		{
			if (m_nowIndex + length <= m_buffer.getLength())
				m_nowIndex += length;
		}

		void getContent(std::vector<Cbuffer>& buffers)
		{
			size_t nowCheck = 0;
			while (true)
			{
				if (m_nowIndex < nowCheck + sizeof(uint32_t))
					break;
				uint32_t packLength = *(const uint32_t *)((const unsigned char *)m_buffer.getData() + nowCheck);
				if (m_nowIndex < nowCheck + sizeof(uint32_t) + packLength)
					break;
				// Copy to buffer.
				buffers.push_back(Cbuffer((const unsigned char *)m_buffer.getData() + nowCheck + sizeof(uint32_t), packLength));
				nowCheck += sizeof(uint32_t) + packLength;
			}
			if (nowCheck > 0)
			{
				// Reinit for next.
				size_t extra = m_nowIndex - nowCheck;
				unsigned char *ptr = (unsigned char *)m_buffer.getData();
				memmove(ptr, ptr + nowCheck, extra);
				m_nowIndex = extra;
			}
		}

		// For udp decode.
		static void getContent(const void *data, const size_t length, std::vector<Cbuffer>& buffers)
		{
			size_t nowCheck = 0;
			while (true)
			{
				if (length < nowCheck + sizeof(uint32_t))
					break;
				uint32_t packLength = *(const uint32_t *)((const unsigned char *)data + nowCheck);
				if (length < nowCheck + sizeof(uint32_t) + packLength)
					break;
				// Copy to buffer.
				buffers.push_back(Cbuffer((const unsigned char *)data + nowCheck + sizeof(uint32_t), packLength));
				nowCheck += sizeof(uint32_t) + packLength;
			}
		}
	};

	class CpeerServer : public CnetworkPoolCallback
	{
	private:
		CmemoryTrace& m_memoryTrace;
		CnetworkPool *m_pool;

		std::unordered_map<CnetworkNode, CpeerContext, __network_hash> m_tcpContext;

	public:
		CpeerServer(CmemoryTrace& memoryTrace)
			:m_memoryTrace(memoryTrace), m_pool(nullptr) {}

		void setNetworkPool(CnetworkPool *pool)
		{
			m_pool = pool;
		}
		CnetworkPool *getNetworkPool() const
		{
			return m_pool;
		}

		void allocateMemoryForMessage(const CnetworkNode& node, size_t suggestedSize, void *& buffer, size_t& lenght)
		{
			if (CnetworkNode::protocol_udp == node.getProtocol())
			{
				// Udp packet.
				buffer = m_memoryTrace._malloc_no_throw(suggestedSize);
				if (buffer != nullptr)
					lenght = suggestedSize;
				else
					lenght = 0;
			}
			else
			{
				auto it = m_tcpContext.find(node);
				if (it != m_tcpContext.end())
					it->second.prepareBuffer(buffer, lenght); // Tcp.
			}
		}
		void deallocateMemoryForMessage(const CnetworkNode& node, void *buffer, size_t lenght)
		{
			if (CnetworkNode::protocol_udp == node.getProtocol())
				m_memoryTrace._free_set_nullptr(buffer); // Udp packet.
		}

		void message(const CnetworkNode& node, const void *data, const size_t length)
		{
			std::vector<Cbuffer> buffers;
			if (CnetworkNode::protocol_udp == node.getProtocol())
				CpeerContext::getContent(data, length, buffers); // Udp packet.
			else
			{
				auto it = m_tcpContext.find(node);
				if (it != m_tcpContext.end())
				{
					ChttpContext& ctx = it->second;
					ctx.recvPush(length);
					ctx.getContent(buffers);
				}
			}
			for (auto& buffer : buffers)
			{
				// Dealing with content.

			}
		}

		void drop(const CnetworkNode& node, const void *data, const size_t length) {}

		void bindStatus(const CnetworkNode& node, const bool bSuccess) {}
		void connectionStatus(const CnetworkNode& node, const bool bSuccess)
		{
			if (bSuccess)
				m_tcpContext.insert(std::make_pair(node, CpeerContext(m_memoryTrace)));
			else
				m_tcpContext.erase(node);
		}
	};
}
