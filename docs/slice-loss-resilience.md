# Slice loss resilience and FEC extension

This branch changes the video wire protocol (revision 3 and a new automatic
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
current layout the conservative source symbol capacity is 1233 bytes (one byte reserved for repair source_count), verified
by the video-fec wire test. Repeating view metadata costs bandwidth but avoids a separate
reliable per-frame metadata dependency.

The receiver buffers two frames. Complete access units are submitted immediately
and exactly once. Incomplete access units initially use this receive cutoff in
the headset's OpenXR clock domain:

```
min(predicted_display_time - decode_reserve - 2 ms,
    first_packet_time + receive_budget)
```

The decode reserve starts at 4 ms and follows measured decoder turnaround
(including queue time): 125% of the latest 1–16 ms clamped sample, with a 1%
decay per decoded frame. The UDP receive budget starts at 4 ms (or the ceiling
below, if smaller). Observations are grouped into four one-second clock buckets.
Each retains up to 256 complete UDP assembly durations, with older samples
replaced within a full bucket. At least eight complete samples are required to
learn 1.75 times the fastest 75% mean, with a 2 ms floor. The ceiling is one
described video frame period clamped to 2–20 ms (90 fps fallback for an invalid
rate). Slow successful outliers, truncated waits, decoder turnaround and TCP
recovery do not train the normal mean.

For the last 64 retired frame IDs, late UDP source packets from an adaptive
expiry contribute elapsed time since that frame's first packet plus 0.5 ms,
bounded to 2–4 ms and the frame-period ceiling. Each clock bucket retains its
peak allowance. Complete-frame duplicates, reliable frames and display-deadline
retirements do not train it. Symbols remain discarded; this changes future
frames. Eight consecutive incomplete UDP frames also raise a recovery target
to at least 4 ms or 150% of the current cap, within the ceiling.

Targets use the recent clock buckets. The applied cap updates at most once per
second, rises by at most 0.5 ms per update and falls by at most 0.1 ms per update,
with a 0.05 ms deadband. Even after an idle gap only one bounded step is applied.
This removes packet-driven jumps and frame-rate-dependent decay. The recent
late-arrival allowance must age out before the cap can decline. The learned cap does not move an already armed frame deadline.

For Iris/V4L2, the adaptive limit is now a soft cutoff. At this cutoff the
receiver may grant one extension of at most 2 ms, anchored to the original
cutoff. It must remain within the earlier of the first-packet frame-period cap
and the predicted display cutoff (including the decode/render reserve). If
that whole extension has already elapsed before the network thread wakes,
it is not granted. Both buffered frames are serviced; silence cannot rearm the
timer. Complete frames still submit immediately, and a newer complete frame
preempts the old partial frame even during grace. TCP/reliable recovery frames,
earlier display cutoffs and other decoder backends keep their existing policy.
A complete frame rescued during grace also contributes to the bounded late-tail
allowance. This is a short opportunity for a usable complete frame, not a claim
that Iris will conceal incomplete VCL input. Without view metadata,
the first-packet limit still applies. If the predicted cutoff was already past
when the first packet arrived, use that same bounded first-packet budget. The
server pacer starts with no decode/display samples; immediately dropping every
fragment of a late first IDR would prevent the very feedback needed to calibrate
its predictions. This fallback allows reference recovery without an unbounded
wait. Extra packets do not reset deadlines. The single Iris grace extension
above is explicit and bounded; a packet arriving after final retirement cannot
reopen the frame.

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

## Wire layout and XOR parity

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

UDP video now uses systematic XOR FEC by default. Each unit is split into
canonical groups of up to eight consecutive source symbols. The sender emits
one repair immediately after each group, including a shorter unit tail group.
It never joins different units or frames, waits for another frame, or adds FEC
to the existing TCP/control path. All encoder backends use the shared sender;
this applies to opaque AV1/raw data as well as H.264/HEVC. Codec-independent
recovery happens before the hardware/software decoder. Slice count remains four
where supported; no new AV1/HEVC tile layout is introduced in this step.

The new `video_stream_repair_shard` variant carries `source_count` (u8) followed
by a source-shaped descriptor. Its `shard_idx` is the first protected source
index, **not** a repair index in the source sequence. The descriptor repeats
stream/frame/unit information, view metadata, and final-frame timing when the
protected group contains the frame's last source symbol. Its payload is the XOR
of all group payloads, each zero-padded to symbol_size. Parity is accumulated
from plaintext **before** source send, because UDP encryption mutates payload
spans in place; both source and repair then use normal transport encryption. The 1400-byte serialized
limit applies to repairs too. Revision 3 and the automatic type hash require
matching server/client builds even when FEC is disabled.

Recovery validates the canonical group alignment/count/range and exact parity
length, then requires exactly one absent source. Every received operand must
match frame, stream, unit, source index and exact length. The recovered symbol
owns its bytes, inherits the repeated view metadata, and is truncated using
byte_count if it is the unit tail. Final-frame timing is attached only when
recovering that tail. Recovered sources enter the ordinary complete-NAL path;
parity occupies separate per-frame storage and never substitutes for a source
in the completeness test. Duplicate repairs cannot repeat submission. A lost
repair alone does not delay a complete frame; two missing sources in the same
group remain unrecoverable. Groups in the same frame recover independently.
The two-frame window and existing deadlines/grace are unchanged. A retired
frame is never reopened, and late repairs never train the receive budget.
Only a fully assembled original access unit clears stream_corrupted.

Set `WIVRN_VIDEO_FEC=0` **in the server process environment** to disable repair
transmission for an A/B comparison; the default is enabled. This initial scheme
adds one symbol per eight full-sized sources (approximately 12.5% payload
redundancy); short tail groups and repeated headers increase the ratio. Encoder
bitrate is unchanged, so total network traffic increases. This is fixed FEC,
not congestion-adaptive coding. Burst losses within a group may still defeat it;
interleaving or a separate multi-repair coding variant can build on the source/
repair namespace without treating parity bytes as video.

The Video FEC graph shows received repair packets (including redundant/late
ones), recovered source packets, and fully assembled frames containing at least
one recovered source. When parity arrives before an original source due to reordering, that source
may be reconstructed before the original arrives; a recovery is not proof the
original packet was permanently lost. Recovery/frame counters are collected at retirement,
independent of decoder output. They do not mean the frame was displayed. Logs
emit `fec_recovered` per recovered source (`received` is its source index),
`fec_frame` at retirement (`received` is recovered source count; `complete`
distinguishes complete/partial recovery), and `late_repair` for retired-frame
repairs. A normal lossless frame can produce late_repair when its final repair
arrives after all its sources; this is not evidence of video loss. Repair
descriptors also contribute to known damaged-VCL and expected_min counts when
all original source packets in that unit are absent. Count FEC successes and
remaining incomplete frames together when comparing runs.

The software tests cover each absent position, short/padded tails, multiple
independent groups, two losses in one group, missing parity, duplicated and
reordered repair/source arrival, metadata mismatches, final-frame metadata,
wire round trips, AES-CTR transport mutation through the actual sender helper,
and the 1400-byte limit. A real x264/FFmpeg run drops one source
in every group across 120 four-slice frames, restores byte-identical access
units, and decodes all 120 frames with matching PTS.

### Latest pre-FEC Steam Frame run

`video-1795363378563497988-107391-0.jsonl` identifies client `12c0c753`.
Both eyes together retired 22,392 UDP frames, including 1,859 incomplete frames
(8.30%); 1,306 were submitted as partial inputs. Across all streams, 2,013 grace
attempts yielded 163 complete assemblies. No backwards display-frame transition
was observed. This is a different run/workload from earlier logs and does not
establish a controlled regression or improvement. It supports moving the next
experiment to packet recovery rather than extending the wait again.

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
ctest --test-dir BUILD_DIR/client -R 'shard-accumulator|video-diagnostics|receive-time-budget|frame-selection' --output-on-failure
```

## Recorded limit adjustment (2026-11-22)

Input: `../log/wivrn-client-log/video-1795278441597786802-77831-0.jsonl`,
client commit `ac4c3504`, approximately 108 seconds, with zero recorder queue
losses. The descriptions advertise 90 fps, 768x768 and HEVC. Reconnect creates
a second stream scene, so analysis keys frame IDs by scene lifetime and stream.

| Stream | Complete UDP frames | Incomplete UDP frames | Incomplete share |
| --- | ---: | ---: | ---: |
| 0 | 7,212 | 1,985 | 21.6% |
| 1 | 7,409 | 1,790 | 19.5% |
| 2 | 2,415 | 22 | 0.9% |

Across streams 0 and 1, 3,775 of 18,396 UDP frames (20.5%) were incomplete.
Every incomplete UDP retirement used the adaptive cap, rather than an earlier
predicted display cutoff. The cap was almost always at the old 1 ms floor.
Of 3,797 incomplete UDP frames across all streams, 3,655 had packets observed
after retirement. Their last observed late packet arrived at a median of
1.301 ms after the first packet, 95th percentile 3.341 ms and 99th percentile
5.079 ms. This is evidence of a too-short cap; the complete-frame population
alone is biased toward arrivals that survived that cap. The old backoff also
required eight consecutive failures and missed frequent isolated failures.
Two UDP retirements also waited 30.128 ms and 35.362 ms despite 4 ms and 1 ms
caps respectively. These are late deadline-service events, not configured wait
budgets; changing the cap alone cannot eliminate such scheduling stalls.

There were 3,126 partial submissions but only 93 corresponding decoded outputs,
compared with 17,036 complete UDP submissions and 17,036 matching outputs.
The recording also contains 3,033 discarded Iris CAPTURE error events with no
matched frame timestamp. These counts indicate that partial submission often
did not produce displayable output on this device, but do not establish the
kernel's error cause. The added raw timestamp/bytes-used/pending-count details
are needed before changing the driver's output acceptance policy.

The adjustment raises the normal floor to 2 ms and the fast-sample multiplier
to 1.5, and uses observed late arrivals to preserve a slowly decaying tail
allowance capped at 4 ms. Complete frames still leave immediately. Existing
absolute display deadlines and the reliable recovery cap are retained. Actual
post-change latency and loss improvement require another headset recording.
The log does not contain original packet indices or unit descriptors for every
late packet; it cannot prove exactly how many frames a different timeout would
have recovered, nor distinguish every late duplicate from a missing symbol.

## Late-frame recovery and display order (2026-11-23)

New logs:

| Client commit | Recording suffix | Duration | Incomplete eye UDP frames | Backward display transitions |
| --- | --- | ---: | ---: | ---: |
| `1443f27d` | `1795361241141511957-94726-0` | 238.0 s | 2,377 / 42,140 (5.64%) | 100 |
| `e907ac93` | `1795361783427188287-99098-0` | 115.3 s | 1,258 / 20,387 (6.17%) | 94 |

Files are under `../log/wivrn-client-log/video-<suffix>.jsonl`. Key IDs by stream
scene lifetime; display counts above sum both eyes, so a stereo backward step
usually contributes two transitions. An example in the second recording is
`48751 -> 48749`. Decoder output itself was monotonic in the observed sequences.
The renderer could select newer per-eye fallback frames and then return to an
older common frame left in its three-frame caches. This is an additional source
of visible discontinuity, separate from repeated images due to real data loss.

Frame selection now rejects every candidate older than that stream's preceding
selected image. It prefers a common eligible frame near the display prediction,
then falls back per stream without reversing progress. Previously selected
images remain held across cache wrap until replacement, and are released after
the GPU fence. A decoder reconfiguration clears the progress floor after that
fence; a new scene naturally starts without an old floor. This prevents frame
rollback; it cannot synthesize missing motion or guarantee the removal of all
stutter. Losing one eye can still require per-eye fallback, as before.

In the second recording, all 1,258 incomplete eye frames retired at the adaptive
limit, with a median cap of 4 ms. 276 had another packet observed within 1 ms
past that cutoff and 330 within 2 ms. This motivates the one-time Iris grace
above, together with the requested 1.75 mean multiplier. It is not an exact
prediction of repaired frames: late-packet records lack full source payload
and descriptors, and some arrivals may be duplicates or still leave holes.
There were 743 partial submissions and no matched decoded partial output in
this recording. No change is made to accepting timestamp-less empty images.

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
available. Frame IDs are per stream and can reset after reconnect or decoder reconfiguration. Correlate
using `stream_created`/`stream_destroyed`, `frame_sequence_reset` and `mono_ns`,
not frame ID alone.
Events include descriptions, decoder creation, first packets, retirement,
submission return, decoded output, display/repeated display, network heartbeat,
application launch requests, errors and Iris source-change/CAPTURE readiness.
Iris capture-error details include the raw timestamp, whether it matches a
pending frame, bytes used and pending-frame count.
Heartbeat `state` values are 0 initializing, 1 streaming, 2 stalled, 3 shutdown;
`xr_session_state` events carry the OpenXR session state value in `detail`.
Heartbeat `received` is cumulative
connection receive bytes; its `budget_ns` is the requested poll timeout.

Frame retirement events also record `vcl_observed` (distinct VCL units with any
received symbol) and `vcl_damaged` (observed units that cannot be submitted).
Existing `vcl` counts intact VCL units; parameter sets and other non-VCL NALs do
not count as slices. Entirely unseen units are absent from the observed count,
so damaged counts are lower bounds, not a presumed four-slice total. These
additive schema-1 fields are zero for unrelated event kinds.

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

Grace is visible in `receive_grace` events: `deadline_ns` is the extended cutoff,
`budget_ns` is the original soft budget, and `detail` includes the absolute hard
cutoff as `hard_deadline_ns`. `grace_completed` means full source assembly before
final retirement, not proof of later decode/display. The Late-frame grace graph
and cumulative text show extensions and frames completed after extension; other
loss graphs count only final decisions, not soft-cutoff visits.

## Recorded stability and slice retention (2026-11-22)

Second input: `../log/wivrn-client-log/video-1795279352841449734-81607-0.jsonl`,
commit `1443f27d`, approximately 160 seconds, no recording overflow.
Streams 0/1 have 841 incomplete UDP frames out of 27,770 (3.03%), compared with
20.52% in the previous recording. Workloads/time intervals differ; this is not
a controlled measurement of the change alone. Their first-packet caps range
from 2 to 4 ms, with 388 frame-to-frame changes of at least 0.1 ms; the median
such change is 0.814 ms. This motivates clock windows and bounded update speed.

Of the 840 deadline retirements in streams 0/1:

| Intact VCL slices | Frames | Share |
| --- | ---: | ---: |
| 0 | 259 | 30.8% |
| 1 | 240 | 28.6% |
| 2 | 202 | 24.0% |
| 3 | 137 | 16.3% |
| 4 | 2 | 0.2% |

Thus most retain at least one whole slice. Four intact slices can coexist with
other lost NALs or frame metadata, so the final row is still incomplete input.
Across all streams there are 591 partial submissions and only three matched
decoded outputs. The two decoded partial outputs in streams 0/1 both retain all
four VCL slices; the third output, in stream 2, retains three. There is no
matched decoded main-eye output with a missing VCL slice in this recording. All 588 Iris CAPTURE errors report `timestamp=0`, `known=false`
and `bytes_used=0`: there is no returned image to display. Increasing the slice
count alone cannot be assumed to fix this driver behaviour. Keep capture output
validation; do not associate timestamp-less empty output with an arbitrary pose.

## Client statistics overlay

The Intact slices at deadline graph bins incomplete H.264/HEVC deadline frames
by 0, 1, 2, 3 or 4+ surviving VCL NALs. The text below shows cumulative bins plus
intact and known-damaged slice totals. A zero-slice frame is all unavailable at
retirement, rather than proof that every byte was lost. Entirely unobserved
frames and opaque codecs do not enter this histogram. Counts include all
active streams, not just the left/right eyes, and remain available without
recording enabled or successful decoder output.

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
[XOR parity principles (RFC 5109; this implementation is not RTP)](https://www.rfc-editor.org/rfc/rfc5109.html),
[Vulkan H.264 picture info](https://docs.vulkan.org/refpages/latest/refpages/source/VkVideoEncodeH264PictureInfoKHR.html),
[Vulkan HEVC capabilities](https://docs.vulkan.org/refpages/latest/refpages/source/VkVideoEncodeH265CapabilityFlagBitsKHR.html),
[NVENC guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/),
[V4L2 stateful decoder API](https://docs.kernel.org/userspace-api/media/v4l/dev-decoder.html).
