# Configurable items

Configuration is split between headset and server. Headset contains most configuration, while server has system specific items such as encoders and paths.
Files are read from
- `/usr/share/wivrn/config.json` (where `/usr` is selected at configure time with `CMAKE_INSTALL_PREFIX`)
- `/etc/wivrn/config.json`
- `$XDG_CONFIG_HOME/wivrn/config.json` or if `$XDG_CONFIG_HOME` is not set, `$HOME/.config/wivrn/config.json`.

Files later in the list replace top-level values from previous ones.

If you installed WiVRn from a flatpack, the config is in `$HOME/.var/app/io.github.wivrn.wivrn/config/wivrn/config.json`.

All elements are optional and have default values.

## `bit-depth`
Default value: 10-bit if supported by the selected codec, encoder and decoder. 8-bit otherwise.

Bit depth of the video. 8-bit is supported by all encoders. 10-bit is supported by `vulkan`, `vaapi` and `nvenc` with `h265` or `av1` codecs.

## `encoder`
The encoder to use, either a single string or object applied to all streams, or a list of string or objects with values for left, right and alpha.
When a string it is used, it is equivalent to the `encoder` item of the object.

WiVRn encodes each eye separately, and the alpha channel as one for both eyes. Each stream is processed independently, this may use resources more effectively and reduce latency.

### `encoder`
Default value, in order: `vulkan` if supported, `nvenc` on Nvidia GPU, `vaapi` on all other GPU, else `x264`.

Identifier of the encoder, one of
* `x264`: software encoding
* `nvenc`: Nvidia hardware encoding
* `vaapi`: AMD/Intel hardware encoding
* `vulkan`: Vulkan Video encode, cross-vendor hardware encoding

### `codec`
Default value: best supported by both headset and encoder of `av1`, `h264`, `h265`.

One of `h264`, `h265`, `av1`, `raw`.

Not all encoders support every codec:
- `x264` encoder only supports `h264` codec
- `vulkan` encoder supports `h264` and `h265` codecs
- `raw` encoder only supports `raw` codec
- `nvenc` and `vaapi` support all codecs, except `raw`

