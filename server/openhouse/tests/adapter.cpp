// SPDX-License-Identifier: GPL-3.0-or-later
#include "wivrn_config.h"
// Reuse the existing real-device-callback tests against the copied adapter.
#define main openhouse_hotplug_main
#include "openhouse_hotplug.cpp"
#undef main
#include "openhouse_interface.h"
#if WIVRN_FEATURE_STEAMVR_LIGHTHOUSE
#include "steamvr_lh_interface.h"
#endif

namespace
{
unsigned initialized = 0, cleaned = 0;
bool reject_factory = false;
class Provider final : public vr::IServerTrackedDeviceProvider
{
public:
    vr::EVRInitError Init(vr::IVRDriverContext * context) override
    {
        ++initialized;
        require(context != nullptr, "linked provider receives the copied OpenVR host");
        return vr::VRInitError_None;
    }
    void Cleanup() override { ++cleaned; }
    const char * const * GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    void RunFrame() override {}
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}
} provider;
}
// Executable interposition supplies a synthetic provider: never open USB devices.
extern "C" void * wivrn_openhouse_factory(const char * name, int * error)
{
    require(std::strcmp(name, vr::IServerTrackedDeviceProvider_Version) == 0, "correct provider interface");
    *error = reject_factory ? vr::VRInitError_Init_InterfaceNotFound : vr::VRInitError_None;
    return reject_factory ? nullptr : &provider;
}

int main(int argc, char ** argv)
{
    setenv("LH_DISCOVER_WAIT_MS", "0", true);
    xrt_system_devices system{};
    reject_factory = true;
    require(openhouse_create_devices(nullptr, &system) != XRT_SUCCESS, "factory failure propagates");
    require(initialized == 0, "failed factory does not initialize a provider");
    reject_factory = false;
    require(openhouse_create_devices(nullptr, &system) == XRT_SUCCESS, "linked provider starts without SteamVR");
    require(initialized == 1 && system.static_xdev_count == 0, "empty runtime accepts later hotplug");
#if WIVRN_FEATURE_STEAMVR_LIGHTHOUSE
    require(!steamvr_lh_standby_trackers(nullptr), "original host remains inactive");
#endif
    openhouse_set_device_added_callback(&system, [](xrt_device *, void *) {}, nullptr);
    require(openhouse_standby_trackers(&system), "standby reaches the selected host");
    openhouse_stop(&system);
    require(cleaned == 1, "provider cleaned up once after frame-thread stop");
    openhouse_stop(&system);
    require(cleaned == 1, "repeated stop is harmless");
    return openhouse_hotplug_main(argc, argv);
}
