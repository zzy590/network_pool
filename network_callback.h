
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
		// Note: Drop before connection down notify means failed to send(maybe other reasons),
		//       and drop after connection down notify means failed by the down of the connection.
		virtual void drop(const CnetworkNode& node, const void *data, const size_t length) = 0;

		// Local bind notify.
		virtual void bindStatus(const CnetworkNode& node, const bool bSuccess) = 0;
		// Remote connection notify.
		// Note: No connection down notify if you send a message without auto connect when there is no connection established.
		virtual void connectionStatus(const CnetworkNode& node, const bool bSuccess) = 0;
	};
}
