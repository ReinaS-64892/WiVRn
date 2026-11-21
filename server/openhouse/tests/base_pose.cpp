// SPDX-License-Identifier: GPL-3.0-or-later
// Reuse upstream's OpenVR host, but feed the real provider synthetic room states
// instead of starting a session or requiring a hardware capture.
#define main upstream_replay_main
#include "mock_host.cpp"
#undef main
#include "driver_lighthouse.cpp"

int main()
{
    Host host;
    CHECK(vr::InitServerDriverContext(&host) == vr::VRInitError_None, "host initializes");
    DeviceStatic info;
    info.serial = "LHB-12345678";
    info.role = lh::DeviceRole::BaseStation;
    Device device(info);
    g.devices.push_back({info.serial, vr::TrackedDeviceClass_TrackingReference, &device});
    CHECK(device.Activate(0) == vr::VRInitError_None, "base activates");

    lh::BaseStationState base;
    base.seen = true;
    auto check = [&](bool valid, vr::ETrackingResult result) {
        device.UpdateBase(base);
        auto pose = device.GetPose();
        const bool visible = valid && base.seen;
        CHECK(pose.poseIsValid == visible, "validity requires room readiness and live sightings");
        CHECK(pose.result == result, "tracking result matches room readiness");
        CHECK(pose.deviceIsConnected == base.seen, "connection follows live sightings");
        CHECK(g.devices[0].last.poseIsValid == visible && g.devices[0].last.result == result,
              "the host receives the same readiness as GetPose");
    };
    check(false, vr::TrackingResult_Uninitialized);
    base.known = true;
    check(false, vr::TrackingResult_Calibrating_InProgress);
    base.origin = true;
    check(false, vr::TrackingResult_Calibrating_InProgress);
    base.origin = false;
    base.locked = true;
    check(false, vr::TrackingResult_Calibrating_InProgress);
    base.locked = false;
    // This is the regression: sufficiently learned, but fewer than the 100
    // keyframes required to lock. It must be usable by alignment clients.
    base.learned = true;
    base.worldFromBase.position = {1, 2, 3};
    check(true, vr::TrackingResult_Running_OK);
    CHECK(g.devices[0].last.vecPosition[0] == 1 && g.devices[0].last.vecPosition[1] == 2 &&
          g.devices[0].last.vecPosition[2] == 3, "learned base pose reaches the host");
    base.locked = true;
    check(true, vr::TrackingResult_Running_OK);
    base.locked = false;
    base.origin = true;
    check(true, vr::TrackingResult_Running_OK);
    base.origin = false;
    base.learned = false;
    check(false, vr::TrackingResult_Calibrating_InProgress);
    base.learned = true;
    base.seen = false;
    check(true, vr::TrackingResult_Running_OK);
    base.known = false;
    check(false, vr::TrackingResult_Uninitialized);
    device.Deactivate();
    vr::CleanupDriverContext();
    return failures ? 1 : 0;
}
