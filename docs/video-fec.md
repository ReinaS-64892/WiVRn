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

Each group emits one repair after eight source packets, or at the end of an
encoder output buffer for a short group. Groups never cross streams or frames;
source order is retained. No source is held back waiting to build a group.
Plaintext parity is captured before UDP's in-place encryption; repairs then use
the normal encrypted transport. Existing TCP/control delivery is unprotected.

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
older experimental branch is wire-compatible with this branch.

`WIVRN_VIDEO_FEC=0` in the **server process environment** disables repair
transmission and retains the original source packetization. FEC is enabled by default. Full groups add one packet per eight
sources; headers, short output buffers and padding increase byte overhead
beyond the nominal 12.5%. Encoder bitrate is unchanged. This is a fixed XOR
scheme, not adaptive congestion control or a multi-erasure code.

Tests cover source/repair serialization and wire limits, every single missing
position, first/last metadata, unequal symbol lengths, multiple groups, multiple
loss rejection, malformed groups, transport encryption, reordered/duplicate
arrivals, and insertion through the real shard accumulator with a mock decoder.
Actual headset performance and loss tolerance still require hardware testing.
