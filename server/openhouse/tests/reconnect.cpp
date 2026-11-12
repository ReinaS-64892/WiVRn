// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual USB reader/probe threads with one persistent HID dongle.
#include "usb_transport.hpp"
#include <hidapi/hidapi.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace std::chrono_literals;
namespace {
constexpr uint32_t tracker2018 = 0x9bb5412d, tracker3 = 0x5d3d7a83;
constexpr const char * serial2018 = "LHR-9BB5412D", * serial3 = "LHR-5D3D7A83";
struct Radio {
    std::mutex mutex;
    uint32_t serial = tracker2018;
    unsigned opens = 0, closes = 0;
    std::deque<std::vector<unsigned char>> reports;
} radio;

void require(bool value, const char * message)
{
    if (!value) throw std::runtime_error(message);
}
template <typename Predicate> void wait_for(Predicate predicate, const char * message)
{
    const auto end = std::chrono::steady_clock::now() + 8s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < end, message);
        std::this_thread::sleep_for(5ms);
    }
}
void switch_to(uint32_t serial, bool notify)
{
    std::lock_guard lock(radio.mutex);
    radio.serial = serial;
    if (notify) radio.reports.push_back({0x26});
}
void imu(uint8_t tick)
{
    // A Wv2 IMU sample, matching upstream's WirelessReconnect fixture.
    std::vector<unsigned char> report(30);
    report[0] = 0x23; report[1] = tick; report[2] = 15;
    report[4] = 0xe8; report[5] = 1;
    int16_t acceleration = 16384;
    std::memcpy(report.data() + 6, &acceleration, sizeof acceleration);
    std::lock_guard lock(radio.mutex);
    radio.reports.push_back(std::move(report));
}
struct Sink : lh::InputSinkBase {
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> attached;
    std::vector<std::string> detached;
    std::vector<std::pair<std::string, lh::ImuSample>> samples;
    void OnDeviceAttached(const lh::DeviceId & id, const std::string & config) override {
        std::lock_guard lock(mutex); attached.emplace_back(id, config);
    }
    void OnDeviceDetached(const lh::DeviceId & id) override {
        std::lock_guard lock(mutex); detached.push_back(id);
    }
    void OnImu(const lh::DeviceId & id, const lh::ImuSample & sample) override {
        std::lock_guard lock(mutex); samples.emplace_back(id, sample);
    }
    bool has_attachment(const char * id, size_t count = 1) {
        std::lock_guard lock(mutex);
        return size_t(std::count_if(attached.begin(), attached.end(), [&](const auto & a) {
            return a.first == id;
        })) >= count;
    }
    size_t sample_count() { std::lock_guard lock(mutex); return samples.size(); }
} sink;
}

struct hid_device_ {
    std::vector<unsigned char> config;
    size_t offset = 0;
};
// These definitions satisfy lh_usb's HID references before the real hidapi
// archive is considered. No test operation reaches the operating system's HID.
extern "C" {
int hid_init() { return 0; }
hid_device_info * hid_enumerate(unsigned short, unsigned short)
{
    auto * info = new hid_device_info{};
    info->path = strdup("synthetic-watchman");
    info->serial_number = wcsdup(L"39B40C7D41");
    info->vendor_id = 0x28de; info->product_id = 0x2101;
    info->interface_number = 0;
    return info;
}
void hid_free_enumeration(hid_device_info * info)
{
    free(info->path); free(info->serial_number); delete info;
}
hid_device * hid_open_path(const char *)
{
    std::lock_guard lock(radio.mutex); ++radio.opens;
    return new hid_device_;
}
void hid_close(hid_device * device)
{
    std::lock_guard lock(radio.mutex); ++radio.closes; delete device;
}
int hid_set_nonblocking(hid_device *, int) { return 0; }
int hid_read(hid_device *, unsigned char * data, size_t length)
{
    std::lock_guard lock(radio.mutex);
    if (radio.reports.empty()) return 0;
    auto report = std::move(radio.reports.front()); radio.reports.pop_front();
    require(report.size() <= length, "HID report fits");
    std::memcpy(data, report.data(), report.size());
    return int(report.size());
}
int hid_send_feature_report(hid_device *, const unsigned char *, size_t length) { return int(length); }
int hid_get_feature_report(hid_device * device, unsigned char * data, size_t length)
{
    std::lock_guard lock(radio.mutex);
    const auto id = data[0];
    std::memset(data, 0, length); data[0] = id;
    if (id == 0xff) {
        data[1] = 0x83; data[2] = 5; data[3] = 0;
        std::memcpy(data + 4, &radio.serial, 4);
        return 8;
    }
    if (id == 0x05) return 45; // no hardware sensor type, no retry
    if (id == 0x01) {
        data[1] = data[2] = radio.serial == tracker3 ? 1 : 0;
        return 3;
    }
    if (id == 0x10) {
        const std::string config = radio.serial == tracker3
            ? "{\"model_number\":\"VIVE Tracker 3.0 MV\"}"
            : "{\"model_number\":\"VIVE Tracker Pro MV\"}";
        uLongf size = compressBound(config.size()); device->config.resize(size);
        require(compress(device->config.data(), &size,
                         reinterpret_cast<const Bytef *>(config.data()), config.size()) == Z_OK,
                "synthetic config compresses");
        device->config.resize(size); device->offset = 0;
        data[1] = size >> 8; data[2] = size & 0xff;
        return 3;
    }
    if (id == 0x11) {
        const auto n = std::min(length - 2, device->config.size() - device->offset);
        data[1] = n;
        std::memcpy(data + 2, device->config.data() + device->offset, n);
        device->offset += n;
        return int(n + 2);
    }
    return -1;
}
}

