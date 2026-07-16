cmake_minimum_required(VERSION 3.14)
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

# ---- tgcalls (WebRTC calling engine, replaces libtgvoip) ----
# Vendored as the third_party/tgcalls git submodule (github.com/MarshalX/tgcalls)
# rather than committed in-tree: tgcalls' own core is LGPLv3, and combining
# LGPLv3 code into this GPLv2-or-later project only stays license-clean if
# the combined work is distributed under GPLv3 - a submodule keeps that
# licensing boundary explicit instead of blurring it by vendoring the
# source directly alongside tg2sip's own GPLv2 files.
set(TGCALLS_ROOT ${CMAKE_SOURCE_DIR}/third_party/tgcalls/tgcalls)
set(TGCALLS_CMAKE_HELPERS ${CMAKE_SOURCE_DIR}/third_party/tgcalls/cmake)

if (NOT EXISTS ${TGCALLS_ROOT}/third_party/webrtc/CMakeLists.txt)
    message(FATAL_ERROR "third_party/tgcalls submodule not initialized - run: "
                        "git submodule update --init --recursive third_party/tgcalls")
endif()

# The submodule needs two small local patches to build against current
# distro packages (FFmpeg 7.x removed the AVFrame/AVCodecContext fields the
# vendored H264 decoder/encoder use) - see patches/README.md. Applied here,
# idempotently, rather than committed into the submodule: patching a git
# submodule's checkout in place doesn't survive `git submodule update` or a
# fresh clone, and this project has no fork of MarshalX/tgcalls to commit
# the patch into instead.
find_package(Git REQUIRED)
foreach (patch_file 0001-tg2sip-drop-h264-ffmpeg7-incompatible.patch)
    set(patch_path ${CMAKE_SOURCE_DIR}/patches/${patch_file})
    execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${patch_path}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/tgcalls
            RESULT_VARIABLE patch_already_applied
            OUTPUT_QUIET ERROR_QUIET)
    if (NOT patch_already_applied EQUAL 0)
        message(STATUS "Applying tgcalls patch: ${patch_file}")
        execute_process(
                COMMAND ${GIT_EXECUTABLE} apply ${patch_path}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/tgcalls
                RESULT_VARIABLE patch_apply_result)
        if (NOT patch_apply_result EQUAL 0)
            message(FATAL_ERROR "Failed to apply ${patch_file} - the "
                                "third_party/tgcalls submodule checkout may "
                                "have diverged from what this patch expects")
        endif()
    endif()
endforeach ()

# tgcalls/tg_owt predate GCC 14's stricter transitive-include behavior in
# libstdc++ (several files use uint8_t etc. without directly including
# <cstdint>) - confirmed during the build spike for this migration.
# CXX-only: <cstdint> isn't a valid C header, and this tree also has plain
# C sources (pffft, openh264, ...) that must not see this flag.
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-include>" "$<$<COMPILE_LANGUAGE:CXX>:cstdint>")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# Screen sharing is irrelevant to an audio-only SIP gateway, and tg_owt's
# dlopen-based PipeWire stub mode leaves a symbol undefined at final link
# time (webrtc::BaseCapturerPipeWire::CreateRawCapturer) unless disabled.
set(TG_OWT_USE_PIPEWIRE OFF CACHE BOOL "" FORCE)
set(DESKTOP_APP_USE_PACKAGED ON)

# EXCLUDE_FROM_ALL suppresses tg_owt's own install() rules (it's normally a
# standalone installable library - without this, `cmake --install` ships its
# full public header tree plus a duplicate libtg_owt.a under /usr/include and
# /usr/lib, ~40MB of dead weight nobody links against post-install since it's
# already folded into the tg2sip-webrtc binary via --whole-archive at build
# time). tg_owt itself still gets built normally since tg2sip-webrtc depends
# on it - EXCLUDE_FROM_ALL only opts it out of the default all/install sets.
add_subdirectory(${TGCALLS_ROOT}/third_party/webrtc third_party_tg_owt EXCLUDE_FROM_ALL)

