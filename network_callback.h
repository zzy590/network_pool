
#pragma once

#include "network_node.h"

namespace NETWORK_POOL
{
	class CnetworkPoolCallback
	{
	public:
		virtual ~CnetworkPoolCallback() {}

		// Message received.
		virtual void message(const CnetworkNode& node, const void *data, const size_t length) = 0;

		// Message which dropped due to the connection failed.
		// Note: Drop before connection down notify means failed to send(maybe other reason),
		//       and drop after connection down notify means failed by the down of the connection.
		virtual void drop(const CnetworkNode& node, const void *data, const size_t length) = 0;

		// Local bind and remote connection notify.
		virtual void bindStatus(const CnetworkNode& node, const bool bSuccess) = 0;
		// Note: No connection down notify if you send a message with no auto connect when no connection.
		virtual void connectionStatus(const CnetworkNode& node, const bool bSuccess) = 0;
	};
}
