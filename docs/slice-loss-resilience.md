# Slice loss resilience and FEC extension

This branch changes the video wire protocol (revision 2 and a new automatic
type hash). Install matching server and client builds. Older peers must fail
the existing protocol compatibility check rather than interpret these packets.

## Encoding and decoding

H.264 and HEVC use a target of four slices. x264 uses four sliced encoding
threads (the library can clamp this for small images); NVENC uses slice mode 3;
VAAPI requests four slices through AVCodecContext and accepts existing backend
options. Vulkan H.264 clamps the count to macroblock rows and maxSliceCount.
Vulkan HEVC also checks multipleSliceSegmentsPerTile and maxSliceSegmentCount,
and otherwise keeps one slice. Driver support is still required, particularly
for VAAPI. AV1 tiles and raw image partial decoding are outside this change.

The shared sender splits Annex B H.264/HEVC access units at NAL boundaries,
including encoder backends returning multiple NALs in one buffer. A source
packet never crosses a NAL boundary. x264's existing NAL callback uses the same
sender. Parameter sets and IDRs retain the backend's existing reliable delivery
policy. This also fixes x264's reordered callback path losing its reliable
delivery preference.

Every packet repeats view metadata and a unit descriptor. Losing the first
packet no longer loses the poses needed to display the frame. Timing remains
on the last packet. The maximum serialized video packet, including its variant
tag and optional metadata, is 1400 bytes before transport encryption. With the
current layout the conservative source symbol capacity is 1234 bytes, verified
by the video-wire test. Repeating view metadata costs bandwidth but avoids a separate
reliable per-frame metadata dependency.

The receiver buffers two frames. Complete access units are submitted immediately
and exactly once. Incomplete access units have an absolute deadline in the
headset's OpenXR clock domain:

```
min(predicted_display_time - decode_reserve - 2 ms,
    first_packet_time + receive_budget)
```

The decode reserve starts at 4 ms and follows measured decoder turnaround
(including queue time): 125% of the latest 1–16 ms clamped sample, with a 1%
decay per decoded frame. The UDP receive budget starts at 4 ms (or the ceiling
below, if smaller). After eight complete UDP samples it becomes 1.3 times the
mean of the fastest 75% of the last 32 complete UDP assembly durations, with a
1 ms floor. The ceiling is one described video frame period clamped to 2–20 ms
(90 fps fallback for an invalid rate). Slow successful outliers, truncated
frames, decoder turnaround and TCP recovery do not train the normal budget.
After eight consecutive incomplete UDP frames, expand the budget by 50% to at
least 4 ms, within the ceiling, and restart learning. This avoids permanently
censoring slower samples after a sustained network change. Without view metadata,
the first-packet limit still applies. If the predicted cutoff was already past
when the first packet arrived, use that same bounded first-packet budget. The
server pacer starts with no decode/display samples; immediately dropping every
fragment of a late first IDR would prevent the very feedback needed to calibrate
its predictions. This fallback allows reference recovery without an unbounded
wait. UDP deadlines never extend as more packets arrive; a packet arriving
after retirement cannot reopen the frame.

Control/TCP video packets retain their transport provenance locally, without a
wire format change. A frame containing reliable packets has a separate 100 ms
first-packet cap and does not use the predicted display deadline. This protects
large recovery IDRs and codec parameter sets from the short datagram loss wait,
which could otherwise repeatedly truncate decoder initialization. Complete
recovery frames are still submitted immediately. This also applies in TCP-only
mode, where the adaptive UDP budget is not used. The two-frame progress bound
still applies. This addresses a plausible startup/reconnect failure path;
confirm the reported Steam Frame reconnect failure with device recordings.

The network loop services these deadlines even without further packets and
rounds poll wakeups up to the nearest millisecond. Draining a buffered socket
batch returns to the loop to recalculate its timeout, rather than waiting with
the old idle timeout. These are receiver scheduling limits, not a hard realtime
guarantee: OS scheduling and blocking decoder/network calls can delay service.
The reserve is a starting policy requiring hardware latency validation.

An incomplete frame can also close earlier when the following frame is complete
or a packet two frames ahead arrives. Only whole NALs are used. This preserves
the earlier frame-progress bound in addition to the new clock deadline. Missing
the final packet no longer requires another video frame to trigger retirement.
Late packets for retired frames and duplicate source packets are ignored.

Partial submission requires H.264/HEVC and at least one complete VCL NAL.
Incomplete NALs contribute no bytes, including when their initial or final
symbol is missing. AV1/raw still require a complete frame. A completely lost
frame or a frame with no intact slices cannot be displayed by this mechanism.
Submitting an access unit means `push_data(..., partial=false)` followed by
`frame_completed`; it does not flush or reset the codec between pictures.

