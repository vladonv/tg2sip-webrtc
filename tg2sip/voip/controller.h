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
#include <functional>
#include <memory>
#include <vector>

#include "tgcalls/Instance.h"

#include "audio_bridge.h"
#include "ring_buffer.h"

namespace voip {

    // Replaces tgvoip::VoIPController. Owns one tgcalls call end-to-end:
    // builds a tgcalls::Descriptor from the pieces gateway.cpp already reads
    // out of td_api::callStateReady, wires tgcalls' audio callbacks to a
    // pair of pj::AudioMedia-backed PJSIP ports via a ring buffer on each
    // direction, and exposes the same Start()/Stop()/Connect()/
    // AudioMediaInput()/AudioMediaOutput()/GetConnectionMaxLayer() surface
    // gateway.cpp already calls today, so the state machine's action bodies
    // barely change shape.
    class TgCallsController {
    public:
        TgCallsController(std::array<uint8_t, 256> encryption_key,
                          bool is_outgoing,
                          std::vector<tgcalls::Endpoint> endpoints,
                          std::vector<tgcalls::RtcServer> rtc_servers,
                          bool enable_p2p,
                          bool enable_aec,
                          bool enable_ns,
                          bool enable_agc,
                          std::unique_ptr<tgcalls::Proxy> proxy,
                          std::function<void(const std::vector<uint8_t> &)> on_signaling_data);

        TgCallsController(const TgCallsController &) = delete;

        TgCallsController &operator=(const TgCallsController &) = delete;

        ~TgCallsController();

        // Builds the tgcalls::Descriptor and creates the Instance - this is
        // where the call actually starts connecting (tgcalls has no
        // separate connect step the way libtgvoip did: Meta::Create()
        // begins negotiating immediately).
        void Start();

        // No-op: kept only so gateway.cpp's existing Start()+Connect() call
        // pair (mirroring the old VoIPController usage) doesn't need to
        // change shape.
        void Connect();

        // Blocks until tgcalls has fully torn the instance down, matching
        // the synchronous semantics gateway.cpp's CleanUp{} action expects.
        void Stop();

        // Inbound signaling data from TDLib's updateNewCallSignalingData.
        void UpdateSignaling(const std::vector<uint8_t> &data);

        pj::AudioMedia *AudioMediaInput() { return &audio_input_; }

        pj::AudioMedia *AudioMediaOutput() { return &audio_output_; }

        // Replaces tgvoip::VoIPController::GetConnectionMaxLayer().
        static int32_t GetConnectionMaxLayer();

        // Replaces the hardcoded voip_library_versions() helper in
        // gateway.cpp - the actual protocol versions this tgcalls build
        // supports, straight from tgcalls::Meta::Versions().
        static std::vector<std::string> Versions();

    private:
        static constexpr size_t RING_BUFFER_CAPACITY = 48000; // 1s @ 48kHz mono

        RingBuffer mic_buffer_{RING_BUFFER_CAPACITY};
        RingBuffer playout_buffer_{RING_BUFFER_CAPACITY};

        SoftwareAudioInput audio_input_{mic_buffer_};
        SoftwareAudioOutput audio_output_{playout_buffer_};

        std::shared_ptr<TgCallsRenderer> renderer_{std::make_shared<TgCallsRenderer>(playout_buffer_)};
        std::shared_ptr<TgCallsRecorder> recorder_{std::make_shared<TgCallsRecorder>(mic_buffer_)};

        std::unique_ptr<tgcalls::Instance> instance_;

        std::array<uint8_t, 256> encryption_key_;
        bool is_outgoing_;
        std::vector<tgcalls::Endpoint> endpoints_;
        std::vector<tgcalls::RtcServer> rtc_servers_;
        bool enable_p2p_;
        bool enable_aec_;
        bool enable_ns_;
        bool enable_agc_;
        std::unique_ptr<tgcalls::Proxy> proxy_;
        std::function<void(const std::vector<uint8_t> &)> on_signaling_data_;
    };

}

#endif //TG2SIP_VOIP_CONTROLLER_H