int main()
{
    const auto capture = std::filesystem::temp_directory_path() /
        ("wivrn-reconnect-" + std::to_string(getpid()) + ".lhraw");
    try {
        lh::usb::UsbTransportOptions options;
        options.capturePath = capture.string();
        options.steamLighthouseConfigDir = "/nonexistent-wivrn-reconnect-test";
        lh::usb::UsbTransport transport(&sink, options);
        require(transport.Start(), "transport starts");
        wait_for([] { return sink.has_attachment(serial2018); }, "2018 attaches");
        imu(100);
        wait_for([] { return sink.sample_count() == 1; }, "2018 produces IMU");

        switch_to(0, true);
        wait_for([&] {
            const auto receivers = transport.Receivers();
            return receivers.size() == 1 && receivers[0].connected.empty();
        }, "disconnected dongle returns to discovery with no stale device identity");
        switch_to(tracker3, false);
        wait_for([] { return sink.has_attachment(serial3); }, "replacement 3.0 must attach with its own identity");
        imu(50);
        wait_for([] { return sink.sample_count() == 2; }, "3.0 produces IMU");
        {
            std::lock_guard lock(sink.mutex);
            require(sink.samples[1].first == serial3, "3.0 IMU is not attributed to 2018");
            require(std::abs(sink.samples[1].second.accel[0] / sink.samples[0].second.accel[0] - 2) < 1e-6,
                    "replacement uses freshly fetched accelerometer range");
            require(sink.attached.back().second.find("VIVE Tracker 3.0") != std::string::npos,
                    "replacement gets its own sensor configuration");
            require(sink.detached.front() == serial2018, "old tracker disconnects");
        }
        switch_to(tracker3, true);
        wait_for([] { return sink.has_attachment(serial3, 2); }, "same tracker reconnects");
        imu(1);
        wait_for([] { return sink.sample_count() == 3; }, "rebooted IMU clock is accepted");
        {
            std::lock_guard lock(sink.mutex);
            require(sink.samples[2].second.time < sink.samples[1].second.time, "same-serial reconnect resets timestamps");
        }

        // A missed disconnect must not leak the replacement's first packet
        // through the previous device's parser or into its capture endpoint.
        std::this_thread::sleep_for(1100ms);
        switch_to(tracker2018, false);
        imu(2);
        wait_for([] { return sink.has_attachment(serial2018, 2); }, "data gap re-identifies receiver without 0x26");
        require(sink.sample_count() == 3, "unverified post-gap sample is dropped");
        imu(3);
        wait_for([] { return sink.sample_count() == 4; }, "2018 resumes under correct identity");
        require(transport.Receivers().size() == 1 && transport.Receivers()[0].connected == serial2018,
                "receiver metadata follows replacement");
        transport.Stop();

        lh::usb::RawCaptureReader reader;
        require(reader.Open(capture.string()), "raw capture opens");
        std::vector<lh::usb::EndpointInfo> endpoints;
        require(reader.ReadAll([&](const auto & e) { endpoints.push_back(e); }, [](const auto &) {}),
                "capture remains replayable");
        require(endpoints.size() == 4 && endpoints[0].serial == serial2018 &&
                endpoints[1].serial == serial3 && endpoints[2].serial == serial3 &&
                endpoints[3].serial == serial2018, "capture records each verified connection as a new endpoint");
        require(endpoints[1].accelRange == 1 && endpoints[3].accelRange == 0,
                "capture preserves per-connection IMU ranges");
        require(radio.opens == radio.closes, "all reopened handles close");
        std::filesystem::remove(capture);
        std::cout << "PASS: replacement tracker, same-device reboot, missed disconnect, capture and receiver metadata\n";
        return 0;
    } catch (const std::exception & e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        std::filesystem::remove(capture);
        return 1;
    }
}
