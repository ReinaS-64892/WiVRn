// SPDX-License-Identifier: GPL-3.0-or-later
#include "imgui.h"
#include "utils/i18n.h"
#include "version.h"
#include "video_diagnostics.h"
#include <spdlog/fmt/fmt.h>

bool wivrn::gui::video_recording_button()
{
	auto & recorder = video_diagnostics::get();
	auto status = recorder.snapshot();
	bool started = false;
	if (ImGui::Button(status.recording ? _S("Stop diagnostic recording") : _S("Record diagnostics")))
	{
		if (status.recording)
			recorder.stop();
		else if (recorder.start())
		{
			started = true;
			recorder.record({.kind = "client", .detail = wivrn::git_commit});
		}
		status = recorder.snapshot();
	}
	if (!status.path.empty())
		ImGui::TextWrapped("%s", fmt::format(_F("Diagnostic log: {}"), status.path.string()).c_str());
	if (!status.error.empty())
		ImGui::TextWrapped("%s", fmt::format(_F("Recording error: {}"), status.error).c_str());
	if (status.lost_events)
		ImGui::TextWrapped("%s", fmt::format(_F("Recording queue overflow: {} events lost"), status.lost_events).c_str());
	return started;
}
