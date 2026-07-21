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

#include "ntgcalls_runtime.h"

using namespace voip;

NtgCallsRuntime &NtgCallsRuntime::Instance() {
    static NtgCallsRuntime instance;
    return instance;
}

NtgCallsRuntime::NtgCallsRuntime() : ptr_(ntg_init()) {
    ntg_on_signaling_data(ptr_, &NtgCallsRuntime::SignalingTrampoline, this);
    ntg_on_frames(ptr_, &NtgCallsRuntime::FramesTrampoline, this);
}

NtgCallsRuntime::~NtgCallsRuntime() {
    ntg_destroy(ptr_);
}

void NtgCallsRuntime::Register(const int64_t user_id, Sink *sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_[user_id] = sink;
}

void NtgCallsRuntime::Unregister(const int64_t user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.erase(user_id);
}

void NtgCallsRuntime::SignalingTrampoline(uintptr_t /*ptr*/, const int64_t user_id, uint8_t *data, const int size,
                                          void *user_data) {
    auto *self = static_cast<NtgCallsRuntime *>(user_data);
    std::lock_guard<std::mutex> lock(self->mutex_);
    const auto it = self->sinks_.find(user_id);
    if (it != self->sinks_.end() && it->second->on_signaling_data) {
        it->second->on_signaling_data(data, size);
    }
}

void NtgCallsRuntime::FramesTrampoline(uintptr_t /*ptr*/, const int64_t chat_id, const ntg_stream_mode_enum mode,
                                       const ntg_stream_device_enum device, ntg_frame_struct *frames,
                                       const uint64_t count, void *user_data) {
    auto *self = static_cast<NtgCallsRuntime *>(user_data);
    std::lock_guard<std::mutex> lock(self->mutex_);
    const auto it = self->sinks_.find(chat_id);
    if (it != self->sinks_.end() && it->second->on_frames) {
        it->second->on_frames(mode, device, frames, count);
    }
}