Iris CAPTURE buffers flagged ERROR are accepted for intentionally incomplete
access units when the driver returns nonempty image data. Unexpected ERROR
buffers remain discarded and now request reference recovery. FFmpeg permits
corrupt/concealed output. Android receives a closed access unit through its
existing MediaCodec path; its concealment behavior needs device validation.

`feedback.stream_corrupted` records partial input or a reported decoding error.
`sent_to_decoder` continues to record actual submission, and
`received_last_packet` stays zero for incomplete input. Default IDR handling
requests a clean IDR for damaged reference frames; Vulkan reference management
does not acknowledge partial frames as clean references. Concealment can still
propagate errors through inter-frame prediction until recovery.

## Wire layout and planned parity

Existing `stream_item_idx`, `frame_idx`, and frame-wide `shard_idx` are retained.
Every source packet now includes this descriptor:

| Field | Meaning |
| --- | --- |
| unit_idx (u16) | NAL/access-unit identifier within the stream frame |
| first_shard (u16) | First frame-wide source index of this unit |
| shard_count (u16) | Number of source symbols in this unit |
| byte_count (u32) | Original NAL length, including its Annex B start code |
| symbol_size (u16) | Capacity of every source symbol except its shorter tail |
| vcl (bool) | Unit carries an independently framed video slice |

Descriptors and symbol lengths are validated before allocation/insertion and
again before assembly. The final symbol length is derived from byte_count;
conflicting descriptors or incomplete units cannot be concatenated into the
decoder input.

**Parity generation and recovery are not implemented in this branch.** The next
step can introduce a separate parity packet variant keyed by
`(stream_item_idx, frame_idx, unit_idx)` without changing source shard numbering.
That packet should repeat the source descriptor, codec configuration/view
metadata as necessary, a FEC scheme/version, and a repair-symbol index. Keep
repair indices separate from frame-wide source shard indices, and do not count
parity packets when deciding whether a frame is complete.

For a first XOR scheme, zero-pad all symbols to symbol_size and transmit one
XOR repair symbol per unit. Recover exactly one missing source symbol, trim its
tail using byte_count, validate its descriptor, then insert it into the same
source-shard slot before complete-NAL selection. A single parity symbol cannot
repair two erasures in one unit. Large NALs should be split into bounded FEC
groups with an explicit group identifier/source range; this bounds parity
overhead, recovery work and buffering. More general erasure coding can use the
same source/repair distinction. Repair packets must fit the wire budget; split
repair data or reduce the source symbol capacity to accommodate their header.

Recovery must happen before the absolute receiver deadline and within the
two-frame buffering window, without
waiting for parity or retransmission before retiring a stale frame. Preserve
duplicate/late-packet handling, reject mismatched group metadata, cap recovery
work, and add separate feedback for recovered and unrecoverable source symbols.
Only a fully recovered original access unit should clear stream_corrupted.

## Validation and remaining hardware work

The supplied `../log/wivrn_steamframe_slice_reference` reports Iris concealing
a removed H.264 slice and outputting 120/120 frames. It does not establish HEVC,
AV1, IDR-loss or arbitrary driver behavior. The supplied packet audit contains
253,616 frame records, of which 9,017 have complete=false; these are per-stream
records, not a measured aggregate UDP packet-loss rate.

With WIVRN_BUILD_TEST enabled:

- video-units checks NAL boundaries, out-of-order/duplicate delivery, every
  source-symbol loss position, whole-NAL loss and malformed descriptors.
- video-wire checks serialization round trips, corruption feedback and the
  complete 1400-byte packet budget.
- video-slice-decode is built when libavcodec/libavutil/x264 are available. It
  generates 120 moving, four-slice H.264 pictures, drops one symbol in the
  second slice of picture 40, reconstructs using the actual unit selector and
  verifies that software decoding still returns all 120 pictures, including 40.
- shard-accumulator is built with the desktop client and exercises the actual
  two-frame receiver with a recording decoder and clock. It covers reordered
  and duplicate packets, first/final packet loss, consecutive damaged frames,
  frame gaps, late packets, view metadata and the AV1/raw complete-frame policy.
  It also checks timer expiry in complete silence, exact boundary times,
  missing prediction metadata, measured decode reserves, already-expired
  predictions, immediate complete-frame submission and receive statistics.

Run the registered tests from the build tree's common and client directories:

```sh
ctest --test-dir BUILD_DIR/common -R 'video-' --output-on-failure
ctest --test-dir BUILD_DIR/client -R 'shard-accumulator|video-diagnostics' --output-on-failure
```

## Client diagnostic recordings

Use **Record diagnostics** on the server list, the connected application picker,
or the streaming Statistics page. Begin before disconnecting, reproduce the
failure and use **Stop diagnostic recording** afterwards. Recording belongs to
the client process and continues through disconnect/lobby/reconnect. Each
recording gets a distinct `video-<Unix nanoseconds>-<pid>-<sequence>.jsonl` file.
The UI shows the full path, any write error and queue overflow count. Files are
created with mode 0600; a newly created final log directory is mode 0700.

