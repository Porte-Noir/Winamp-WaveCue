#pragma once
// WaveCue experimental key analysis with genuine libKeyFinder.
// Copyright in libKeyFinder: Ibrahim Sha'ath and Mixxx contributors.
// libKeyFinder: GPL-3.0-or-later. FFTW3: GPL-2.0-or-later.
// Feed every audio frame (mono-averaged across channels) at its native rate.
// Process bounded blocks in the background worker without allocating a full song.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "../third_party/libkeyfinder/src/keyfinder.h"

namespace harmonic {
class Probe {
    unsigned int rate_ = 0;
    size_t framesTotal_ = 0;
    size_t maxFrames_ = 0;
    size_t flushFrames_ = 0;
    bool failed_ = false;
    KeyFinder::KeyFinder engine_;
    KeyFinder::Workspace workspace_;
    std::vector<float> samples_;

    void Flush() {
        if (failed_ || samples_.empty()) return;
        try {
            KeyFinder::AudioData chunk;
            chunk.setChannels(1);
            chunk.setFrameRate(rate_);
            chunk.addToFrameCount(static_cast<unsigned int>(samples_.size()));
            for (size_t i = 0; i < samples_.size(); ++i)
                chunk.setSample(static_cast<unsigned int>(i), samples_[i]);
            engine_.progressiveChromagram(chunk, workspace_);
            samples_.clear();
        } catch (...) {
            failed_ = true;
            samples_.clear();
        }
    }
public:
    explicit Probe(int sampleRate) {
        if (sampleRate < 8000 || sampleRate > 384000) return;
        rate_ = static_cast<unsigned int>(sampleRate);
        // Fifteen minutes limits very long mixes while analyzing typical songs
        // from their beginning through the end, at the true sample rate.
        maxFrames_ = static_cast<size_t>(rate_) * 900;
        flushFrames_ = static_cast<size_t>(rate_) * 8;
        samples_.reserve(flushFrames_);
    }

    void Add(float mono) {
        if (!rate_ || failed_ || framesTotal_ >= maxFrames_) return;
        ++framesTotal_;
        samples_.push_back(std::isfinite(mono) ? std::clamp(mono, -1.0f, 1.0f) : 0.0f);
        if (samples_.size() >= flushFrames_) Flush();
    }

    int Estimate() {
        if (failed_ || !rate_ || framesTotal_ < static_cast<size_t>(rate_) * 10)
            return -1;
        Flush();
        if (failed_) return -1;
        try {
            engine_.finalChromagram(workspace_);
            const auto key = engine_.keyOfChromagram(workspace_);
            // KeyFinder's documented A_MAJOR..A_FLAT_MINOR enumeration.
            // Convert to WaveCue's C..B major then C..B minor indexes.
            static constexpr int map[24] = {
                9, 21, 10, 22, 11, 23, 0, 12, 1, 13, 2, 14,
                3, 15, 4, 16, 5, 17, 6, 18, 7, 19, 8, 20
            };
            const int k = static_cast<int>(key);
            return k >= 0 && k < 24 ? map[k] : -1;
        } catch (...) {
            return -1;
        }
    }
};

inline const wchar_t* Camelot(int key) {
    static constexpr const wchar_t* majors[12] = {
        L"8B",L"3B",L"10B",L"5B",L"12B",L"7B",
        L"2B",L"9B",L"4B",L"11B",L"6B",L"1B"};
    static constexpr const wchar_t* minors[12] = {
        L"5A",L"12A",L"7A",L"2A",L"9A",L"4A",
        L"11A",L"6A",L"1A",L"8A",L"3A",L"10A"};
    return key < 0 || key >= 24 ? L"--" : key >= 12
        ? minors[key - 12] : majors[key];
}
} // namespace harmonic
