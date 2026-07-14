/*
 * Copyright (C) 2017-2018 infactum (infactum@gmail.com)
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

#include <cassert>
#include <algorithm>
#include <atomic>
#include <spdlog/spdlog.h>
#include "audio_bridge.h"

using namespace voip;

namespace {
    constexpr unsigned SAMPLES_PER_FRAME = 480; // 10ms @ 48kHz mono
}

// ---- SoftwareAudioInput (SIP -> tgcalls) ----

SoftwareAudioInput::SoftwareAudioInput(RingBuffer &mic_buffer) : mic_buffer_(mic_buffer) {
    pj_pool_ = pjsua_pool_create("voip-input%p", 2048, 512);
    media_port_ = PJ_POOL_ZALLOC_T(pj_pool_, pjmedia_port);

    pj_str_t name = pj_str((char *) "voip-input");
    pj_status_t status = pjmedia_port_info_init(&media_port_->info, &name,
                                                PJMEDIA_SIG_CLASS_PORT_AUD('T', 'I'), // tgcalls Input
                                                48000, 1, 16, SAMPLES_PER_FRAME);
    assert(status == PJ_SUCCESS);

    media_port_->port_data.pdata = this;
    media_port_->put_frame = &PutFrameCallback;
    media_port_->get_frame = &GetFrameCallback;

    registerMediaPort(media_port_);
}

SoftwareAudioInput::~SoftwareAudioInput() {
    unregisterMediaPort();
    pjmedia_port_destroy(media_port_);
    pj_pool_release(pj_pool_);
}

void SoftwareAudioInput::Start() { active_ = true; }

void SoftwareAudioInput::Stop() { active_ = false; }

pj_status_t SoftwareAudioInput::PutFrameCallback(pjmedia_port *port, pjmedia_frame *frame) {
    if (frame->type != PJMEDIA_FRAME_TYPE_AUDIO) {
        return PJ_SUCCESS;
    }

    auto *input = (SoftwareAudioInput *) port->port_data.pdata;
    if (!input->active_) {
        return PJ_SUCCESS;
    }

    input->mic_buffer_.push(static_cast<const int16_t *>(frame->buf), frame->size / sizeof(int16_t));

    return PJ_SUCCESS;
}

pj_status_t SoftwareAudioInput::GetFrameCallback(pjmedia_port *port, pjmedia_frame *frame) {
    frame->type = PJMEDIA_FRAME_TYPE_NONE;
    frame->size = 0;
    return PJ_SUCCESS;
}

// ---- SoftwareAudioOutput (tgcalls -> SIP) ----

SoftwareAudioOutput::SoftwareAudioOutput(RingBuffer &playout_buffer) : playout_buffer_(playout_buffer) {
    pj_pool_ = pjsua_pool_create("voip-output%p", 2048, 512);
    media_port_ = PJ_POOL_ZALLOC_T(pj_pool_, pjmedia_port);

    pj_str_t name = pj_str((char *) "voip-output");
    pj_status_t status = pjmedia_port_info_init(&media_port_->info, &name,
                                                PJMEDIA_SIG_CLASS_PORT_AUD('T', 'O'), // tgcalls Output
                                                48000, 1, 16, SAMPLES_PER_FRAME);
    assert(status == PJ_SUCCESS);

    media_port_->port_data.pdata = this;
    media_port_->put_frame = &PutFrameCallback;
    media_port_->get_frame = &GetFrameCallback;

    registerMediaPort(media_port_);
}

SoftwareAudioOutput::~SoftwareAudioOutput() {
    unregisterMediaPort();
    pjmedia_port_destroy(media_port_);
    pj_pool_release(pj_pool_);
}

void SoftwareAudioOutput::Start() { active_ = true; }

void SoftwareAudioOutput::Stop() { active_ = false; }

pj_status_t SoftwareAudioOutput::PutFrameCallback(pjmedia_port *port, pjmedia_frame *frame) {
    frame->type = PJMEDIA_FRAME_TYPE_NONE;
    frame->size = 0;
    return PJ_SUCCESS;
}

pj_status_t SoftwareAudioOutput::GetFrameCallback(pjmedia_port *port, pjmedia_frame *frame) {
    auto *output = (SoftwareAudioOutput *) port->port_data.pdata;

    if (!output->active_) {
        frame->type = PJMEDIA_FRAME_TYPE_NONE;
        frame->size = 0;
        return PJ_SUCCESS;
    }

    output->playout_buffer_.pop(static_cast<int16_t *>(frame->buf), SAMPLES_PER_FRAME);
    frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
    frame->size = SAMPLES_PER_FRAME * sizeof(int16_t);

    return PJ_SUCCESS;
}

// ---- tgcalls::FakeAudioDeviceModule glue ----

bool TgCallsRenderer::Render(const tgcalls::AudioFrame &frame) {
    static std::atomic<uint64_t> call_count{0};
    static std::atomic<int16_t> max_abs{0};

    int16_t local_max = 0;
    for (size_t i = 0; i < frame.num_samples; ++i) {
        local_max = std::max(local_max, static_cast<int16_t>(std::abs(frame.audio_samples[i])));
    }
    auto prev = max_abs.load(std::memory_order_relaxed);
    if (local_max > prev) max_abs.store(local_max, std::memory_order_relaxed);

    auto n = call_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % 100 == 0) {
        spdlog::get("core")->info("[diag] TgCallsRenderer::Render calls={} num_samples={} max_abs_since_start={}",
                                   n, frame.num_samples, max_abs.load(std::memory_order_relaxed));
    }

    playout_buffer_.push(frame.audio_samples, frame.num_samples);
    return true;
}

tgcalls::AudioFrame TgCallsRecorder::Record() {
    mic_buffer_.pop(scratch_, SAMPLES_PER_FRAME);

    tgcalls::AudioFrame frame{};
    frame.audio_samples = scratch_;
    frame.num_samples = SAMPLES_PER_FRAME;
    frame.bytes_per_sample = sizeof(int16_t);
    frame.num_channels = 1;
    frame.samples_per_sec = 48000;
    frame.elapsed_time_ms = 0;
    frame.ntp_time_ms = 0;
    return frame;
}
