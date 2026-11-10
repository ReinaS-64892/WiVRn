#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Clone the patched Monado OpenVR host without modifying its source tree.

Run after Monado's patches, so prediction, hotplug, roles, hands and shutdown
stay identical to steamvr_lh. Only ownership symbols and provider loading differ.
Unexpected upstream loader changes fail configuration rather than loading Valve.
"""
import re
import sys
from pathlib import Path


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f"Monado loader changed: expected one occurrence of {old!r}")
    return text.replace(old, new, 1)


def rename_symbols(text):
    names = ('Context DeviceBuilder Property Device VivePro2Data HmdDevice '
             'ControllerDevice TrackingReferenceDevice InputClass Settings Resources '
             'IOBuffer DriverManager Server BlockQueue Paths BaseStationStabilizer').split()
    # Preserve comments and string literals (including resource names and logs).
    tokens = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\b(' + '|'.join(names) + r')\b', re.DOTALL)
    text = tokens.sub(lambda m: 'Openhouse' + m[1] if m[1] else m[0], text)
    return text.replace('steamvr_lh', 'openhouse').replace('lighthouse_reference', 'openhouse_reference').replace('reference_recorder.hpp', 'openhouse_reference_recorder.hpp')


def prepare(source, destination):
    for path in sorted(source.rglob('*')):
        if path.suffix not in ('.cpp', '.hpp', '.h'):
            continue
        text = path.read_text()
        # Prefix types as well as C entry points: both static adapters are linked
        # into one executable, even though only one is initialized at a time.
        text = rename_symbols(text)
        text = text.replace('SteamVR Lighthouse Tracking', 'openhouse Lighthouse Tracking')
        if path.name == 'steamvr_lh.cpp':
            text = replace_once(text, '#include <dlfcn.h>',
                'extern "C" void *wivrn_openhouse_factory(const char *, int *);')
            begin = text.index('\tif (steamvr.empty()) {', text.index('openhouse_create_devices('))
            end = text.index('\tauto ctx = OpenhouseContext::create(', begin)
            text = text[:begin] + '''\t// SteamVR is an optional source of controller resources, never our provider.
\tint error = vr::VRInitError_None;
\tauto *provider = static_cast<vr::IServerTrackedDeviceProvider *>(
\t    wivrn_openhouse_factory(vr::IServerTrackedDeviceProvider_Version, &error));
\tif (provider == nullptr || error != vr::VRInitError_None)
\t\treturn xrt_result::XRT_ERROR_DEVICE_CREATION_FAILED;
\tstd::vector<vr::IServerTrackedDeviceProvider *> drivers{provider};
''' + text[end:]
        relative = str(path.relative_to(source)).replace('steamvr_lh', 'openhouse')
        relative = relative.replace('reference_recorder.hpp', 'openhouse_reference_recorder.hpp')
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.read_text() != text:
            target.write_text(text)


if __name__ == '__main__':
    if sys.argv[1] == '--test':
        Path(sys.argv[3]).write_text(rename_symbols(Path(sys.argv[2]).read_text()))
    else:
        prepare(Path(sys.argv[1]), Path(sys.argv[2]))
