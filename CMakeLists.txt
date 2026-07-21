cmake_minimum_required(VERSION 3.27) # ntgcalls' own CMakeLists.txt requires 3.27
project(tg2sip-webrtc)

set(CMAKE_CXX_STANDARD 17)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(Td REQUIRED) # Debian's libtd-dev shim only supports an exact
                          # version match or "unversioned = latest installed"
                          # (currently 1.8.38, well above the 1.7.10 floor
                          # this project was originally built against)
find_package(spdlog REQUIRED) # Debian ships 1.15.2; CMake's default version
                              # compatibility check rejects it against "0.17"
                              # (different major), but spdlog's own API
                              # compat is much wider than that check implies
pkg_check_modules(PJSIP libpjproject>=2.8 REQUIRED)
pkg_check_modules(OPUS opus REQUIRED)

find_package(Git REQUIRED) # still needed for the ntgcalls patch-apply block below

pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale libswresample)

# ---- ntgcalls (replacement calling backend - see the migration plan in
# ~/.claude/plans/jaunty-roaming-cat.md and the
# project_tg2sip_ntgcalls_migration_feasibility memory for the full story).
# Vendored as the third_party/ntgcalls git submodule
# (github.com/pytgcalls/ntgcalls), pinned to v2.2.5 - same LGPLv3-core
# licensing-boundary rationale the old third_party/tgcalls submodule used
# (a submodule keeps the licensing boundary explicit instead of blurring it
# by vendoring the source directly alongside tg2sip's own GPLv2 files).
#
# third_party/tgcalls itself (and the tg_owt WebRTC fork it built from
# source) is no longer part of this build at all - not just superseded at
# the tg2sip source level (controller.cpp/audio_bridge.cpp no longer call
# into it, see below), but genuinely unbuildable together with ntgcalls:
# both vendor forks/snapshots of the same upstream Chromium WebRTC
# codebase, so thousands of plain (non-template, no STL types in the
# mangled name - so no libc++/libstdc++ ABI-namespace protection either)
# C++ symbols like webrtc::SyncBuffer::PushFrontZeros collide at link time
# between tg_owt's libtg_owt.a and ntgcalls' prebuilt libwebrtc.a. Found by
# actually trying it (150+ duplicate-symbol linker errors) - the C-API ABI
# boundary below only solves the libc++-vs-libstdc++ problem, it doesn't
# and can't solve two copies of the same symbols existing in one binary.
# The third_party/tgcalls submodule and patches/0001-.../0002-... (its own
# patches, unrelated to the two ntgcalls ones below) are left in git for
# now rather than deleted outright - nothing in tg2sip's own source
# references them anymore, but removing the submodule/patches themselves is
# left as a separate, deliberate cleanup commit rather than folded in here.
#
# tg2sip's own code (voip/ntgcalls_runtime.{h,cpp}, controller.{h,cpp})
# talks to ntgcalls exclusively through its flat C API
# (third_party/ntgcalls/include/ntgcalls.h) rather than its C++ classes.
# Reason: ntgcalls' prebuilt WebRTC static library is unconditionally built
# against Chromium's own vendored libc++ (namespaced std::__Cr::..., fetched
# below via USE_LIBCXX=ON), a different, incompatible ABI from the plain
# system libc++ this project's own code/TDLib/PJSIP were rebuilt against in
# the Clang+libc++ toolchain migration (CMakePresets.json's "clang-libcxx"
# preset). The flat C API has no C++ name-mangling/STL-ABI surface at all,
# so it's a safe boundary between the two differently-compiled halves of
# the same final binary - neither half needs to match the other's ABI,
# they just can't pass STL types across a C++ function call directly.
if (NOT EXISTS ${CMAKE_SOURCE_DIR}/third_party/ntgcalls/CMakeLists.txt)
    message(FATAL_ERROR "third_party/ntgcalls submodule not initialized - run: "
                        "git submodule update --init third_party/ntgcalls")
endif ()

foreach (patch_file 0001-tg2sip-ntgcalls-add_subdirectory-fixes.patch
                     0002-tg2sip-ntgcalls-missing-includes.patch
                     0003-tg2sip-ntgcalls-fix-audio-incoming-enable-check.patch)
    set(patch_path ${CMAKE_SOURCE_DIR}/patches/${patch_file})
    execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${patch_path}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/ntgcalls
            RESULT_VARIABLE ntg_patch_already_applied
            OUTPUT_QUIET ERROR_QUIET)
    if (NOT ntg_patch_already_applied EQUAL 0)
        message(STATUS "Applying ntgcalls patch: ${patch_file}")
        execute_process(
                COMMAND ${GIT_EXECUTABLE} apply ${patch_path}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/ntgcalls
                RESULT_VARIABLE ntg_patch_apply_result)
        if (NOT ntg_patch_apply_result EQUAL 0)
            message(FATAL_ERROR "Failed to apply ${patch_file} - the "
                                "third_party/ntgcalls submodule checkout may "
                                "have diverged from what this patch expects")
        endif ()
    endif ()
