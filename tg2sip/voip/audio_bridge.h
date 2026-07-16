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

#ifndef TG2SIP_VOIP_AUDIO_BRIDGE_H
#define TG2SIP_VOIP_AUDIO_BRIDGE_H

#include <pjsua2.hpp>

#include "tgcalls/FakeAudioDeviceModule.h"
#include "ring_buffer.h"

namespace voip {

    // PJSIP-facing audio ports. Same registration pattern as the old
    // libtgvoip/audio/SoftwareAudioInput|Output (a pjmedia_port wrapped as a
    // pj::AudioMedia via registerMediaPort()), but reading/writing a
    // RingBuffer instead of a libtgvoip callback. Both run at 10ms/480
    // samples @ 48kHz mono - matches tgcalls' FakeAudioDeviceModule frame
    // size exactly, so neither side needs to buffer partial frames.

    // SIP -> tgcalls direction: PJSIP pushes RTP audio in, we buffer it for
    // TgCallsRecorder to hand to tgcalls as "microphone" input.
    class SoftwareAudioInput : public pj::AudioMedia {
    public:
        explicit SoftwareAudioInput(RingBuffer &mic_buffer);

        ~SoftwareAudioInput() override;

        void Start();

        void Stop();

    private:
        static pj_status_t PutFrameCallback(pjmedia_port *port, pjmedia_frame *frame);

        static pj_status_t GetFrameCallback(pjmedia_port *port, pjmedia_frame *frame);

        RingBuffer &mic_buffer_;
        bool active_{false};

        pj_pool_t *pj_pool_;
        pjmedia_port *media_port_;
    };

    // tgcalls -> SIP direction: TgCallsRenderer buffers decoded Telegram
    // audio here, PJSIP pulls it out to send over RTP.
    class SoftwareAudioOutput : public pj::AudioMedia {
    public:
        explicit SoftwareAudioOutput(RingBuffer &playout_buffer);

        ~SoftwareAudioOutput() override;

        void Start();

        void Stop();

    private:
        static pj_status_t PutFrameCallback(pjmedia_port *port, pjmedia_frame *frame);

        static pj_status_t GetFrameCallback(pjmedia_port *port, pjmedia_frame *frame);

        RingBuffer &playout_buffer_;
        bool active_{false};

        pj_pool_t *pj_pool_;
        pjmedia_port *media_port_;
    };

    // tgcalls::FakeAudioDeviceModule::Renderer/Recorder are the hook tgcalls
    // uses instead of a real webrtc::AudioDeviceModule - see
    // Descriptor::createAudioDeviceModule in controller.cpp.

    class TgCallsRenderer : public tgcalls::FakeAudioDeviceModule::Renderer {
    public:
        explicit TgCallsRenderer(RingBuffer &playout_buffer) : playout_buffer_(playout_buffer) {}

        bool Render(const tgcalls::AudioFrame &frame) override;

    private:
        RingBuffer &playout_buffer_;
    };

    class TgCallsRecorder : public tgcalls::FakeAudioDeviceModule::Recorder {
    public:
        explicit TgCallsRecorder(RingBuffer &mic_buffer) : mic_buffer_(mic_buffer) {}

        tgcalls::AudioFrame Record() override;

    private:
        RingBuffer &mic_buffer_;
        int16_t scratch_[480];
    };

}

#endif //TG2SIP_VOIP_AUDIO_BRIDGE_H
