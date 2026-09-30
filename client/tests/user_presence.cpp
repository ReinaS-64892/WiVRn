// SPDX-License-Identifier: GPL-3.0-or-later

#include "xr/user_presence.h"
#include <cstdlib>
#include <iostream>

static void check(bool condition)
{
	if (not condition)
		std::abort();
}

int main()
{
	int first_handle, second_handle;
	const auto first = reinterpret_cast<XrSession>(&first_handle);
	const auto second = reinterpret_cast<XrSession>(&second_handle);
	xr::user_presence presence;
	auto event = [&](XrSession session, bool present) {
		return presence.update({
		        .type = XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT,
		        .next = nullptr,
		        .session = session,
		        .isUserPresent = XrBool32(present ? XR_TRUE : XR_FALSE),
		});
	};

	// No running session: no invented state, even if an event is supplied.
	check(not presence.get().has_value());
	check(not event(first, true));

	// Initial true and false received in the lobby survive until connection.
	for (bool initial: {true, false})
	{
		presence.begin(first, true);
		check(not presence.get().has_value());
		check(event(first, initial));
		check(presence.get() == initial);

		// A snapshot is a read, so reconnecting without a change retains the value.
		check(presence.get() == initial);
		check(presence.get() == initial);
		check(not event(second, not initial));
		check(presence.get() == initial);

		// Subsequent events update the state used by the same sender.
		check(event(first, not initial));
		check(presence.get() == not initial);
	}

	// Multiple lobby changes: the snapshot contains the latest actual event.
	check(event(first, true));
	check(event(first, false));
	check(event(first, true));
	check(presence.get() == true);

	// Ending and restarting the same session must discard the previous value.
	presence.reset();
	check(not presence.get().has_value());
	check(not event(first, false));
	presence.begin(first, true);
	check(not presence.get().has_value());
	check(event(first, false));
	check(presence.get() == false);

	// Session recreation rejects events from the old handle.
	presence.begin(second, true);
	check(not presence.get().has_value());
	check(not event(first, true));
	check(event(second, true));
	check(presence.get() == true);

	// Extension disabled / capability false: do not manufacture presence.
	presence.begin(second, false);
	check(not presence.get().has_value());
	check(not event(second, true));
	check(not event(second, false));
	check(not presence.get().has_value());

	std::cout << "user presence lifecycle tests passed\n";
}
