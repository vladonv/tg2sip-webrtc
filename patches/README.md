# patches/

Small local patches applied to the `third_party/tgcalls` git submodule
(`github.com/MarshalX/tgcalls`) at CMake configure time - see the
`execute_process(COMMAND ${GIT_EXECUTABLE} apply ...)` block near the top of
the root `CMakeLists.txt`.

These are **not** committed into the submodule itself: this project has no
fork of `MarshalX/tgcalls` to carry them, and local edits inside a submodule
checkout don't survive `git submodule update` or a fresh clone. Applying
them from the parent project's build instead keeps them versioned alongside
the rest of tg2sip and makes a fresh `git submodule update --init` +
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
own API modernization. tg2sip is an audio-only SIP gateway - no video calls
- so H264 support isn't needed. Safe to drop: `modules/video_coding/codecs/h264/h264.cc`
already has a proper `#ifdef WEBRTC_USE_H264` fallback path (the same
pattern already used for the AV1 `_absent` stubs), so undefining
`WEBRTC_USE_H264` and excluding the two files just makes
`CreateH264Decoder`/`Encoder` report unsupported instead of failing to
compile/link.

If a future tgcalls/tg_owt update upstream fixes FFmpeg 7.x compatibility
for H264, or if this project ever needs to support video calls, this patch
should be dropped and reassessed rather than blindly reapplied.
