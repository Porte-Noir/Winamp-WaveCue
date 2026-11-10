#pragma once
// WaveCue experimental key analysis: genuine libKeyFinder backend.
// GPL-3.0-or-later (libKeyFinder by Ibrahim Sha'ath / Mixxx team).
// Runs only on the existing background decoding worker.
#include <algorithm>
#include <cstdint>
#include <vector>
#include "../third_party/libkeyfinder/src/keyfinder.h"

namespace harmonic {
class Probe {
    unsigned int inputRate_ = 0;
    unsigned int stride_ = 1;
    uint64_t frames_ = 0;
    uint64_t start_ = 0;
    uint64_t limit_ = 0;
    std::vector<float> mono_;
public:
    explicit Probe(int sampleRate) {
        if (sampleRate < 8000 || sampleRate > 384000) return;
        inputRate_ = static_cast<unsigned int>(sampleRate);
        // Bound analysis memory while retaining enough chromatic material.
        // Sample 160 seconds from near the beginning of the song.
        stride_ = std::max(1u, inputRate_ / 22050u);
        start_ = static_cast<uint64_t>(inputRate_) * 5u;
        limit_ = start_ + static_cast<uint64_t>(inputRate_) * 160u;
        mono_.reserve(static_cast<size_t>(160u * (inputRate_ / stride_)));
    }
    void Add(float mono) {
        if (!inputRate_) return;
        const uint64_t index = frames_++;
        if (index < start_ || index >= limit_) return;
        if ((index - start_) % stride_ == 0)
            mono_.push_back(std::clamp(mono, -1.0f, 1.0f));
    }
    // Convert KeyFinder's enumerated key to the pre-existing 0..23
    // pitch-class/mode values used by WaveCue's Camelot labels.
    int Estimate() const {
        if (mono_.size() < static_cast<size_t>(inputRate_ / stride_) * 10)
            return -1;
        try {
            KeyFinder::AudioData audio;
            audio.setChannels(1);
            audio.setFrameRate(inputRate_ / stride_);
            audio.addToSampleCount(static_cast<unsigned int>(mono_.size()));
            for (size_t i = 0; i < mono_.size(); ++i)
                audio.setSample(static_cast<unsigned int>(i), mono_[i]);
            KeyFinder::KeyFinder detector;
            const auto key = detector.keyOfAudio(audio);
            static const int map[24] = {
                9,21,10,22,11,23,0,12,1,13,2,14,
                3,15,4,16,5,17,6,18,7,19,8,20
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
    return key < 0 || key >= 24 ? L"--" : key >= 12 ?
        minors[key - 12] : majors[key];
}
} // namespace harmonic
