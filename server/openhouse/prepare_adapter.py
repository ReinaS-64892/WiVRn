#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Clone the patched Monado OpenVR host without modifying its source tree.

Run after Monado's patches, so prediction, hotplug, roles, hands and shutdown
are inherited from steamvr_lh. Explicitly excluded Valve provider hooks are
reverted in a private copy before ownership symbols and provider loading change.
Unexpected upstream loader changes fail configuration rather than loading Valve.
"""
import re
import sys
import shutil
import subprocess
import tempfile
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


def prepare_provider(source, destination):
    # A learned base is accurate enough for alignment before RoomMap::Locked
    # freezes further refinement (100 keyframes). Match the runtime's public
    # readiness flag instead of hiding learned, still-refining bases from xdev.
    text = source.read_text()
    old_prefix = (
        "    // Until it's locked (or is the origin) it shows as searching (a blinking icon): its\n"
        "    // place can still move. Learned but not locked yet: calibrating.\n")
    # The current pin also accepts learned bases upstream, but still treats
    # origin/locked as sufficient. Keep the adapter's learned-only policy for
    # both versions, and reject unrecognized provider changes.
    old_blocks = [old_prefix + line for line in (
        "    const bool settled = b.known && (b.origin || b.locked);",
        "    const bool settled = b.known && (b.origin || b.locked || b.learned);  // learned: placed well enough to track with",
    )]
    if sum(text.count(block) for block in old_blocks) != 1:
        raise RuntimeError("Openhouse provider changed: expected one recognized base readiness block")
    old = next(block for block in old_blocks if block in text)
    text = replace_once(text, old,
        "    // Use the runtime's accuracy/evidence threshold for tracking and alignment.\n"
        "    // Locking only controls whether the room solver keeps refining this base.\n"
        "    const bool settled = b.known && b.learned;")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or destination.read_text() != text:
        destination.write_text(text)


def prepare(source, destination, excluded_patches=()):
    if excluded_patches:
        # Work on a private copy: never revert patches in the shared Monado tree.
        # Retain metadata/xdev/libmonado changes while removing Valve-only hooks.
        with tempfile.TemporaryDirectory(prefix='openhouse-adapter-') as temporary:
            root = Path(temporary)
            relative = Path('src/xrt/drivers/steamvr_lh')
            shutil.copytree(source, root / relative)
            for patch in reversed(list(excluded_patches)):
                sections = re.split(r'(?=^diff --git )', patch.read_text(), flags=re.M)
                driver_changes = ''.join(section for section in sections
                    if section.startswith('diff --git a/src/xrt/drivers/steamvr_lh/'))
                if not driver_changes:
                    raise RuntimeError(f'No steamvr_lh changes found in excluded patch: {patch}')
                subprocess.run(['patch', '--reverse', '--batch', '--fuzz=0', '-p1'],
                    cwd=root, input=driver_changes, text=True, check=True,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            prepare(root / relative, destination)
        return
    expected = set()
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
        expected.add(target)
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.read_text() != text:
            target.write_text(text)

    # Removed/excluded upstream files must not survive a reconfigure.
    for target in destination.rglob('*'):
        if target.suffix in ('.cpp', '.hpp', '.h') and target not in expected:
            target.unlink()


if __name__ == '__main__':
    if sys.argv[1] == '--provider':
        prepare_provider(Path(sys.argv[2]), Path(sys.argv[3]))
    elif sys.argv[1] == '--test':
        Path(sys.argv[3]).write_text(rename_symbols(Path(sys.argv[2]).read_text()))
    else:
        prepare(Path(sys.argv[1]), Path(sys.argv[2]), [Path(p) for p in sys.argv[3:]])
