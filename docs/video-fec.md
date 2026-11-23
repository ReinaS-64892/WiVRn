# Packet-level video FEC

Branch `feat/video-fec` starts at `70485093`, before the slice/receive-deadline
experiments on `feat/video-transport-resilience`. It adds packet recovery without
changing encoder slice/tile settings, decoder error handling, receive waiting,
or frame selection. Reed-Solomon (RS) is the default; XOR remains available for
comparisons. No cross-eye buffering or coalescing is used.

## Recovery and configuration

The sender uses a systematic Cauchy Reed-Solomon erasure code over GF(256).
Original video packets are sent unchanged in format; repairs are separate
packets. A full block contains 16 sources and, by default, two repair packets.
Any 16 of those 18 packets suffice to recover all sources. Thus two lost sources,
or one lost source plus one lost repair, are recoverable. Receiving all sources
submits the frame immediately without waiting for repairs.

Configure these in the **server process environment**:

| Variable | Default | Meaning |
| --- | --- | --- |
| `WIVRN_VIDEO_FEC` | enabled | `0` disables repairs and restores unprotected packetization |
| `WIVRN_VIDEO_FEC_SCHEME` | `rs` | `rs` or `xor` |
| `WIVRN_VIDEO_FEC_REPAIRS` | `2` | RS repair packets per 16 sources, integer 1–8; ignored for XOR |
| `WIVRN_VIDEO_FEC_INTERLEAVE` | `4` | Maximum groups per window, integer 1–4 |

Invalid scheme/count/depth values are logged and fall back to their defaults.
The server logs each stream's selected scheme, full-block sizes, interleave
depth and enabled state at encoder creation. Client configuration is unnecessary.
For stronger RS comparisons, set `WIVRN_VIDEO_FEC_REPAIRS=4` or `8`.
The default stays at 2.

Full 16+2 blocks add 12.5% repair packets; 16+4 adds 25%, and 16+8 adds 50%.
Headers, padding and short blocks increase byte overhead. Encoder bitrate is unchanged, so repairs
increase total network traffic. The RS repair count for a short block of K
sources is `ceil(K * configured_repairs / 16)`, at least one. For example, the
default uses one repair for K=1–8 and two for K=9–16. This is a fixed code rate,
not adaptive congestion control. Losing more symbols than a block's repair
budget remains unrecoverable, regardless of the session's average loss rate.

## Sending and interleaving

Only already encoded data is grouped. No window crosses an encoder output
buffer, stream or frame. No sleep, timer, separate sending thread or wait for
future output is introduced. Small per-NAL encoder callbacks can therefore
produce short blocks even when a whole frame is large.

An RS window contains up to 64 sources at the default four-group depth. The
sender uses `ceil(N/16)` balanced consecutive source ranges, whose sizes differ
by at most one, then emits one symbol per group in round-robin order. Each
group's repairs follow its own sources and participate in the same schedule.
For a full 64-source, 16+2 window:

```
0,16,32,48, 1,17,33,49, ... 15,31,47,63,
repairA0,repairB0,repairC0,repairD0,
repairA1,repairB1,repairC1,repairD1
```

This full window tolerates any single burst of up to eight emitted datagrams,
including repairs, assuming all other packets arrive. With four repairs per
block the full window tolerates sixteen; with eight repairs it tolerates
thirty-two. Short/uneven windows, multiple bursts
and losses crossing window boundaries have different limits; there is no
blanket eight-packet guarantee for arbitrary frames.

XOR uses the previous balanced groups of at most eight sources, one repair per
group, and at most 32 sources per window. Source ordering and group boundaries
in XOR mode retain the previous scheduler behavior. The new wire header is
shared by both schemes, so XOR comparisons also require the new client.

All repairs for a window are computed **before the first source is sent**:
transport encryption mutates source payload spans in place. Repairs then use
normal transport encryption. Original source spans borrow the live encoder
buffer; temporary parity/source-body storage is bounded by the current window.
Reordering delays some contiguous decoder input relative to sequential sending;
actual latency and CPU cost still need hardware measurement.

## Wire format and code definition