add_library(external_webrtc INTERFACE)
target_link_libraries(external_webrtc INTERFACE tg_owt)
target_include_directories(external_webrtc INTERFACE ${TGCALLS_ROOT}/third_party/webrtc/src)

include(${TGCALLS_CMAKE_HELPERS}/nice_target_sources.cmake)
include(${TGCALLS_CMAKE_HELPERS}/init_target.cmake)
include(${TGCALLS_CMAKE_HELPERS}/target_link_static_libraries.cmake)

add_library(common_options INTERFACE)
add_library(desktop-app::common_options ALIAS common_options)
include(${TGCALLS_CMAKE_HELPERS}/options_linux.cmake)

include(${TGCALLS_ROOT}/cmake/external_ffmpeg.cmake)

get_filename_component(third_party_loc "${TGCALLS_ROOT}/third_party" REALPATH)
include(${TGCALLS_ROOT}/cmake/lib_tgcalls.cmake)

# 1:1 calls only (tg2sip has no group/SFU support) - also doesn't compile
# against FFmpeg 7.x (AVFrame::channels was removed upstream).
remove_target_sources(lib_tgcalls ${tgcalls_loc}
        group/GroupInstanceCustomImpl.cpp
        group/GroupInstanceCustomImpl.h
        group/GroupInstanceImpl.h
        group/GroupJoinPayloadInternal.cpp
        group/GroupJoinPayloadInternal.h
        group/GroupJoinPayload.h
        group/GroupNetworkManager.cpp
        group/GroupNetworkManager.h
        group/StreamingPart.cpp
        group/StreamingPart.h
        )

# tgcalls' Meta/Instance version registry is populated by whole-archive
# linking lib_tgcalls (InstanceImpl.cpp's Register<InstanceImpl>()
# specialization is otherwise never pulled from the static archive) - see
# voip/controller.cpp's EnsureRegistered() for the runtime-side half of this.
set(TGCALLS_LINK_LIBRARIES
        -Wl,--whole-archive lib_tgcalls -Wl,--no-whole-archive
        tg_owt
        ${OPUS_LIBRARIES}
        jpeg
        ssl
        crypto
        vpx
        event
        X11
        Xext
        Xdamage
        Xfixes
        Xrender
        Xrandr
        Xtst
        Xcomposite
        asound
        pulse)

pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale libswresample)
pkg_check_modules(ABSL REQUIRED
        absl_strings absl_str_format_internal absl_time absl_base
        absl_synchronization absl_flags_internal absl_hash absl_status
        absl_statusor absl_bad_optional_access absl_bad_variant_access
        absl_raw_hash_set absl_city absl_low_level_hash)

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
        )

add_custom_command(
        TARGET tg2sip-webrtc PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/settings.ini
        $<TARGET_FILE_DIR:tg2sip-webrtc>/config.ini.sample)

target_include_directories(tg2sip-webrtc PRIVATE
        ${PJSIP_INCLUDE_DIRS}
        ${PROJECT_SOURCE_DIR}
        ${OPUS_INCLUDE_DIRS}
        ${FFMPEG_INCLUDE_DIRS}
        ${TGCALLS_ROOT}
        include)

target_link_directories(tg2sip-webrtc PRIVATE
        ${PJSIP_LIBRARY_DIRS})

target_link_libraries(tg2sip-webrtc PRIVATE
        ${PJSIP_LIBRARIES}
        ${TGCALLS_LINK_LIBRARIES}
        ${FFMPEG_LIBRARIES}
        ${ABSL_LIBRARIES}
        spdlog::spdlog
        Threads::Threads
        ${CMAKE_DL_LIBS}
        Td::TdStatic)

add_executable(gen_db
        tg2sip/gen_db.cpp
        tg2sip/settings.cpp
        tg2sip/settings.h
        tg2sip/utils.cpp
        tg2sip/utils.h)

target_include_directories(gen_db PRIVATE
        include)

target_link_libraries(gen_db PRIVATE
        Td::TdStatic)

install(TARGETS tg2sip-webrtc gen_db RUNTIME DESTINATION bin)