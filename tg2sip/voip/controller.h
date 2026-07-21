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

#ifndef TG2SIP_VOIP_CONTROLLER_H
#define TG2SIP_VOIP_CONTROLLER_H

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <ntgcalls.h>

#include "audio_bridge.h"
#include "ntgcalls_runtime.h"
#include "ring_buffer.h"

namespace voip {

    // A single ntgcalls RTCServer entry - the plain-data tg2sip-side mirror
    // of ntg_rtc_server_struct (which uses raw char*/uint8_t* pointers not
    // safe to hold onto past one call). gateway.cpp builds these from
    // td_api::callStateReady's server list; controller.cpp converts them to
    // ntg_rtc_server_struct right before ntg_connect_p2p, since those
    // pointers only need to be valid for the duration of that one call.
    struct RtcServer {
        uint64_t id{};
        std::string ipv4;
        std::string ipv6;
        std::string username;
        std::string password;
        uint16_t port{};
        bool turn{};
        bool stun{};
        bool tcp{};
        std::vector<uint8_t> peer_tag;
    };

    // Replaces tgvoip::VoIPController, now backed by ntgcalls instead of
    // tgcalls - see the ntgcalls migration plan
    // (~/.claude/plans/jaunty-roaming-cat.md) for the full design. Owns one
    // P2P call end-to-end against the single process-wide NtgCallsRuntime,
    // and wires its audio to a pair of pj::AudioMedia-backed PJSIP ports via
    // a ring buffer on each direction - the same shape gateway.cpp already
    // calls today, so the state machine's action bodies barely change.
    //
    // Unlike the old tgcalls backend, ntgcalls has no AEC/NS/AGC toggles
    // anywhere in its API (grepped the full C and C++ surface) - it always
    // runs WebRTC's standard audio processing pipeline. The enable_aec/ns/agc
    // settings this project still reads from tg2sip.conf are inert with this
    // backend; kept in Settings for the still-present old tgcalls path.
    class TgCallsController {
    public:
        TgCallsController(int64_t user_id,
                          std::array<uint8_t, 256> encryption_key,
                          bool is_outgoing,
                          std::vector<RtcServer> rtc_servers,
                          bool enable_p2p,
                          std::function<void(const std::vector<uint8_t> &)> on_signaling_data);

        TgCallsController(const TgCallsController &) = delete;

        TgCallsController &operator=(const TgCallsController &) = delete;

        ~TgCallsController();

        // Runs the whole call-setup sequence against ntgcalls
        // (ntg_create_p2p -> ntg_set_stream_sources x2 -> ntg_skip_exchange
        // -> ntg_connect_p2p), blocking until each step completes, then
        // registers with NtgCallsRuntime and starts the audio ports and the
        // mic-feed thread.
        void Start();

        // No-op: ntgcalls starts connecting as part of Start()'s
        // ntg_connect_p2p call, same as the old tgcalls backend had no
        // separate connect phase either. Kept so gateway.cpp's existing
        // Start()+Connect() call pair doesn't need to change shape.
        void Connect();

        // Blocks until ntgcalls has fully torn the call down.
        void Stop();

        // Inbound signaling data from TDLib's updateNewCallSignalingData.
        void UpdateSignaling(const std::vector<uint8_t> &data);

        pj::AudioMedia *AudioMediaInput() { return &audio_input_; }

        pj::AudioMedia *AudioMediaOutput() { return &audio_output_; }

        // Replaces tgvoip::VoIPController::GetConnectionMaxLayer().
        static int32_t GetConnectionMaxLayer();

        // Replaces gateway.cpp's separate CALL_PROTO_MIN_LAYER macro - with
        // ntgcalls only the V2 protocol is implemented, so min == max here,
        // unlike the old tgcalls backend which could fall back a long way.
        static int32_t GetConnectionMinLayer();

        // The actual protocol versions this ntgcalls build supports.
        static std::vector<std::string> Versions();

    private:
        static constexpr size_t RING_BUFFER_CAPACITY = 48000; // 1s @ 48kHz mono
        static constexpr uint32_t SAMPLE_RATE = 48000;
        static constexpr uint8_t CHANNEL_COUNT = 1;
        static constexpr size_t FRAME_SAMPLES = SAMPLE_RATE / 100; // 10ms
        static constexpr size_t FRAME_BYTES = FRAME_SAMPLES * sizeof(int16_t) * CHANNEL_COUNT;

        // Called by NtgCallsRuntime, dispatched to this call via user_id_.
        void OnSignalingData(const uint8_t *data, int size);

        void OnFrames(ntg_stream_mode_enum mode, ntg_stream_device_enum device,
                     ntg_frame_struct *frames, uint64_t count);

        // Wakes every 10ms, pops one frame from mic_buffer_, pushes it to
        // ntgcalls via a detached (fire-and-forget) ntg_send_external_frame
        // call - ntgcalls pulls nothing itself, unlike the old tgcalls
        // backend's Recorder callback, so this thread is new.
        void MicFeederLoop();

        int64_t user_id_;
        std::array<uint8_t, 256> encryption_key_;
        bool is_outgoing_;
        std::vector<RtcServer> rtc_servers_;
        bool enable_p2p_;
        std::function<void(const std::vector<uint8_t> &)> on_signaling_data_;

        std::shared_ptr<RingBuffer> mic_buffer_{std::make_shared<RingBuffer>(RING_BUFFER_CAPACITY)};
        std::shared_ptr<RingBuffer> playout_buffer_{std::make_shared<RingBuffer>(RING_BUFFER_CAPACITY)};

        SoftwareAudioInput audio_input_{mic_buffer_};
        SoftwareAudioOutput audio_output_{playout_buffer_};

        NtgCallsRuntime::Sink sink_;
        bool registered_{false};
        bool created_{false};

        std::atomic<bool> mic_feeder_running_{false};
        std::thread mic_feeder_thread_;

        // Diagnostic only (ported from the old TgCallsRenderer::Render) -
        // used to confirm real, nonzero audio energy during live-call
        // verification (see the plan's B6).
        std::atomic<int32_t> max_abs_since_start_{0};
        std::atomic<uint64_t> frames_received_{0};
    };

}

#endif //TG2SIP_VOIP_CONTROLLER_H