endforeach ()

set(IS_PYTHON OFF CACHE BOOL "" FORCE)
set(STATIC_BUILD ON CACHE BOOL "" FORCE)
# USE_LIBCXX intentionally left at its default ON - see the big comment
# above: ntgcalls' own code must match its prebuilt WebRTC's Chromium-libc++
# ABI, not this project's system-libc++ toolchain.
add_subdirectory(third_party/ntgcalls third_party_ntgcalls EXCLUDE_FROM_ALL)

set(NTGCALLS_ROOT ${CMAKE_SOURCE_DIR}/third_party/ntgcalls)

# The add_subdirectory() call above already fetched ntgcalls' prebuilt
# libwebrtc.a (its own configure-time GitClone/download step) - patch it in
# place now, before anything links against it, AND force ntgcalls' own
# source to use the same renamed symbols wherever it calls BoringSSL's raw
# crypto API directly. See buildenv/rename-webrtc-openssl-symbols.sh for
# the full why (three different real crashes on the way to this fix, not a
# theoretical risk) - both halves (the archive patch AND the force-include
# header below) are required together; either alone reintroduces the same
# class of bug in a different place. Idempotent - safe to run on every
# configure, including ones where the archive was already patched by a
# previous run (the header is cheap and always regenerated).
set(NTGCALLS_PREBUILT_WEBRTC ${NTGCALLS_ROOT}/deps/libwebrtc/src/lib/libwebrtc.a)
set(NTGCALLS_OPENSSL_RENAME_HEADER ${CMAKE_BINARY_DIR}/ntgcalls_openssl_renames.h)
if (EXISTS ${NTGCALLS_PREBUILT_WEBRTC})
    execute_process(
            COMMAND ${CMAKE_SOURCE_DIR}/buildenv/rename-webrtc-openssl-symbols.sh ${NTGCALLS_PREBUILT_WEBRTC} ${NTGCALLS_OPENSSL_RENAME_HEADER}
            RESULT_VARIABLE ntg_rename_result)
    if (NOT ntg_rename_result EQUAL 0)
        message(FATAL_ERROR "Failed to rename colliding OpenSSL symbols in ${NTGCALLS_PREBUILT_WEBRTC}")
    endif ()
else ()
    message(FATAL_ERROR "${NTGCALLS_PREBUILT_WEBRTC} not found after add_subdirectory(third_party/ntgcalls) - "
                        "expected ntgcalls' own CMake to have fetched it by now")
endif ()

# ntgcalls' own source (wrtc/src/utils/encryption.cpp, random.cpp - the P2P
# call's own key-derivation/AES-CTR encryption path) calls BoringSSL's raw
# crypto functions directly, expecting BoringSSL's own struct layouts -
# force this header first into every translation unit of just these two
# targets so those calls (and any indirect/header-inline use the same
# grep-for-direct-callers approach might miss) consistently resolve to the
# archive's newly-renamed symbols instead of colliding with TDLib's/PJSIP's
# real OpenSSL. Scoped to wrtc/ntgcalls-native only, not tg2sip-webrtc's
# own sources - they don't call these functions at all.
if (TARGET wrtc)
    target_compile_options(wrtc PRIVATE -include ${NTGCALLS_OPENSSL_RENAME_HEADER})
endif ()
if (TARGET ntgcalls-native)
    target_compile_options(ntgcalls-native PRIVATE -include ${NTGCALLS_OPENSSL_RENAME_HEADER})
endif ()

# Same bug, one library over: libwebrtc.a also bundles its own libsrtp
# (2.0.0), which shares ~140 plain C symbol names with PJSIP's own bundled
# libsrtp (2.1.0, found via pkg-config's -L dir for libpjproject above).
# 2.1.0 changed srtp_policy_t's layout (added MKI support) versus 2.0.0, so
# whichever definition the linker keeps for the whole binary silently breaks
# whichever side didn't win - confirmed real: WebRTC's own SrtpSession ends
# up calling PJSIP's srtp_create() with a struct shaped for 2.0.0, which
# 2.1.0 rejects as srtp_err_status_bad_param right after a real call's DTLS
# handshake completes. See buildenv/rename-webrtc-libsrtp-symbols.sh. No
# force-include header needed here (unlike the OpenSSL case above) -
# ntgcalls' own source never calls libsrtp directly, only libwebrtc.a does
# internally.
file(GLOB PJSIP_LIBSRTP_ARCHIVE ${PJSIP_LIBRARY_DIRS}/libsrtp*.a)
if (NOT PJSIP_LIBSRTP_ARCHIVE)
    message(FATAL_ERROR "No libsrtp*.a found in ${PJSIP_LIBRARY_DIRS} - "
                        "expected PJSIP's own bundled libsrtp there")
endif ()
execute_process(
        COMMAND ${CMAKE_SOURCE_DIR}/buildenv/rename-webrtc-libsrtp-symbols.sh ${NTGCALLS_PREBUILT_WEBRTC} ${PJSIP_LIBSRTP_ARCHIVE}
        RESULT_VARIABLE ntg_srtp_rename_result)
