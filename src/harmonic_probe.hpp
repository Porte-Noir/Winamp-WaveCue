#pragma once
// Experimental lightweight pitch-class (chroma) estimator.
// Independent of libKeyFinder; intended for A/B testing against KeyFinder.
// Does not interact with playback or the Winamp audio thread.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace harmonic {
constexpr int FFT_SIZE = 8192;
constexpr double PI = 3.14159265358979323846;
using Frame = std::array<float, FFT_SIZE>;

class Probe {
    int inputRate_ = 0;
    int stride_ = 1;
    uint64_t frames_ = 0;
    uint64_t nextFrame_ = 0;
    uint64_t totalLimit_ = 0;
    int collected_ = 0;
    Frame current_{};
    std::vector<Frame> framesToAnalyze_;
public:
    explicit Probe(int sampleRate) : inputRate_(sampleRate) {
        stride_ = std::max(1, sampleRate / 11025);
        totalLimit_ = static_cast<uint64_t>(std::max(0, sampleRate)) * 480;
        // Skip first 8 s to reduce influence of unpitched introductions.
        nextFrame_ = static_cast<uint64_t>(std::max(0, sampleRate)) * 8;
        framesToAnalyze_.reserve(52);
    }
    void Add(float mono) {
        if (inputRate_ < 8000 || inputRate_ > 384000 || frames_ > totalLimit_) {
            ++frames_;
            return;
        }
        const uint64_t index = frames_++;
        if (index < nextFrame_ || framesToAnalyze_.size() >= 52) return;
        if ((index - nextFrame_) % static_cast<uint64_t>(stride_) != 0) return;
        current_[collected_++] = std::clamp(mono, -1.0f, 1.0f);
        if (collected_ == FFT_SIZE) {
            framesToAnalyze_.push_back(current_);
            collected_ = 0;
            nextFrame_ = index + static_cast<uint64_t>(inputRate_) * 6;
        }
    }
    // 0..11 = C..B; 12..23 = Cm..Bm. -1 means insufficient evidence.
    int Estimate() const {
        if (framesToAnalyze_.size() < 3) return -1;
        std::array<double, 12> chroma{};
        using Complex = std::complex<double>;
        std::vector<Complex> spectrum(FFT_SIZE);
        const double rate = static_cast<double>(inputRate_) / stride_;
        int usable = 0;
        for (const Frame& frame : framesToAnalyze_) {
            double energy = 0;
            for (int i = 0; i < FFT_SIZE; ++i) {
                const double window = 0.5 - 0.5 * std::cos(2 * PI * i / (FFT_SIZE - 1));
                const double v = frame[i] * window;
                spectrum[i] = Complex(v, 0);
                energy += v * v;
            }
            if (energy < 0.01) continue;
            // Radix-two FFT, executed only after the requested waveform decode.
            for (int i = 1, j = 0; i < FFT_SIZE; ++i) {
                int bit = FFT_SIZE >> 1;
                for (; j & bit; bit >>= 1) j ^= bit;
                j ^= bit;
                if (i < j) std::swap(spectrum[i], spectrum[j]);
            }
            for (int len = 2; len <= FFT_SIZE; len <<= 1) {
                const double angle = -2.0 * PI / len;
                const Complex step(std::cos(angle), std::sin(angle));
                for (int i = 0; i < FFT_SIZE; i += len) {
                    Complex w(1, 0);
                    for (int j = 0; j < len / 2; ++j) {
                        const Complex a = spectrum[i + j], b = spectrum[i + j + len / 2] * w;
                        spectrum[i + j] = a + b;
                        spectrum[i + j + len / 2] = a - b;
                        w *= step;
                    }
                }
            }
            std::array<double, 12> local{};
            for (int bin = 1; bin < FFT_SIZE / 2; ++bin) {
                const double freq = rate * bin / FFT_SIZE;
                if (freq < 75.0 || freq > 1600.0) continue;
                const double midi = 69.0 + 12.0 * std::log2(freq / 440.0);
                const int note = static_cast<int>(std::round(midi));
                const double dist = std::fabs(midi - note);
                if (dist > 0.32) continue;
                const double amplitude = std::abs(spectrum[bin]);
                local[((note % 12) + 12) % 12] +=
                    std::sqrt(amplitude) * (1.0 - dist / 0.32) /
                    std::sqrt(std::max(freq / 220.0, 0.3));
            }
            double sum = 0;
            for (double v : local) sum += v;
            if (sum < 0.01) continue;
            for (int i = 0; i < 12; ++i) chroma[i] += local[i] / sum;
            ++usable;
        }
        if (usable < 3) return -1;
        // Krumhansl-Schmuckler major/minor profiles; experimental scores.
        const std::array<double, 12> major = {6.35,2.23,3.48,2.33,4.38,4.09,2.52,5.19,2.39,3.66,2.29,2.88};
        const std::array<double, 12> minor = {6.33,2.68,3.52,5.38,2.60,3.53,2.54,4.75,3.98,2.69,3.34,3.17};
        double best = -1e100, second = -1e100;
        int key = -1;
        for (int mode = 0; mode < 2; ++mode) {
            const auto& profile = mode ? minor : major;
            for (int root = 0; root < 12; ++root) {
                double avgC = 0, avgP = 0;
                for (int i = 0; i < 12; ++i) {
                    avgC += chroma[i]; avgP += profile[i];
                }
                avgC /= 12; avgP /= 12;
                double numerator = 0, normC = 0, normP = 0;
                for (int i = 0; i < 12; ++i) {
                    const double c = chroma[(i + root) % 12] - avgC;
                    const double p = profile[i] - avgP;
                    numerator += c * p; normC += c * c; normP += p * p;
                }
                const double score = numerator / std::sqrt(std::max(1e-20, normC * normP));
                if (score > best) { second = best; best = score; key = mode * 12 + root; }
                else if (score > second) second = score;
            }
        }
        if (best < 0.15 || best - second < 0.012) return -1;
        return key;
    }
};

inline const wchar_t* Camelot(int key) {
    static constexpr const wchar_t* majors[12] = {
        L"8B",L"3B",L"10B",L"5B",L"12B",L"7B",L"2B",L"9B",L"4B",L"11B",L"6B",L"1B"};
    static constexpr const wchar_t* minors[12] = {
        L"5A",L"12A",L"7A",L"2A",L"9A",L"4A",L"11A",L"6A",L"1A",L"8A",L"3A",L"10A"};
    return key < 0 || key >= 24 ? L"--" : key >= 12 ? minors[key - 12] : majors[key];
}
} // namespace harmonic