On Linux/Steam Frame files go to `$XDG_STATE_HOME/wivrn/logs/`, falling back to
`$HOME/.local/state/wivrn/logs/`. Empty or relative XDG paths are ignored, following
the [XDG Base Directory specification](https://specifications.freedesktop.org/basedir/latest/).
On Android they go to `logs/` under the client app's existing private files directory.
Recording starts only through the button; it does not automatically resume on
client process restart.

JSON Lines schema 1 uses nanoseconds. The start record contains wall clock Unix
time; subsequent events carry enqueue-time `mono_ns` and OpenXR `xr_ns` where
available. Frame IDs are per stream and can reset after reconnect. Correlate
using `stream_created`/`stream_destroyed` and `mono_ns`, not frame ID alone.
Events include descriptions, decoder creation, first packets, retirement,
submission return, decoded output, display/repeated display, network heartbeat,
application launch requests, errors and Iris source-change/CAPTURE readiness.
Heartbeat `state` values are 0 initializing, 1 streaming, 2 stalled, 3 shutdown;
`xr_session_state` events carry the OpenXR session state value in `detail`.
Heartbeat `received` is cumulative
connection receive bytes; its `budget_ns` is the requested poll timeout.

Frame retirement events contain first/retirement/display/deadline times, the
learned UDP cap, transport, received source count, advertised minimum source
count, intact NAL/VCL counts, completeness, attempted submission and expiry.
`expected_min` is a lower bound if later unit descriptors never arrived.
`detail` distinguishes `complete`, `partial`, `no_intact_slice`, `no_view` and
`partial_unsupported`. `expired` is orthogonal: a deadline can lead to either
partial input or a discard. `submitted` records the decision before entering
the decoder; `decoder_submit_complete` confirms that call returned. Only a
`decoded`/`display` event establishes later pipeline progress. Late-packet
`received` is the shard index; display `received` is the repeat-display count.
Empty fields in other event kinds are zero, not measured timestamps.

The network/render/decoder threads enqueue bounded events to a dedicated writer
instead of writing files. At most 4096 queued events are retained, and overflow
is counted in the UI and final stop record. Stop drains and closes the file.
A crash may leave the final line unfinished and omit the stop record; earlier
complete lines remain usable. These are timing/decision logs, not packet payload
or image dumps, nor a complete copy of the client's standard-output log.

## Client statistics overlay

The existing Statistics overlay now includes Video loss, Receive wait and Late
video packets. Loss counters are collected at receiver retirement, independently
of decoded image handles, so discarded frames remain visible. History and
totals are sampled even before both eyes have produced their first decoded
image or while rendering is suppressed. The loss graph
shows counts per display interval summed across all active streams; they are
not percentages or per-eye FPS. A partial frame also counts as incomplete, and
expiry is a retirement reason that can overlap either partial submission or
discard. A complete frame is not delayed just to generate a wait-time sample.

Receive wait (UDP) plots show mean and maximum time from the first accepted
packet to receiver retirement for each interval, plus the maximum current
adaptive cap across streams. An absolute display deadline may retire earlier.
Reliable receive wait is plotted separately, so a recovery IDR cannot obscure
normal datagram waits. Discard reasons count missing intact slices and codecs
without partial decoding support. The text below the graphs shows session
totals for incomplete frames, partial decode inputs, pre-decode discards,
deadline retirements and late packets. These decisions do not guarantee that a
driver outputs or displays an image; downstream decoder/render drops are not
included in the receiver discard count. Frame-ID gaps are reported separately
as unobserved, since the server's IDR handling can intentionally skip IDs. The
first observed frame establishes the start and does not invent a gap from zero.

Missing timestamps on partial frames are gaps in the decoder timing plots,
rather than artificial zero/epoch timing values. Distinct series colors and
immediate axis growth keep individual loss spikes visible. Japanese overlay
labels are included. Reference recovery/IDR acknowledgement behavior remains
unchanged; these new counters help distinguish receive waiting from other stalls.

Before deployment, validate actual WiVRn on Steam Frame and Android: first,
middle and last slice loss; missing first/final frame packet; IDR loss; burst
loss; pose/PTS correspondence; HEVC; and post-loss reference recovery. Compare
decoded pixels with a lossless transport run and measure display latency,
bitrate overhead and encoder throughput. Software decoding and compilation do
not establish hardware quality or latency.

Implementation references:
[Vulkan H.264 picture info](https://docs.vulkan.org/refpages/latest/refpages/source/VkVideoEncodeH264PictureInfoKHR.html),
[Vulkan HEVC capabilities](https://docs.vulkan.org/refpages/latest/refpages/source/VkVideoEncodeH265CapabilityFlagBitsKHR.html),
[NVENC guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/),
[V4L2 stateful decoder API](https://docs.kernel.org/userspace-api/media/v4l/dev-decoder.html).