if (NOT ntg_srtp_rename_result EQUAL 0)
    message(FATAL_ERROR "Failed to rename colliding libsrtp symbols in ${NTGCALLS_PREBUILT_WEBRTC}")
endif ()

# Extra glibc resolver stubs: the prebuilt libgio-2.0.a (an ntgcalls
# transitive dep) references legacy BIND resolver symbols
# (__dn_expand/__res_nquery) that modern glibc no longer exports under
# those names - dead code for ntgcalls' actual P2P-call usage (DNS TXT/SRV
# lookup, never exercised), stubbed out rather than chasing an older
# libresolv. See buildenv/ntgcalls_resolv_stubs.c.
set(NTGCALLS_LINK_LIBRARIES
        ntgcalls-native wrtc
        WebRTC::webrtc Boost::json
        gnome::gio-2.0 gnome::gmodule-2.0 gnome::glib-2.0 gnu::pcre2 gnome::gobject-2.0 gnu::libffi
        mesa::gbm gnu::expat mesa::drm
        xorg::X11 xorg::xcb xorg::Xau xorg::Xcomposite xorg::Xdamage xorg::Xext xorg::Xfixes xorg::Xrandr xorg::Xrender xorg::Xtst
        ffmpeg::avcodec ffmpeg::avformat ffmpeg::avutil mesa::vdpau intel::va intel::va-drm ffmpeg::swresample
        cisco::OpenH264
        Boost::process Boost::atomic Boost::context Boost::date_time Boost::filesystem
        resolv)

add_executable(tg2sip-webrtc
        tg2sip/main.cpp
        tg2sip/tg.cpp
        tg2sip/tg.h
        tg2sip/sip.cpp
        tg2sip/sip.h
        tg2sip/settings.cpp
        tg2sip/settings.h
        tg2sip/logging.cpp
        tg2sip/logging.h
        tg2sip/utils.cpp
        tg2sip/utils.h
        tg2sip/queue.h
        tg2sip/gateway.cpp
        tg2sip/gateway.h
        tg2sip/voip/controller.cpp
        tg2sip/voip/controller.h
        tg2sip/voip/audio_bridge.cpp
        tg2sip/voip/audio_bridge.h
        tg2sip/voip/ring_buffer.h
        tg2sip/voip/ntgcalls_runtime.cpp
        tg2sip/voip/ntgcalls_runtime.h
        buildenv/ntgcalls_resolv_stubs.c
        )

add_custom_command(
        TARGET tg2sip-webrtc PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/tg2sip.conf
        $<TARGET_FILE_DIR:tg2sip-webrtc>/tg2sip.conf.sample)

target_include_directories(tg2sip-webrtc PRIVATE
        ${PJSIP_INCLUDE_DIRS}
        ${PROJECT_SOURCE_DIR}
        ${OPUS_INCLUDE_DIRS}
        ${FFMPEG_INCLUDE_DIRS}
        ${NTGCALLS_ROOT}/include
        include)

target_link_directories(tg2sip-webrtc PRIVATE
        ${PJSIP_LIBRARY_DIRS})

# Td::TdStatic listed before ${NTGCALLS_LINK_LIBRARIES}: harmless/no longer
# load-bearing on its own (CMake's own link-line generation reorders things
# based on the target dependency graph regardless of textual position here,
# confirmed empirically), but kept as the natural reading order since
# PJSIP/TDLib are this project's own real dependencies and ntgcalls is
# further down the chain. The actual fix for the BoringSSL/OpenSSL symbol
# collision this comment used to describe is
# buildenv/rename-webrtc-openssl-symbols.sh, applied right after
# add_subdirectory(third_party/ntgcalls) above - link order alone cannot
# fix this class of bug (ELF's default global symbol scope lets the main
# executable's own symbol table interpose shared libraries' internal calls
# too, e.g. PJSIP's libsrtp calling into BoringSSL's HMAC_Init_ex instead
# of its own linked libcrypto.so.3, regardless of where anything sits on
# this link line) - only removing BoringSSL's conflicting symbols'
# global visibility does.
target_link_libraries(tg2sip-webrtc PRIVATE
        ${PJSIP_LIBRARIES}
        Td::TdStatic
        ${OPUS_LIBRARIES}
        ${NTGCALLS_LINK_LIBRARIES}
        ${FFMPEG_LIBRARIES}
        spdlog::spdlog
        Threads::Threads
        ${CMAKE_DL_LIBS})

add_executable(tg2sip-gendb
        tg2sip/gen_db.cpp
        tg2sip/settings.cpp
        tg2sip/settings.h
        tg2sip/utils.cpp
        tg2sip/utils.h)

target_include_directories(tg2sip-gendb PRIVATE
        include)

target_link_libraries(tg2sip-gendb PRIVATE
        Td::TdStatic)

install(TARGETS tg2sip-webrtc tg2sip-gendb RUNTIME DESTINATION bin)