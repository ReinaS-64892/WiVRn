# Packet-level video FEC

Branch `feat/video-fec` starts at `70485093`, before the slice/receive-deadline
experiments on `feat/video-transport-resilience`. The experimental branch is
retained separately. This implementation adds video packet recovery without
changing encoder slice/tile settings, decoder error handling, receive waiting,
or frame selection. Client statistics and opt-in FEC diagnostics are described
below.

## Wire format

The original `video_stream_data_shard` layout is retained. View/pose information
is still on the first source packet, and timing/frame-end information is still
on the last. A new `video_stream_repair_shard` variant contains:

| Field | Meaning |
| --- | --- |
| stream_item_idx, frame_idx | Protected stream and frame |
| first_shard | First protected source packet index |
| source_count | Number of consecutive protected packets, 1–8 |
| source_sizes[8] | Serialized source-body lengths; unused entries are zero |
| payload | XOR of the serialized source bodies, zero-padded to the longest |

The protected body includes the original source header, optional metadata and
video payload, excluding the outer packet type tag. This allows exact recovery
of a lost first or last packet, including the metadata needed for decoding and
frame completion. The algorithm is codec-independent and has no NAL/slice
boundary descriptors. A repair's payload is never interpreted as video.

With interleaving enabled (the default), the sender prepares at most 32 source
packets from the already available encoder output buffer. It divides each window
into up to four consecutive source ranges of at most eight packets, then sends
one symbol per group in round-robin order. Each group's last symbol is its
repair. For a full window this gives:

```
0,8,16,24, 1,9,17,25, ... 7,15,23,31, repairA,repairB,repairC,repairD
```

Short windows use ceil(N/8) balanced groups whose sizes differ by at most one.
Repairs participate in the same round-robin schedule, including when another
group is still sending data. This keeps a tail source separated from its own
repair and does not add repair packets compared to the original grouping of
that window. For example, nine sources use groups of five and four:

```
0,5, 1,6, 2,7, 3,8, 4,repairB, repairA
```

Within a window of G groups, any single contiguous burst of up to G emitted
video datagrams (including repairs) leaves at most one erasure in each group.
Thus a full four-group window tolerates a burst of four, assuming the other
packets arrive. A shorter buffer/window has fewer groups and less burst
protection. Multiple bursts or two missing symbols in a group can still defeat
recovery; this is not a multi-erasure code within each group.

Windows never cross encoder output buffers, streams or frames. In particular,
small per-NAL encoder callbacks may offer only one group even when a whole frame
contains more data. The sender adds no sleep, timer or wait for a later encoder
output; it reorders only already encoded data and flushes the final partial
window immediately. Reordering does delay some early contiguous source data
relative to sequential sending, so decoder input timing still needs hardware
measurement. Temporary storage is bounded to 32 source descriptors and four
repair symbols per send call; source payloads reference the live encoder buffer.

All group parity is captured before the first send, because UDP encryption
mutates source payload spans in place. Repairs then use normal transport
encryption. Existing TCP/control delivery and FEC-disabled source order are
unchanged.

With FEC enabled, source and repair datagrams fit the 1400-byte serialized wire
budget before transport encryption. On protected UDP video, source payloads
reserve enough room for the larger repair header. Variable source lengths
accommodate the first/last metadata without repeating it on every source.

## Receiver and compatibility

The existing current/next frame buffering and incremental decoder submission
are retained. Repairs use separate storage; exactly one missing source in a
group can be recovered. Received operand lengths and identifiers, repair ranges
and reconstructed packet fields are checked before inserting the recovered
source into its original slot. Duplicated/overlapping repairs and retired-frame
repairs cannot cause a second submission. Two source losses in the same group
remain unrecoverable. Complete frames do not wait for parity. There is no new
receive timer, partial-slice decode path, or decoder fallback.

The new packet variant changes the automatic protocol hash (revision 2).
Install matching server and client builds; neither the base branch nor the
older experimental branch is wire-compatible with this branch. Interleaving
itself changes only sender scheduling/group boundaries, not the protocol: the
FEC-only client from `61123918` already supports this sender.

`WIVRN_VIDEO_FEC=0` in the **server process environment** disables repair
transmission and retains the original source packetization. FEC is enabled by
default. `WIVRN_VIDEO_FEC_INTERLEAVE=1` selects sequential 8+1 sending for an A/B
comparison; values 2, 3 or 4 select that maximum group depth (default 4).
Out-of-range/non-integer values are logged and fall back to 4. These settings
belong to the server process environment. No client configuration is needed.
Full groups add one packet per eight
sources; headers, short output buffers and padding increase byte overhead
beyond the nominal 12.5%. Encoder bitrate is unchanged. This is a fixed XOR
scheme, not adaptive congestion control or a multi-erasure code.

Tests cover source/repair serialization and wire limits, every single missing
position, first/last metadata, unequal symbol lengths, multiple groups, multiple
loss rejection, malformed groups, transport encryption, reordered/duplicate
arrivals, and insertion through the real shard accumulator with a mock decoder.
Interleave tests sweep every burst position, including parity losses and uneven
1–32-source windows, and exercise bursts across consecutive full windows in the
real receiver. The encrypted sending callbacks test that parity is built before
any source mutation.
Actual headset performance and loss tolerance still require hardware testing.

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

The schema-2 start record identifies `purpose: video_fec`, the client commit,
protocol revision and clock units. Each `frame` record describes a receiver
outcome, including healthy frames for a denominator. `receiver` distinguishes
decoder instances and reconnects; `stream` distinguishes eyes/alpha.

| Field | Meaning |
| --- | --- |
| `complete`, `recovered`, `reason` | Decoder submission, packets reconstructed, and the existing reason for completion/retirement |
| `sources_rx`, `repairs_rx`, `payload_bytes` | Received packets/payload bytes for this frame while recording; includes duplicates |
| `present`, `expected_min`, `end_known`, `missing_min` | Present source slots after FEC and a lower bound on absent slots; missing tail packets cannot be counted without end metadata |
| `missing_without_repair` | Known missing slots with no received repair covering them at retirement |
| `multiple_missing_groups` | Received repair groups still missing two or more sources, beyond XOR recovery capacity |
| `unrecovered_single_groups` | Received repair groups with one remaining hole; indicates reconstruction/validation did not succeed |
| `missing`, `damaged_groups` | Missing source indices and damaged groups' first index, count and relative missing bitmask |
| `max_gap_ns`, `receive_span_ns`, `age_ns` | Largest gap between this frame's arrivals, first-to-last arrival span and first-arrival-to-retirement time |
| `partial_capture` | Recording began/restarted after some of this frame's packets had already arrived |

Counts cover all observed slots/groups; detailed lists are capped at 64 entries,
with `lists_truncated` marking omissions. Arrival reordering includes deliberate
interleaving and does not by itself indicate network reordering.
`duplicate_sources` includes originals arriving after FEC filled their slot. Payload byte
counts exclude packet headers and are not link bitrate.

For the last 32 incomplete or FEC-rescued frames per receiver, `late` records
identify subsequently received original packets and repair ranges, with elapsed
time since retirement and `complete_at_retire`. These show confirmed lateness;
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
