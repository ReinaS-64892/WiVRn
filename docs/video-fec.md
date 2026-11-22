# Packet-level video FEC

Branch `feat/video-fec` starts at `70485093`, before the slice/receive-deadline
experiments on `feat/video-transport-resilience`. The experimental branch is
retained separately. This implementation adds video packet recovery without
changing encoder slice/tile settings, decoder error handling, receive waiting,
frame selection, or the client interface.

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
