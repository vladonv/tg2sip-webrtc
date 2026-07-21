/*
 * Copyright (C) 2026 Vlad Vorobyev (vlad.vorobev@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <spdlog/spdlog.h>

#include "controller.h"

using namespace voip;

namespace {

    // ntgcalls only implements the V2 protocol (see
    // signaling::Signaling::SupportedVersions() in ntgcalls' own source) -
    // unlike the old tgcalls backend there is no legacy fallback to
    // negotiate down to, so tg2sip always advertises exactly these two.
    const std::vector<std::string> &SupportedVersions() {
        static const std::vector<std::string> versions{"8.0.0", "9.0.0"};
        return versions;
    }

    ntg_protocol_struct FetchProtocol() {
        ntg_protocol_struct protocol{};
        ntg_get_protocol(&protocol);
        return protocol;
    }

}

TgCallsController::TgCallsController(const int64_t user_id,
                                     std::array<uint8_t, 256> encryption_key,
                                     const bool is_outgoing,
                                     std::vector<RtcServer> rtc_servers,
                                     const bool enable_p2p,
                                     std::function<void(const std::vector<uint8_t> &)> on_signaling_data)
        : user_id_(user_id),
          encryption_key_(encryption_key),
          is_outgoing_(is_outgoing),
          rtc_servers_(std::move(rtc_servers)),
          enable_p2p_(enable_p2p),
          on_signaling_data_(std::move(on_signaling_data)) {
    sink_.on_signaling_data = [this](const uint8_t *data, const int size) { OnSignalingData(data, size); };
    sink_.on_frames = [this](const ntg_stream_mode_enum mode, const ntg_stream_device_enum device,
                             ntg_frame_struct *frames, const uint64_t count) {
        OnFrames(mode, device, frames, count);
    };
}

TgCallsController::~TgCallsController() {
    if (created_) {
        Stop();
    }
}

void TgCallsController::Start() {
    auto &runtime = NtgCallsRuntime::Instance();
    const auto ptr = runtime.ptr();

    runtime.CallBlocking([&](const ntg_async_struct future) {
        return ntg_create_p2p(ptr, user_id_, future);
    });
    created_ = true;

    ntg_audio_description_struct audio_desc{};
    audio_desc.mediaSource = NTG_EXTERNAL;
    audio_desc.input = const_cast<char *>("");
    audio_desc.sampleRate = SAMPLE_RATE;
    audio_desc.channelCount = CHANNEL_COUNT;
    audio_desc.keepOpen = false;

    ntg_media_description_struct capture_desc{};
    capture_desc.microphone = &audio_desc;
    runtime.CallBlocking([&](const ntg_async_struct future) {
        return ntg_set_stream_sources(ptr, user_id_, NTG_STREAM_CAPTURE, capture_desc, future);
    });

    ntg_media_description_struct playback_desc{};
    playback_desc.speaker = &audio_desc;
    runtime.CallBlocking([&](const ntg_async_struct future) {
        return ntg_set_stream_sources(ptr, user_id_, NTG_STREAM_PLAYBACK, playback_desc, future);
    });

    runtime.CallBlocking([&](const ntg_async_struct future) {
        return ntg_skip_exchange(ptr, user_id_, encryption_key_.data(),
                                 static_cast<int>(encryption_key_.size()), is_outgoing_, future);
    });

    // ntg_rtc_server_struct only holds raw pointers - keep the owning
    // strings/vectors (rtc_servers_) alive for the duration of this call,
    // build the C structs right here rather than storing them.
    std::vector<ntg_rtc_server_struct> c_servers(rtc_servers_.size());
    for (size_t i = 0; i < rtc_servers_.size(); ++i) {
        const auto &server = rtc_servers_[i];
        auto &c_server = c_servers[i];
        c_server.id = server.id;
        c_server.ipv4 = const_cast<char *>(server.ipv4.c_str());
        c_server.ipv6 = const_cast<char *>(server.ipv6.c_str());
        c_server.username = const_cast<char *>(server.username.c_str());
        c_server.password = const_cast<char *>(server.password.c_str());
        c_server.port = server.port;
        c_server.turn = server.turn;
        c_server.stun = server.stun;
        c_server.tcp = server.tcp;
        c_server.peerTag = server.peer_tag.empty() ? nullptr : const_cast<uint8_t *>(server.peer_tag.data());
        c_server.peerTagSize = static_cast<int>(server.peer_tag.size());
    }

    std::vector<std::string> version_strings = SupportedVersions();
    std::vector<char *> c_versions(version_strings.size());
    for (size_t i = 0; i < version_strings.size(); ++i) {
        c_versions[i] = const_cast<char *>(version_strings[i].c_str());
    }

    runtime.CallBlocking([&](const ntg_async_struct future) {
        return ntg_connect_p2p(ptr, user_id_, c_servers.data(), static_cast<int>(c_servers.size()),
                               c_versions.data(), static_cast<int>(c_versions.size()), enable_p2p_, future);
    });

    runtime.Register(user_id_, &sink_);
    registered_ = true;

    audio_input_.Start();
    audio_output_.Start();

    mic_feeder_running_ = true;
    mic_feeder_thread_ = std::thread(&TgCallsController::MicFeederLoop, this);
}

void TgCallsController::Connect() {
    // ntgcalls starts connecting as part of ntg_connect_p2p in Start() -
    // there is no separate connect phase.
}

void TgCallsController::Stop() {
    if (mic_feeder_running_.exchange(false)) {
        mic_feeder_thread_.join();
    }

    audio_input_.Stop();
    audio_output_.Stop();

    if (registered_) {
        // Unregister before ntg_stop - blocks until any in-flight
        // OnSignalingData/OnFrames dispatch to this call has returned, so
        // ntgcalls can never call back into this (about to be torn down)
        // controller once ntg_stop is issued. Same use-after-free class the
        // old tgcalls backend once had (git log: 50c8886) - this ordering
        // is what avoids it here.
        NtgCallsRuntime::Instance().Unregister(user_id_);
        registered_ = false;
    }

    if (created_) {
        NtgCallsRuntime::Instance().CallBlocking([&](const ntg_async_struct future) {
            return ntg_stop(NtgCallsRuntime::Instance().ptr(), user_id_, future);
        });
        created_ = false;
    }
}

void TgCallsController::UpdateSignaling(const std::vector<uint8_t> &data) {
    if (!created_) {
        return;
    }
    // Copy: ntg_send_signaling_data doesn't take ownership, but this call
    // is detached (fire-and-forget) and returns before `data` - a
    // reference into the caller's event - is guaranteed to still be alive.
    auto buffer = std::make_shared<std::vector<uint8_t>>(data);
    NtgCallsRuntime::Instance().CallDetached([&, buffer](const ntg_async_struct future) {
        return ntg_send_signaling_data(NtgCallsRuntime::Instance().ptr(), user_id_,
                                       buffer->data(), static_cast<int>(buffer->size()), future);
    });
}

void TgCallsController::OnSignalingData(const uint8_t *data, const int size) {
    if (on_signaling_data_) {
        on_signaling_data_(std::vector<uint8_t>(data, data + size));
    }
}

void TgCallsController::OnFrames(const ntg_stream_mode_enum mode, const ntg_stream_device_enum device,
                                 ntg_frame_struct *frames, const uint64_t count) {
    if (mode != NTG_STREAM_PLAYBACK || device != NTG_STREAM_SPEAKER) {
        return;
    }
    for (uint64_t i = 0; i < count; ++i) {
        const auto &frame = frames[i];
        const auto *samples = reinterpret_cast<const int16_t *>(frame.data);
        const size_t sample_count = frame.sizeData / sizeof(int16_t);
        playout_buffer_->push(samples, sample_count);

        int32_t local_max = max_abs_since_start_.load(std::memory_order_relaxed);
        for (size_t s = 0; s < sample_count; ++s) {
            local_max = std::max(local_max, static_cast<int32_t>(std::abs(samples[s])));
        }
        max_abs_since_start_.store(local_max, std::memory_order_relaxed);
        const auto n = frames_received_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n % 100 == 0) {
            spdlog::get("core")->info(
                    "[diag] TgCallsController::OnFrames calls={} sample_count={} max_abs_since_start={}",
                    n, sample_count, max_abs_since_start_.load(std::memory_order_relaxed));
        }
    }
}

void TgCallsController::MicFeederLoop() {
    int16_t scratch[FRAME_SAMPLES];
    auto next_deadline = std::chrono::steady_clock::now();

    while (mic_feeder_running_.load(std::memory_order_relaxed)) {
        next_deadline += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next_deadline);

        mic_buffer_->pop(scratch, FRAME_SAMPLES);

        auto frame = std::make_shared<std::array<uint8_t, FRAME_BYTES>>();
        std::memcpy(frame->data(), scratch, FRAME_BYTES);

        NtgCallsRuntime::Instance().CallDetached([&, frame](const ntg_async_struct future) {
            ntg_frame_data_struct frame_data{};
            return ntg_send_external_frame(NtgCallsRuntime::Instance().ptr(), user_id_, NTG_STREAM_MICROPHONE,
                                           frame->data(), static_cast<int>(frame->size()), frame_data, future);
        });
    }
}

int32_t TgCallsController::GetConnectionMaxLayer() {
    static const int32_t max_layer = [] {
        const auto protocol = FetchProtocol();
        // ntg_get_protocol's libraryVersions is allocated with C++ `new`/
        // `new[]` (see copyAndReturn(std::string, char**) and
        // copyAndReturn(const std::vector<std::string>&) in ntgcalls'
        // bindings/ntgcalls.cpp) - must be released with delete[], not
        // free() (unlike ntg_async_struct's errorMessage, which is
        // strdup()'d and does need free()).
        for (int i = 0; i < protocol.libraryVersionsSize; ++i) {
            delete[] protocol.libraryVersions[i];
        }
        delete[] protocol.libraryVersions;
        return protocol.maxLayer;
    }();
    return max_layer;
}

int32_t TgCallsController::GetConnectionMinLayer() {
    static const int32_t min_layer = [] {
        const auto protocol = FetchProtocol();
        // ntg_get_protocol's libraryVersions is allocated with C++ `new`/
        // `new[]` (see copyAndReturn(std::string, char**) and
        // copyAndReturn(const std::vector<std::string>&) in ntgcalls'
        // bindings/ntgcalls.cpp) - must be released with delete[], not
        // free() (unlike ntg_async_struct's errorMessage, which is
        // strdup()'d and does need free()).
        for (int i = 0; i < protocol.libraryVersionsSize; ++i) {
            delete[] protocol.libraryVersions[i];
        }
        delete[] protocol.libraryVersions;
        return protocol.minLayer;
    }();
    return min_layer;
}

std::vector<std::string> TgCallsController::Versions() {
    return SupportedVersions();
}
