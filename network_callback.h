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

#include "network_node.h"

namespace NETWORK_POOL
{
	class CnetworkPoolCallback
	{
	public:
		virtual ~CnetworkPoolCallback() {}

		// Every message which received will call allocate first before message function.
		// You can point buffer to a cache related to a connection to get better performance(no alloc and no copy).
		virtual void allocateMemoryForMessage(const CnetworkNode& node, size_t suggestedSize, void *& buffer, size_t& lenght) = 0;
		// Every allocate with a deallocate.
		virtual void deallocateMemoryForMessage(const CnetworkNode& node, void *buffer, size_t lenght) = 0;

		// Message received.
		virtual void message(const CnetworkNode& node, const void *data, const size_t length) = 0;

		// Message which want to send will be dropped.
		// Note: Drop before connection down notification means failed to send(maybe other reasons),
		//       and drop after connection down notification means failed by the down of the connection.
		virtual void drop(const CnetworkNode& node, const void *data, const size_t length) = 0;

		// Local bind notification.
		virtual void bindStatus(const CnetworkNode& node, const bool bSuccess) = 0;
		// Remote connection notification.
		// Note: No connection down notification if you send a message without auto connect when there is no connection established.
		virtual void connectionStatus(const CnetworkNode& node, const bool bSuccess) = 0;

		// Error notifications of binded port.
		// Note: Error on tcp connection will cause connection down.
		//       But error on udp and tcp listening socket will send following notifications.
		//       Node is the local binded port.
		virtual void tcpListenError(const CnetworkNode& node, int err) {}
		virtual void udpSendError(const CnetworkNode& node, int err) {}
		virtual void udpRecvError(const CnetworkNode& node, int err) {}
	};
}
