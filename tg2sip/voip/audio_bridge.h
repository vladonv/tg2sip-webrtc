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

#include <memory>

#include <pjsua2.hpp>

#include "ring_buffer.h"

namespace voip {

    // PJSIP-facing audio ports. Same registration pattern as the old
    // libtgvoip/audio/SoftwareAudioInput|Output (a pjmedia_port wrapped as a
    // pj::AudioMedia via registerMediaPort()), but reading/writing a
    // RingBuffer instead of a libtgvoip callback. Both run at 10ms/480
    // samples @ 48kHz mono - matches the calling backend's own frame size
    // exactly, so neither side needs to buffer partial frames.
    //
    // The calling-backend-facing glue (TgCallsRenderer/TgCallsRecorder,
    // tgcalls::FakeAudioDeviceModule::Renderer/Recorder subclasses) that
    // used to live in this file was removed with the ntgcalls migration -
    // ntgcalls' C API has no equivalent virtual-interface hook, just a
    // plain onFrames callback (TgCallsController::OnFrames) and a push-style
    // send call (TgCallsController::MicFeederLoop) - see controller.cpp.

    // SIP -> tgcalls direction: PJSIP pushes RTP audio in, we buffer it for
    // TgCallsRecorder to hand to tgcalls as "microphone" input.
    class SoftwareAudioInput : public pj::AudioMedia {
    public:
        explicit SoftwareAudioInput(std::shared_ptr<RingBuffer> mic_buffer);

        ~SoftwareAudioInput() override;

        void Start();

        void Stop();

    private:
        static pj_status_t PutFrameCallback(pjmedia_port *port, pjmedia_frame *frame);

        static pj_status_t GetFrameCallback(pjmedia_port *port, pjmedia_frame *frame);

        std::shared_ptr<RingBuffer> mic_buffer_;
        bool active_{false};

        pj_pool_t *pj_pool_;
        pjmedia_port *media_port_;
    };

    // tgcalls -> SIP direction: TgCallsRenderer buffers decoded Telegram
    // audio here, PJSIP pulls it out to send over RTP.
    class SoftwareAudioOutput : public pj::AudioMedia {
    public:
        explicit SoftwareAudioOutput(std::shared_ptr<RingBuffer> playout_buffer);

        ~SoftwareAudioOutput() override;

        void Start();

        void Stop();

    private:
        static pj_status_t PutFrameCallback(pjmedia_port *port, pjmedia_frame *frame);

        static pj_status_t GetFrameCallback(pjmedia_port *port, pjmedia_frame *frame);

        std::shared_ptr<RingBuffer> playout_buffer_;
        bool active_{false};

        pj_pool_t *pj_pool_;
        pjmedia_port *media_port_;
    };

}

#endif //TG2SIP_VOIP_AUDIO_BRIDGE_H