Protocol revision 4 requires matching server and client builds. Revision-3
clients only support up to four repairs, so both ends must be updated even
though the repair header layout is unchanged. The original
`video_stream_data_shard` layout is retained: first-packet view/pose metadata and
last-packet timing/frame-end metadata keep their existing placement. The repair
variant contains:

| Field | Meaning |
| --- | --- |
| `stream_item_idx`, `frame_idx` | Protected stream and frame |
| `first_shard`, `source_count` | Consecutive protected source range |
| `scheme` | 0 = XOR (1–8 sources), 1 = RS (1–16 sources) |
| `repair_index`, `repair_count` | Independent repair equation index and total number sent (1–8 for RS) |
| `source_sizes[16]` | Serialized source-body lengths; unused entries must be zero |
| `payload` | Coded body, zero-padded to the longest source |

The protected body includes the source header, optional metadata and video
payload, excluding the outer packet type tag. First/last packets can therefore
be reconstructed exactly, including the metadata needed for decoding. This is
codec-independent and works across encoder backends; repairs are never video.
Source and repair datagrams fit the 1400-byte serialized wire budget before
transport encryption. Source payloads reserve room for the larger repair header.

The field polynomial is `0x11d`, with primitive element 2. The systematic
encoding matrix is `[I; C]`, where `C[r,i] = inverse((16+r) XOR i)` for repair
indices r=0–7 and local source indices i=0–15. The two index sets are disjoint,
so all square Cauchy submatrices are invertible. This permits recovery from any
K of K+R symbols. XOR uses coefficient 1 for every source instead.
This is WiVRn's own packet format and matrix definition, not an implementation
of the RFC 5510 wire protocol. There is no additional library dependency.

## Receiver and validation

The existing current/next frame buffering and incremental decoder submission
remain. The receiver stores distinct repair equations by block and repair index.
On a source or repair arrival it attempts recovery when missing source count
is at most the number of received independent repairs. It solves only the
missing-symbol system (at most 8x8), validates every reconstructed source, then
inserts the entire recovered set atomically in the original source slots.
Complete frames do not wait for parity. There is no new receive timer or partial
decode policy.

Repair counts/indices, ranges, lengths, unused size fields and body budgets are
validated. Conflicting/overlapping block descriptions and duplicate repair
indices cannot add recovery rank. Reconstructed stream/frame/shard IDs, lengths
and zero padding must match before insertion. Retired packets cannot submit a
frame twice. Erasure coding assumes received packets are intact; it is not a
substitute for transport integrity checks.

Tests exhaust all erasure patterns up to R for full 16+1/2/3/4 blocks and short
blocks, including lost repairs. Budgets 5–8 additionally exercise reproducible
random erasures and every contiguous burst up to the budget, including arbitrary
repair subsets, maximum-rank 8x8 recovery and rejection beyond capacity.
They compare decoded bytes/metadata, use an independent polynomial GF
multiplication oracle, and exercise in-place encrypted
wire round trips. Sender burst schedules cover uneven windows and wire budgets.
The actual accumulator with a mock decoder is tested for every erasure pattern
within 16+2 and 16+4 budgets, sampled/burst erasures for 16+5/6/7/8,
reordered/duplicate/conflicting repairs, and capacity-vs-missing-repair
diagnostic classification. Previous XOR regressions
remain. Hardware streaming performance and outcome rates require real trials.

## Client statistics

The separate **FEC statistics** sidebar page includes two graphs: recovered source
packets and incomplete video frames, counted per render sample (no smoothing).
Its counters total received source packets, valid repair packets, recovered
source packets, completed frames, frames completed with at least one FEC
recovery, and incomplete frames retired by the existing two-frame receiver.
Recovered packet counts are recorded when their frame completes or retires,
including recoveries that did not suffice to complete the frame. Rescued frames
are a subset of completed frames; completion means submission to the decoder,
not successful decoding or display. Each eye and optional alpha stream count
independently. Totals start with the streaming scene and survive decoder
reconfiguration once sampled. Receive counts include duplicate and late packets;
late parity for a healthy completed frame does not count as a recovery.
Unobserved frame IDs (including wholly lost frames) and buffered frames still
awaiting an outcome are not counted as incomplete. These are receiver outcome
statistics, not a measurement of network packet loss. No telemetry is uploaded.

