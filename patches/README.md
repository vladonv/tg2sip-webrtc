# patches/

Small local patches applied to the `third_party/tgcalls` git submodule
(`github.com/MarshalX/tgcalls`) at CMake configure time - see the
`execute_process(COMMAND ${GIT_EXECUTABLE} apply ...)` block near the top of
the root `CMakeLists.txt`.

These are **not** committed into the submodule itself: this project has no
fork of `MarshalX/tgcalls` to carry them, and local edits inside a submodule
checkout don't survive `git submodule update` or a fresh clone. Applying
them from the parent project's build instead keeps them versioned alongside
the rest of tg2sip-webrtc and makes a fresh `git submodule update --init` +
`cmake` always reproduce the same working tree.

Patch application is idempotent (checked via `git apply --reverse --check`
before applying) - safe to run `cmake` repeatedly, and safe after a
`git submodule update` that leaves the submodule at its clean upstream
state.

## 0001-tg2sip-drop-h264-ffmpeg7-incompatible.patch

Removes the FFmpeg-backed H264 decoder/encoder from `tg_owt` (tgcalls'
WebRTC fork). `modules/video_coding/codecs/h264/h264_decoder_impl.cc`
doesn't compile against FFmpeg 7.x (the distro version on Debian trixie,
where this migration's build spike was done): `AVFrame`/
`AVCodecContext::reordered_opaque` was removed upstream as part of FFmpeg's
own API modernization. tg2sip-webrtc is an audio-only SIP gateway - no video
calls - so H264 support isn't needed. Safe to drop: `modules/video_coding/codecs/h264/h264.cc`
already has a proper `#ifdef WEBRTC_USE_H264` fallback path (the same
pattern already used for the AV1 `_absent` stubs), so undefining
`WEBRTC_USE_H264` and excluding the two files just makes
`CreateH264Decoder`/`Encoder` report unsupported instead of failing to
compile/link.

If a future tgcalls/tg_owt update upstream fixes FFmpeg 7.x compatibility
for H264, or if this project ever needs to support video calls, this patch
should be dropped and reassessed rather than blindly reapplied.

## 0002-tg2sip-dont-advertise-video-support.patch

Stops `MediaManager` from falsely advertising video-receive capability to
peers. Previously `_myVideoFormats` was populated from `tg_owt`'s platform
VP8/VP9 codec-factory `GetSupportedFormats()` **regardless of whether an
actual `videoCapture` device exists** - tg2sip-webrtc never provides one (it
has no camera, no video renderer, and is audio-only by design), but the
codec factories themselves don't know that and report generic platform
capability anyway. `setPeerVideoFormats()` intersects this against whatever
the peer offers; a non-empty intersection tells the peer "receiving video is
negotiated," even though nothing on this side would ever render it.

Found 2026-07-17 while investigating the pre-existing iOS-callee/caller
silent-audio bug (see `project-tg2sip-ios-silent-audio-investigation`
memory): a real iOS caller on a nominally voice-only call (camera
confirmed off, TDLib's own `call.is_video_` reported `false`) sent
substantial real video data instead of audio, apparently starving audio
bandwidth on the sender's side. This patch (`_myVideoFormats = {}`) is
correct regardless - tg2sip should never claim video capability it doesn't
have - but empirically it did **not** stop the iOS peer from sending video
data (re-tested live, no change), so it does not fix that silent-audio bug
by itself. Kept as a real, independent correctness fix; the silent-audio
investigation itself is documented separately and remains unresolved,
believed to be iOS-Telegram-client-side and outside this project's control.

## Tried and abandoned: forcing TCP-only TURN for `voip_proxy_*`

2026-07-17: attempted a patch forcing every ICE candidate onto TCP-TURN
(`PORTALLOCATOR_DISABLE_UDP`/`_STUN`/`_UDP_RELAY` + `cricket::PROTO_TCP`
instead of `PROTO_UDP` for TURN servers) when `voip_proxy_*` is configured,
on the theory that WebRTC's SOCKS5 proxy support only covers TCP sockets
(true) so forcing TCP would let the proxy actually carry the media.

**Reverted - confirmed dead end via packet capture.** The SOCKS5 CONNECT to
a real Telegram TURN server succeeds and `tgcalls` sends a correctly-formed
STUN/TURN Allocate Request (retransmitted 3x per the normal backoff), but
the TURN server never sends back any response at all - Telegram's TURN
infrastructure on the ports/IPs it hands out does not appear to serve the
TURN protocol over TCP, even though the bare TCP port accepts a connection.
See the `project-tg2sip-proxy-untested` memory for the full investigation
(including a real use-after-free crash found and fixed along the way,
unrelated to this specific patch). Not fixable from tg2sip's side - if
proxying VoIP media is revisited, look at either (a) implementing SOCKS5
`UDP ASSOCIATE` support in `NetworkManager.cpp`'s socket creation path (real
UDP relay, but needs a new `rtc::AsyncPacketSocket` and confirmation the
target proxy actually supports the UDP ASSOCIATE command), or (b) a
transparent/TProxy-mode proxy at the network/router level instead of
app-level SOCKS5, which needs no tg2sip-webrtc code changes at all.
