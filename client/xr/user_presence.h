// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <openxr/openxr.h>

namespace xr
{
// Accessed on the application thread, including while a connection is created.
class user_presence
{
	XrSession session = XR_NULL_HANDLE;
	std::optional<bool> present;

public:
	void begin(XrSession new_session, bool supported)
	{
		session = supported ? new_session : XR_NULL_HANDLE;
		present.reset();
	}

	void reset()
	{
		session = XR_NULL_HANDLE;
		present.reset();
	}

	bool update(const XrEventDataUserPresenceChangedEXT & event)
	{
		if (session == XR_NULL_HANDLE or event.session != session)
			return false;
		present = event.isUserPresent == XR_TRUE;
		return true;
	}

	std::optional<bool> get() const
	{
		return present;
	}
};
} // namespace xr
