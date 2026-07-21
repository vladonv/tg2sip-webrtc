# patches/

Small local patches applied to the `third_party/ntgcalls` git submodule
(`github.com/pytgcalls/ntgcalls`) at CMake configure time - see the
`execute_process(COMMAND ${GIT_EXECUTABLE} apply ...)` block near the top of
the root `CMakeLists.txt`.

These are **not** committed into the submodule itself: this project has no
fork of `pytgcalls/ntgcalls` to carry them, and local edits inside a
submodule checkout don't survive `git submodule update` or a fresh clone.
Applying them from the parent project's build instead keeps them versioned
alongside the rest of tg2sip-webrtc and makes a fresh
`git submodule update --init` + `cmake` always reproduce the same working
tree.

Patch application is idempotent (checked via `git apply --reverse --check`
before applying) - safe to run `cmake` repeatedly, and safe after a
`git submodule update` that leaves the submodule at its clean upstream
state.

## 0001-tg2sip-ntgcalls-add_subdirectory-fixes.patch, 0002-tg2sip-ntgcalls-missing-includes.patch

Applied to the `third_party/ntgcalls` submodule (pinned at `v2.2.5` - the
calling backend, see the ntgcalls migration plan/memory:
`project_tg2sip_ntgcalls_migration_feasibility`). Found while getting a
standalone scratch consumer of ntgcalls to link before touching tg2sip's
own source - ntgcalls has apparently never actually been exercised as an
`add_subdirectory()` dependency by its own maintainers (only as a
standalone top-level build via `python3 setup.py build_lib`), so every one
of these is a real, `add_subdirectory()`-specific bug, not anything to do
with tg2sip itself:

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

## 0003-tg2sip-ntgcalls-fix-audio-incoming-enable-check.patch

`StreamManager::optimizeSources()` (`ntgcalls/src/stream_manager.cpp`)
computed `enableAudioIncoming()` from `writers`/`externalWriters`, which
`handlePlaybackConfig()` populates for Playback-mode devices. `Microphone`
is a Capture-mode device (its own config lives in `readers`/
`externalReaders`, populated by `handleCaptureConfig()`), so
`writers.contains(Microphone)` can never be true for any real call -
`enableAudioIncoming(true)` was effectively unreachable, and
`NativeNetworkInterface::addIncomingSmartSource()` never constructed an
`IncomingAudioChannel` for the remote peer's audio.

Found via a live P2P call with an external audio source: SRTP activated
correctly and decrypted RTP genuinely arrived (confirmed via `tcpdump`),
but `ntg_on_frames` never fired even once - traced to the incoming audio
channel never being created in the first place. Fixed by reusing the
existing `hasDeviceInternal()` helper, which already implements the
correct per-mode map lookup - also reported upstream:
https://github.com/pytgcalls/ntgcalls/pull/52

## Tried and abandoned: forcing TCP-only TURN for `voip_proxy_*`

2026-07-17 (against the old tgcalls backend, since replaced by ntgcalls -
see above): attempted a patch forcing every ICE candidate onto TCP-TURN
(`PORTALLOCATOR_DISABLE_UDP`/`_STUN`/`_UDP_RELAY` + `cricket::PROTO_TCP`
instead of `PROTO_UDP` for TURN servers) when `voip_proxy_*` is configured,
on the theory that WebRTC's SOCKS5 proxy support only covers TCP sockets
(true) so forcing TCP would let the proxy actually carry the media.

**Reverted - confirmed dead end via packet capture.** The SOCKS5 CONNECT to
a real Telegram TURN server succeeds and the old backend sends a
correctly-formed STUN/TURN Allocate Request (retransmitted 3x per the
normal backoff), but the TURN server never sends back any response at all -
Telegram's TURN infrastructure on the ports/IPs it hands out does not
appear to serve the TURN protocol over TCP, even though the bare TCP port
accepts a connection. See the `project-tg2sip-proxy-untested` memory for
the full investigation (including a real use-after-free crash found and
fixed along the way, unrelated to this specific patch). Not fixable from
tg2sip's side, and moot now anyway - ntgcalls has no proxy hook at all in
its API (see the same memory's 2026-07-21 update) - if proxying VoIP media
is revisited, it would need to be built from scratch against ntgcalls, and
the same underlying infrastructure limits (Telegram's TURN doesn't serve
TCP; the specific proxy tested doesn't support SOCKS5 UDP ASSOCIATE) would
still apply. A transparent/TProxy-mode proxy at the network/router level
instead of app-level SOCKS5 remains the one approach that sidesteps both,
needing no tg2sip-webrtc code changes at all.
