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

#ifndef TG2SIP_VOIP_RING_BUFFER_H
#define TG2SIP_VOIP_RING_BUFFER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace voip {

    // Single-producer/single-consumer lock-free ring buffer of PCM samples.
    // Decouples tgcalls' 10ms audio thread from PJSIP's 20ms media thread -
    // the two sides pull/push at different frame sizes and on different
    // threads, so a plain shared buffer would need locking without this.
    class RingBuffer {
    public:
        explicit RingBuffer(size_t capacity_samples)
                : capacity_(capacity_samples), buffer_(capacity_samples) {}

        // Called from the producer thread only.
        void push(const int16_t *data, size_t samples) {
            size_t head = head_.load(std::memory_order_relaxed);
            size_t tail = tail_.load(std::memory_order_acquire);

            size_t available = capacity_ - 1 - (head - tail + capacity_) % capacity_;
            if (samples > available) samples = available;
            if (samples == 0) return;

            for (size_t i = 0; i < samples; ++i) {
                buffer_[(head + i) % capacity_] = data[i];
            }
            head_.store((head + samples) % capacity_, std::memory_order_release);
        }

        // Called from the consumer thread only. Pads with silence on underrun
        // so callers always get exactly `samples` values.
        void pop(int16_t *data, size_t samples) {
            size_t head = head_.load(std::memory_order_acquire);
            size_t tail = tail_.load(std::memory_order_relaxed);

            size_t available = (head - tail + capacity_) % capacity_;

            size_t to_copy = samples < available ? samples : available;
            for (size_t i = 0; i < to_copy; ++i) {
                data[i] = buffer_[(tail + i) % capacity_];
            }
            for (size_t i = to_copy; i < samples; ++i) {
                data[i] = 0;
            }
            tail_.store((tail + to_copy) % capacity_, std::memory_order_release);
        }

    private:
        const size_t capacity_;
        std::vector<int16_t> buffer_;
        std::atomic<size_t> head_{0};
        std::atomic<size_t> tail_{0};
    };

}

#endif //TG2SIP_VOIP_RING_BUFFER_H