If `nvenc` encoder is in use, you can refer to [nvidia website](https://developer.nvidia.com/video-encode-decode-support-matrix) to make sure that your GPU supports encoding with the desired codec.

### Examples
1. Simple configuration
```json
{
	"encoder": {
		"encoder": "vaapi",
		"codec": "h265"
	}
}
```
Use vaapi hardware encoding, h265 video codec (HEVC).

2. Hardware + software encoder
```json
{
	"encoder": [
		{
			"encoder": "vaapi",
			"codec": "h265",
		},
		{
			"encoder": "x264",
			"codec": "h264",
		},
		{
			"encoder": "vaapi",
			"codec": "h265",
		},
	]
}
```
Creates a hardware encoder for left eye and transparency, and a software encoder for right eye.

### `device`, only for vaapi
Default value: unset

Manually specify the device for encoding, can be used to offload encode to an iGPU. Device shall be in the form "/dev/dri/renderD128".


### `options` (very advanced), only for vaapi
Default value: unset

Json object of additional options to pass directly to ffmpeg `avcodec_open2`'s `option` parameter.

## `application`
Default value: unset

An application to start when connection with the headset is established, can be a string or an array of strings if parameters need to be provided.

### Example
```json
{
	"application": ["steam", "steam://launch/275850/VR"]
}
```
Launch No Man's Sky in VR mode on Steam when connection with headset is established.

## `tcp-only`
Default value: `false`

Only use TCP for communications with the client, this may have increased latency.
If `false` or unset, WiVRn will use both TCP and UDP.

### Example
```json
{
	"tcp-only": true
}
```

## `publish-service`
Default value: `avahi`

How to publish the service over the network, `avahi` or null.

If set to null, service will not be published and address has to be entered manually on the headset.

## `openvr-compat-path`
Default value: unset

Provides the path to the directory of an OpenVR compatibility tool (such as OpenComposite).

If unset, WiVRn will autodetect the path of such a tool as usual (see [the SteamVR guide](./steamvr.md)).

If set to an null, WiVRn will not manage the OpenVR configuration.

## `hid-forwarding`
Default value: `false`

Only available when the `uinput` kernel module is loaded and the user has write access.

Mirrors input devices forwarded from the headset (keyboard, mouse, gamepad) to `uinput`
devices, for applications that do not read them through OpenXR. Which devices are forwarded is
chosen on the headset.

A forwarded gamepad is also exposed as a native OpenXR gamepad at `/user/gamepad`, which needs
no permission and is always available. Only the digested controller state is forwarded
(buttons, two sticks, analog triggers and a d-pad), so device specific features such as gyro,
adaptive triggers or the touchpad are not available.

## `debug-gui`
Default value: `false`

Only available when built with `WIVRN_FEATURE_DEBUG_GUI`.

Enables the Monado debug gui.

## `use-steamvr-lh`
Default value: `false`

Only available when built with `WIVRN_FEATURE_STEAMVR_LIGHTHOUSE`.

Enables the driver to load SteamVR Lighthouse devices.

While a runtime session is running, the dashboard's **Turn off all trackers**
button sends `EnterStandby()` to all Lighthouse trackers, including devices added
after startup. Controllers and base stations are excluded. The action also works
while the session is waiting for the headset to reconnect. It requests standby;
the individual device's driver determines whether the hardware powers off.

## `lh-max-extrapolation`
Default value: unset

Only available when built with `WIVRN_FEATURE_STEAMVR_LIGHTHOUSE`.

Maximum time in milliseconds that poses may be extrapolated ahead for SteamVR Lighthouse devices.

## `lh-stick-deadzone`
Default value: `0`

Only available when built with `WIVRN_FEATURE_STEAMVR_LIGHTHOUSE`

Applies a deadzone to joysticks on SteamVR controllers (e.g. Index).

## `port`
Default value: `9757`

Change the TCP/UDP port used for the connection.

## `hostname`
Default value: unset

If set, overrides the name displayed in the server list.

## `port-forwarding`

Default: `[]` (disabled).

Forwards application TCP streams or UDP datagrams between the connected headset
and PC. Configure rules in the Dashboard's **Settings → Port forwarding** section,
then reconnect the headset. Both the client and server must be built with this
feature: the handshake extension changes WiVRn's protocol hash.

Each rule has:

- `protocol`: `"tcp"` or `"udp"`.
- `source`: `"headset"` or `"pc"`, the side which listens for local applications.
- `listen-port`: local application port on the source side (1–65535).
- `destination-port`: application port on the other side (1–65535).
- `bidirectional`: whether application data can also return through the same
  connection or UDP sender mapping. Default: `false`. This does not create a
  listening port on the destination side. For ordinary TCP protocols, use `true`.

For example, HMD applications sending OSC to `127.0.0.1:9000` can reach VRChat on
PC, and a PC application can connect to an HMD service through PC localhost:

```json
"port-forwarding": [
  {
    "protocol": "udp",
    "source": "headset",
    "listen-port": 9000,
    "destination-port": 9000,
    "bidirectional": false
  },
  {
    "protocol": "tcp",
    "source": "pc",
    "listen-port": 47280,
    "destination-port": 47280,
    "bidirectional": true
  }
]
```

VRChat's outgoing OSC normally targets UDP port 9001. To receive that on the
headset, add a separate PC → headset UDP rule from 9001 to 9001; enabling replies
on the 9000 rule does not redirect traffic addressed to a different port.

All application listeners and destinations are IPv4 `127.0.0.1`. No root or TUN
interface is required. Broadcast/multicast discovery, OSCQuery discovery and
arbitrary LAN destinations are not forwarded. Configure external applications
with localhost explicitly. Forwarding supplies connectivity; it does not port
third-party drivers to Monado or provide access to headset cameras.

Forwarding uses dedicated TCP/UDP sockets and its own worker thread, never the
VR data sockets. Auxiliary records use AES-256-GCM with separate session keys
sent through WiVRn's existing control connection. Keep WiVRn encryption enabled
for protection of those keys. An auxiliary failure disables forwarding until
WiVRn reconnects, without stopping VR streaming. The server worker starts after
the runtime process forks.

There is currently no bandwidth limiter: physical bandwidth is shared with VR.
Buffers and active flows have finite memory/resource bounds (4 MiB per output
buffer, 256 flows per endpoint, 64 rules). UDP sender mappings expire after 60
seconds without traffic. UDP payloads up to 65,472 bytes are supported; larger
ones are dropped. Large datagrams may require IP fragmentation on the network.
An occupied application port disables forwarding on that endpoint and is
reported in the client/server log.

## `port-forwarding-port`

Default: `9758`.

Like WiVRn's main `port` setting, this is a fixed PC listening port. Set
`"port-forwarding-port": 9758` in the server configuration to override it
(valid range: 1–65535). No automatic port selection is used for sessions; if
this port is occupied, forwarding is disabled and the error is logged. The
headset learns the configured port through the WiVRn handshake.

Dedicated PC port used by the auxiliary TCP and UDP connections. Like the main
`port` setting, configure it in the server configuration file; no Dashboard port
control is provided. Allow both protocols through the PC firewall when forwarding
is configured; the standard WiVRn firewall rule for 9757 does not cover this port.
No auxiliary sockets are opened when the rule list is empty.

For an ADB TCP-only WiVRn connection, also reverse this TCP port (for example,
`adb reverse tcp:9758 tcp:9758`). TCP application forwarding can then work over
that separate connection. UDP forwarding requires IP connectivity; it does not
fall back to the VR TCP connection or to ADB TCP forwarding.
