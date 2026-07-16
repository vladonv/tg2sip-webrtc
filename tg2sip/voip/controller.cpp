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

#include <condition_variable>
#include <mutex>

#include "tgcalls/InstanceImpl.h"

#include "controller.h"

using namespace voip;

namespace {

    // tgcalls::Meta's version registry is populated by calling this - it's
    // a plain function (an explicit template specialization), not a
    // self-running static initializer, so nothing happens until someone
    // calls it. Do it once, lazily, on first use.
    void EnsureRegistered() {
        static std::once_flag flag;
        std::call_once(flag, [] {
            tgcalls::Register<tgcalls::InstanceImpl>();
        });
    }

    // Prefer the newer WebRTC-reference protocol version when available;
    // Meta::Create() maps this string to Config::protocolVersion internally
    // ("2.7.7" -> V0, "3.0.0" -> V1 as of the tgcalls source read for this
    // migration).
    std::string PreferredVersion() {
        auto versions = tgcalls::Meta::Versions();
        for (const auto &v : versions) {
            if (v == "3.0.0") return v;
        }
        return versions.empty() ? std::string{} : versions.back();
    }

}

TgCallsController::TgCallsController(std::array<uint8_t, 256> encryption_key,
                                     bool is_outgoing,
                                     std::vector<tgcalls::Endpoint> endpoints,
                                     std::vector<tgcalls::RtcServer> rtc_servers,
                                     bool enable_p2p,
                                     bool enable_aec,
                                     bool enable_ns,
                                     bool enable_agc,
                                     std::function<void(const std::vector<uint8_t> &)> on_signaling_data)
        : encryption_key_(encryption_key),
          is_outgoing_(is_outgoing),
          endpoints_(std::move(endpoints)),
          rtc_servers_(std::move(rtc_servers)),
          enable_p2p_(enable_p2p),
          enable_aec_(enable_aec),
          enable_ns_(enable_ns),
          enable_agc_(enable_agc),
          on_signaling_data_(std::move(on_signaling_data)) {
    EnsureRegistered();
}

TgCallsController::~TgCallsController() {
    if (instance_) {
        Stop();
    }
}

void TgCallsController::Start() {
    tgcalls::Config config;
    // Conservative defaults - tgcalls doesn't document exact units in
    // Instance.h beyond the field names; treated as seconds here to match
    // other double-typed timeout fields in this codebase.
    config.initializationTimeout = 30.;
    config.receiveTimeout = 20.;
    config.dataSaving = tgcalls::DataSaving::Never;
    config.enableP2P = enable_p2p_;
    config.allowTCP = false;
    config.enableStunMarking = false;
    config.enableAEC = enable_aec_;
    config.enableNS = enable_ns_;
    config.enableAGC = enable_agc_;
    config.enableCallUpgrade = false;
    config.maxApiLayer = tgcalls::Meta::MaxLayer();

    tgcalls::FakeAudioDeviceModule::Options adm_options;
    adm_options.samples_per_sec = 48000;
    adm_options.num_channels = 1;

    // tgcalls::Descriptor has no default constructor - EncryptionKey has no
    // default constructor of its own, which makes the whole aggregate's
    // implicit default constructor deleted. Aggregate-initialize instead,
    // in field declaration order (Instance.h), skipping fields we're happy
    // leaving at their own defaults (persistentState/proxy/
    // initialNetworkType/mediaDevicesConfig/videoCapture).
    tgcalls::Descriptor descriptor{
            .config = config,
            .endpoints = endpoints_,
            .rtcServers = rtc_servers_,
            .encryptionKey = tgcalls::EncryptionKey(
                    std::make_shared<std::array<uint8_t, 256>>(encryption_key_), is_outgoing_),
            .stateUpdated = [](tgcalls::State) {},
            .signalBarsUpdated = [](int) {},
            .audioLevelUpdated = [](float) {},
            .remoteBatteryLevelIsLowUpdated = [](bool) {},
            .remoteMediaStateUpdated = [](tgcalls::AudioState, tgcalls::VideoState) {},
            .remotePrefferedAspectRatioUpdated = [](float) {},
            .signalingDataEmitted = [this](const std::vector<uint8_t> &data) {
                if (on_signaling_data_) {
                    on_signaling_data_(data);
                }
            },
            .createAudioDeviceModule = tgcalls::FakeAudioDeviceModule::Creator(renderer_, recorder_, adm_options),
    };

    instance_ = tgcalls::Meta::Create(PreferredVersion(), std::move(descriptor));

    audio_input_.Start();
    audio_output_.Start();
}

void TgCallsController::Connect() {
    // tgcalls starts connecting as soon as the Instance is constructed in
    // Start() - there is no separate connect phase.
}

void TgCallsController::Stop() {
    audio_input_.Stop();
    audio_output_.Stop();

    if (!instance_) {
        return;
    }

    std::mutex m;
    std::condition_variable cv;
    bool done = false;

    instance_->stop([&](tgcalls::FinalState) {
        std::lock_guard<std::mutex> lock(m);
        done = true;
        cv.notify_one();
    });

    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return done; });

    instance_.reset();
}

void TgCallsController::UpdateSignaling(const std::vector<uint8_t> &data) {
    if (instance_) {
        instance_->receiveSignalingData(data);
    }
}

int32_t TgCallsController::GetConnectionMaxLayer() {
    EnsureRegistered();
    return tgcalls::Meta::MaxLayer();
}

std::vector<std::string> TgCallsController::Versions() {
    EnsureRegistered();
    return tgcalls::Meta::Versions();
}