## FEC diagnostic recording

Use **Record FEC diagnostics** on the **FEC statistics** page, reproduce the
problem, then use **Stop FEC recording** before copying the file. Recording
continues while the page is closed and across reconnects. The full file path,
write errors and queue overflow counts appear on that page. Steam launch
options or output redirection are unnecessary. Files use the previous client
recorder's directory and name format:

- Linux: `$XDG_STATE_HOME/wivrn/logs/video-*.jsonl`, defaulting to
  `~/.local/state/wivrn/logs/video-*.jsonl`. Relative XDG values are ignored.
- Android: the application's configuration directory, under `logs/`.

The schema-4 start record identifies `purpose: video_fec`, the client commit,
protocol revision and clock units. Each `frame` record describes a receiver
outcome, including healthy frames for a denominator. `receiver` distinguishes
decoder instances and reconnects; `stream` distinguishes eyes/alpha.

| Field | Meaning |
| --- | --- |
| `complete`, `recovered`, `reason` | Decoder submission, packets reconstructed, and the existing reason for completion/retirement |
| `sources_rx`, `repairs_rx`, `payload_bytes` | Received packets/payload bytes for this frame while recording; includes duplicates |
| `present`, `expected_min`, `end_known`, `missing_min` | Present source slots after FEC and a lower bound on absent slots; missing tail packets cannot be counted without end metadata |
| `missing_without_repair` | Known missing slots with no received repair covering them at retirement |
| `multiple_missing_groups` | Received repair blocks still missing two or more sources; not necessarily beyond RS capacity |
| `xor_blocks`, `rs_blocks_by_repairs` | Observed distinct XOR blocks, and RS block counts by declared repair budget 1–8; blocks whose repairs have not arrived are unknown |
| `over_capacity_groups` | Damaged blocks whose missing source count exceeds the total repair count sent |
| `insufficient_repair_groups` | Damaged blocks within the sent repair budget, but with too few distinct repairs received at retirement |
| `recoverable_unrecovered_groups` | Damaged blocks with enough distinct received repairs; indicates reconstruction/validation did not succeed |
| `unrecovered_single_groups` | Received repair groups with one remaining hole; indicates reconstruction/validation did not succeed |
| `missing`, `damaged_groups` | Missing source indices and damaged blocks' first index, count, relative missing bitmask, scheme, repair budget, distinct received repair count and repair-index bitmask |
| `max_gap_ns`, `receive_span_ns`, `age_ns` | Largest gap between this frame's arrivals, first-to-last arrival span and first-arrival-to-retirement time |
| `partial_capture` | Recording began/restarted after some of this frame's packets had already arrived |

Counts cover all observed slots/groups; detailed lists are capped at 64 entries,
with `lists_truncated` marking omissions. Arrival reordering includes deliberate
interleaving and does not by itself indicate network reordering.
`duplicate_sources` includes originals arriving after FEC filled their slot. Payload byte
counts exclude packet headers and are not link bitrate.

For the last 32 incomplete or FEC-rescued frames per receiver, `late` records
identify subsequently received original packets and repair ranges, with elapsed
time since retirement and `complete_at_retire`. Late repair records also include
the scheme, repair index and total repair count. These show confirmed lateness;
absence of a late record does not prove permanent loss. A missing repair may
have been lost, delayed, or not sent; variable sender group sizes cannot be
inferred for groups whose repair has not arrived. Wholly unseen frame IDs and
frames still buffered when recording stops have no outcome record.

The recorder is opt-in. It saves no encoded pixels, audio, poses or packet
payloads. The receive thread captures metadata into a bounded queue; a separate
writer performs disk I/O. Overflow drops diagnostic events and reports the
count, rather than waiting for the disk. Files are owner-readable/writable
(0600), and a new filename is created for every recording. Logs are local and
are never uploaded automatically. Receive policy, FEC code and wire format are
unchanged by recording.
