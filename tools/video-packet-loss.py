#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare opt-in WiVRn video shard audit logs, one stream/capture at a time."""

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass, field
import json
from pathlib import Path

MARKER = "WIVRN_PACKET_AUDIT "
BITMAPS = ("seen", "on_time", "udp", "accepted")


@dataclass
class Frame:
    seen: int = 0
    on_time: int = 0
    udp: int = 0
    accepted: int = 0
    packets: int = 0
    bytes: int = 0
    reordered: int = 0
    complete: bool = False
    first_ns: int = 0


@dataclass
class Capture:
    frames: dict = field(default_factory=dict)
    ended: bool = False
    retired_before: int = 0
    next_seq: int = 1


def read_log(path):
    captures = {}
    with Path(path).open(encoding="utf-8", errors="replace") as source:
        for number, line in enumerate(source, 1):
            if MARKER not in line:
                continue
            try:
                record, _ = json.JSONDecoder().raw_decode(line.split(MARKER, 1)[1])
                if record["v"] != 1 or record["side"] not in ("tx", "rx"):
                    raise ValueError("unsupported record version/side")
                key = (record["side"], record["stream"], record["capture"])
                event = record["event"]
                if event == "begin":
                    if record["seq"] != 0:
                        raise ValueError("invalid begin sequence")
                    if key in captures:
                        raise ValueError("repeated begin: do not concatenate separate process runs")
                    captures[key] = Capture()
                    continue
                if key not in captures:
                    raise ValueError("missing begin record (log capture started too late)")
                capture = captures[key]
                if capture.ended:
                    raise ValueError("record after end")
                if record["seq"] != capture.next_seq:
                    raise ValueError("audit records missing or reordered in log")
                capture.next_seq += 1
                if event == "end":
                    capture.ended = True
                    capture.retired_before = int(record["retired_before"])
                elif event == "frame":
                    offset = record["offset"]
                    if not isinstance(offset, int) or offset < 0 or offset > 65024 or offset % 512:
                        raise ValueError("invalid bitmap offset")
                    frame = capture.frames.setdefault(record["frame"], Frame())
                    for name in BITMAPS:
                        text = record[name]
                        if len(text) > 128:
                            raise ValueError("oversized bitmap")
                        setattr(frame, name, getattr(frame, name) | (int(text or "0", 16) << offset))
                    if offset == 0:
                        if frame.packets:
                            frame.first_ns = min(frame.first_ns, record["first_ns"])
                        else:
                            frame.first_ns = record["first_ns"]
                        for name in ("packets", "bytes", "reordered"):
                            value = record[name]
                            if not isinstance(value, int) or value < 0:
                                raise ValueError("invalid counter")
                            setattr(frame, name, getattr(frame, name) + value)
                        frame.complete |= record["complete"]
                else:
                    raise ValueError("unknown event")
            except (KeyError, TypeError, ValueError) as error:
                raise ValueError(f"{path}:{number}: {error}") from error
    return captures


def select(captures, side, stream, capture_id=None):
    candidates = [(key, value) for key, value in captures.items()
                  if key[:2] == (side, stream)
                  and (capture_id is None or key[2] == capture_id)]
    nonempty = [(key, value) for key, value in candidates if value.frames]
    if nonempty or side == "tx":
        candidates = nonempty
    if len(candidates) != 1:
        ids = [key[2] for key in captures if key[:2] == (side, stream)]
        raise ValueError(f"select one {side} capture for stream {stream}; available IDs: {ids}")
    key, capture = candidates[0]
    if not capture.ended:
        raise ValueError(f"{side} capture {key[2]} has no end record; disconnect normally and retain the full log")
    for frame in capture.frames.values():
        if frame.packets < frame.seen.bit_count():
            raise ValueError("missing frame fragments/counters in log")
        if (frame.on_time | frame.udp | frame.accepted) & ~frame.seen:
            raise ValueError("bitmap contains shards absent from observed set")
    return key[2], capture


def longest_run(bits):
    result = 0
    while bits:
        bits &= bits << 1
        result += 1
    return result


