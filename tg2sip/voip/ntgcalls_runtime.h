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

#ifndef TG2SIP_VOIP_NTGCALLS_RUNTIME_H
#define TG2SIP_VOIP_NTGCALLS_RUNTIME_H

#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <ntgcalls.h>

namespace voip {

    // ntgcalls' flat C API (third_party/ntgcalls/include/ntgcalls.h) is not
    // "one handle per call": ntg_init() returns a handle to a single
    // NTgCalls instance that multiplexes every call by chatId (for P2P
    // calls, chatId == the Telegram peer's user id), and callback
    // registration (ntg_on_signaling_data/ntg_on_frames) is global - called
    // once for the whole instance, with the relevant chatId passed to the
    // callback each time. This class owns that one process-wide instance
    // and routes the global callbacks to whichever TgCallsController
    // currently owns a given chatId.
    //
    // This is also the ABI boundary of the whole ntgcalls integration:
    // everything in this header is plain C (extern "C", primitive/struct
    // types only) so it links safely regardless of which libc++/libstdc++
    // ABI the calling code was compiled with - see the "Boundary decision"
    // section of the ntgcalls migration plan for why that matters.
    class NtgCallsRuntime {
    public:
        static NtgCallsRuntime &Instance();

        NtgCallsRuntime(const NtgCallsRuntime &) = delete;

        NtgCallsRuntime &operator=(const NtgCallsRuntime &) = delete;

        uintptr_t ptr() const { return ptr_; }

        // Registered/unregistered by a TgCallsController around its own
        // Start()/Stop() - see controller.cpp. Both callbacks are invoked
        // with the runtime's internal mutex held (see .cpp), so they must
        // not call back into Register()/Unregister() themselves.
        struct Sink {
            std::function<void(const uint8_t *data, int size)> on_signaling_data;
            std::function<void(ntg_stream_mode_enum mode, ntg_stream_device_enum device,
                                ntg_frame_struct *frames, uint64_t count)> on_frames;
        };

        void Register(int64_t user_id, Sink *sink);

        // Blocks until any callback currently dispatching to `user_id` has
        // returned, and prevents any future one from starting - safe to
        // destroy the Sink and its captured state right after this call
        // returns.
        void Unregister(int64_t user_id);

        // Blocks the calling thread until the async ntg_* call `issue`
        // starts completes (ntgcalls' own promise callback fires), then
        // throws std::runtime_error if it failed. `issue` is
        // `int(ntg_async_struct)`, forwarding straight to an `ntg_*` call.
        template<typename Fn>
        void CallBlocking(Fn &&issue) {
            struct State {
                std::mutex m;
                std::condition_variable cv;
                bool done = false;
                int error_code = 0;
                char *error_message = nullptr;
            } state;

            ntg_async_struct future{};
            future.userData = &state;
            future.errorCode = &state.error_code;
            future.errorMessage = &state.error_message;
            future.promise = [](void *user_data) {
                auto *s = static_cast<State *>(user_data);
                std::lock_guard<std::mutex> lock(s->m);
                s->done = true;
                s->cv.notify_one();
            };

            const int rc = issue(future);
            if (rc != 0) {
                throw std::runtime_error("ntgcalls call rejected synchronously, code " + std::to_string(rc));
            }

            std::unique_lock<std::mutex> lock(state.m);
            state.cv.wait(lock, [&] { return state.done; });

            if (state.error_code != 0) {
                std::string message = state.error_message ? state.error_message : "";
                std::free(state.error_message);
                throw std::runtime_error("ntgcalls error " + std::to_string(state.error_code) + ": " + message);
            }
        }

        // Fire-and-forget variant for hot paths (the mic-feed thread,
        // outbound signaling data) that must not block on ntgcalls' own
        // worker-pool latency. Heap-allocates its completion state and
        // frees it from the promise callback (or immediately, if `issue`
        // rejects the call synchronously).
        template<typename Fn>
        void CallDetached(Fn &&issue) {
            struct State {
                int error_code = 0;
                char *error_message = nullptr;
            };
            auto *state = new State();

            ntg_async_struct future{};
            future.userData = state;
            future.errorCode = &state->error_code;
            future.errorMessage = &state->error_message;
            future.promise = [](void *user_data) {
                auto *s = static_cast<State *>(user_data);
                std::free(s->error_message);
                delete s;
            };

            const int rc = issue(future);
            if (rc != 0) {
                delete state;
            }
        }

    private:
        NtgCallsRuntime();

        ~NtgCallsRuntime();

        static void SignalingTrampoline(uintptr_t ptr, int64_t user_id, uint8_t *data, int size, void *user_data);

        static void FramesTrampoline(uintptr_t ptr, int64_t chat_id, ntg_stream_mode_enum mode,
                                      ntg_stream_device_enum device, ntg_frame_struct *frames,
                                      uint64_t count, void *user_data);

        uintptr_t ptr_;
        std::mutex mutex_;
        std::unordered_map<int64_t, Sink *> sinks_;
    };

}

#endif //TG2SIP_VOIP_NTGCALLS_RUNTIME_H
