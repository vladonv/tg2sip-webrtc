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

## 0001-tg2sip-fix-h264-ffmpeg7-incompatible.patch

Fixes `modules/video_coding/codecs/h264/h264_decoder_impl.cc` (in `tg_owt`,
tgcalls' WebRTC fork) to actually compile against FFmpeg 7.x, instead of
excluding H264 from the build entirely (an earlier version of this patch,
through 2026-07-17, did the latter - see below for why that changed).

Three small, version-portable fixes:
- `AVCodecContext`/`AVFrame::reordered_opaque` was removed upstream in
  FFmpeg 7.x. This code only ever used it to carry a per-frame timestamp
  through decode (the comment right above says "we don't expect
  reordering"), which `pts` already does - and does on every FFmpeg version,
  not just 7.x - so the fix switches to `packet.pts`/`av_frame_->pts`
  instead of manually copying `reordered_opaque` between the context and
  the frame.
- `avcodec_find_decoder()`'s return type was tightened to `const AVCodec*`
  in FFmpeg 7.x; the local variable it was assigned to is now `const` too.
  This compiles fine against older FFmpeg as well (binding a non-const
  return value to a `const` pointer variable is always legal).

Verified 2026-07-17 building clean end-to-end (including link) on both
Debian 12 (bookworm, FFmpeg 5.1.9 via `libavcodec` 59.37.100) and Debian 13
(trixie, FFmpeg 7.1.5) - neither fix is FFmpeg-7-specific, both are the
actively-maintained/stable APIs on every version tested.

**Why this replaced the original drop-H264 patch**: investigating a
separate, unrelated iOS-caller silent-audio bug (see the
`project-tg2sip-ios-silent-audio-investigation` memory) raised the question
of whether H264 being unavailable (leaving only VP8/VP9 in tg2sip's
advertised codec list) could be provoking the peer's odd behavior. Testing
that hypothesis needed a genuinely working H264 decoder, not just a
codec-list entry with nothing behind it - so it was fixed for real rather
than faked. (For the record: re-tested live with H264 available end-to-end
- the hypothesis didn't hold, that bug is unrelated to H264. But the fix
itself is worth keeping regardless: it restores real, working H264 decode
against current FFmpeg with no behavioral downside, and no longer relies on
`modules/video_coding/codecs/h264/h264.cc`'s `#ifdef WEBRTC_USE_H264`
absent-fallback stub path at all.)

tg2sip-webrtc still never negotiates video with peers either way (see
0002 below) - this patch only concerns whether H264 successfully *builds*
into `tg_owt`, not whether tg2sip-webrtc uses it for anything.

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

## 0001-tg2sip-ntgcalls-add_subdirectory-fixes.patch, 0002-tg2sip-ntgcalls-missing-includes.patch

Applied to the `third_party/ntgcalls` submodule (`github.com/pytgcalls/ntgcalls`,
pinned at `v2.2.5` - the replacement calling backend, see the ntgcalls
migration plan/memory: `project_tg2sip_ntgcalls_migration_feasibility`).
Found while getting a standalone scratch consumer of ntgcalls to link
before touching tg2sip's own source - ntgcalls has apparently never actually
been exercised as an `add_subdirectory()` dependency by its own maintainers
(only as a standalone top-level build via `python3 setup.py build_lib`),
so every one of these is a real, `add_subdirectory()`-specific bug, not
anything to do with tg2sip itself:

- `CMakeLists.txt` never `include()`s `cmake/PlatformUtils.cmake`,
  `cmake/DownloadProject.cmake`, `cmake/GitUtils.cmake` even though it calls
  functions those files define (`GetProperty`, `DownloadProject`,
  `GitClone`) - only worked in upstream's own build via some undocumented
  path. Fixed by adding the missing `include()` calls.
- `cmake/PlatformUtils.cmake`'s `setup_platform_flags()` resolves a
  `cmake_dir` variable via `${CMAKE_SOURCE_DIR}/cmake` - under
  `add_subdirectory()`, `CMAKE_SOURCE_DIR` is fixed to the *outermost*
  project (tg2sip-webrtc's own root), not ntgcalls' - so this always
  resolved to the wrong path. Fixed with `CMAKE_CURRENT_LIST_DIR` instead.
- Several `Find*.cmake` files (`FindWebRTC.cmake`, `FindGLib.cmake`,
  `FindX11.cmake`, `FindBoost.cmake`, `FindMesa.cmake`, `FindOpenH264.cmake`,
  `FindFFmpeg.cmake`) create `IMPORTED` targets without `GLOBAL` - invisible
  outside the directory scope they're defined in, so tg2sip-webrtc's own
  top-level `target_link_libraries()` calls against them fail to resolve.
  Fixed by adding `GLOBAL` to each.
- `cmake/Linux.cmake` is the only platform file (unlike `Toolchain.cmake`
  for Windows, `Android.cmake`, `macOS.cmake`) missing
  `_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE` in its
  `target_compile_definitions` - without it, libc++'s own
  `__configuration/hardening.h` hits a hard `#error`. Fixed by adding the
  same define the other platforms already have.
- `wrtc/src/models/i420_image_data.cpp`, `wrtc/src/utils/bignum.cpp`,
  `wrtc/src/utils/encryption.cpp` use `std::memcpy`/`std::move` etc without
  directly including `<cstring>`/`<utility>` - relied on transitive includes
  that don't hold under every libc++ configuration. Fixed with explicit
  includes.

Not fixed by a patch (a real, unavoidable requirement handled directly in the
ntgcalls `add_subdirectory()` block in the root `CMakeLists.txt` instead):
ntgcalls' own C++ sources need C++20 and WebRTC's own headers need
`-isystem` rather than plain `-I` to avoid a GCC/Clang `-Wchanges-meaning`
hard error - both scoped to ntgcalls' own targets only. tg2sip's own code
(`controller.cpp` etc) only includes the flat C API header
(`third_party/ntgcalls/include/ntgcalls.h` - plain C, `extern "C"`, no C++
types in any public signature) precisely so it does *not* need to adopt
C++20 or ntgcalls' Chromium-libc++ toolchain itself - see the "Boundary
decision" section of the ntgcalls migration plan
(`/home/vlad/.claude/plans/jaunty-roaming-cat.md`) for why that boundary
was chosen.