def compare(tx, rx):
    totals = Counter()
    histogram = Counter()
    groups = Counter()
    seconds = defaultdict(Counter)
    matched = set(tx.frames) & set(rx.frames)
    if rx.frames and not matched:
        raise ValueError("no common frames; check stream/capture pairing")
    for index, sent in sorted(tx.frames.items()):
        received = rx.frames.get(index, Frame())
        totals["incomplete_tx_frames"] += not sent.complete
        if not sent.complete:
            continue
        if received.seen & ~sent.seen:
            raise ValueError("RX shards absent from TX frame; check capture pairing")
        totals["tx_frames"] += 1
        totals["send_failed_or_inactive"] += (sent.seen & ~sent.accepted).bit_count()
        totals["tcp_accepted"] += (sent.accepted & ~sent.udp).bit_count()
        udp = sent.seen & sent.udp & sent.accepted
        missing = udp & ~received.seen
        late = udp & received.seen & ~received.on_time
        totals["udp_accepted"] += udp.bit_count()
        totals["udp_not_received_by_capture_end"] += missing.bit_count()
        totals["udp_received_late_unique"] += late.bit_count()
        bucket = seconds[sent.first_ns // 1_000_000_000]
        bucket["udp_accepted"] += udp.bit_count()
        bucket["udp_not_received_by_capture_end"] += missing.bit_count()
        bucket["udp_received_late_unique"] += late.bit_count()
        if index < rx.retired_before:
            deadline_missing = udp & ~received.on_time
            totals["finalized_frames"] += 1
            totals["finalized_frames_not_completed"] += not received.complete
            totals["udp_finalized_accepted"] += udp.bit_count()
            totals["udp_missing_at_retirement"] += deadline_missing.bit_count()
            if udp:
                histogram[deadline_missing.bit_count()] += 1
                totals["max_consecutive_missing_in_frame"] = max(
                    totals["max_consecutive_missing_in_frame"], longest_run(deadline_missing))
                for offset in range(0, udp.bit_length(), 8):
                    if (udp >> offset) & 255:
                        groups[((deadline_missing >> offset) & 255).bit_count()] += 1
        else:
            totals["unfinalized_frames"] += 1
    for received in rx.frames.values():
        totals["rx_duplicates_all_transports"] += received.packets - received.seen.bit_count()
        totals["rx_out_of_order_all_transports"] += received.reordered
    denominator = totals["udp_finalized_accepted"]
    return {
        "totals": dict(totals),
        "udp_missing_at_retirement_percent": (
            100 * totals["udp_missing_at_retirement"] / denominator if denominator else None),
        "missing_per_finalized_udp_frame": dict(sorted(histogram.items())),
        "missing_per_8_shard_group": dict(sorted(groups.items())),
        "per_tx_second": {key: dict(value) for key, value in sorted(seconds.items())},
        "notes": [
            "Application video shards, not Wi-Fi retry counters or proof of radio loss.",
            "Missing at retirement includes late arrivals; unfinished tail is excluded from that rate.",
            "Capture-end missing assumes the selected captures cover the same connection and interval.",
            "Eight-shard groups align to shard index within each frame; this is not a FEC simulation.",
            "Out-of-order includes duplicates; RX counters include TCP video shards.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tx_log", help="server log including begin/end audit records")
    parser.add_argument("rx_log", help="client log including begin/end audit records")
    parser.add_argument("--stream", type=int, default=0, help="0=left, 1=right, 2=alpha (default: 0)")
    parser.add_argument("--tx-capture", type=int)
    parser.add_argument("--rx-capture", type=int)
    args = parser.parse_args()
    try:
        tx_id, tx = select(read_log(args.tx_log), "tx", args.stream, args.tx_capture)
        rx_id, rx = select(read_log(args.rx_log), "rx", args.stream, args.rx_capture)
        report = compare(tx, rx)
        report.update(stream=args.stream, tx_capture=tx_id, rx_capture=rx_id)
        print(json.dumps(report, ensure_ascii=False, indent=2))
    except (OSError, ValueError) as error:
        parser.exit(2, f"error: {error}\n")


if __name__ == "__main__":
    main()
