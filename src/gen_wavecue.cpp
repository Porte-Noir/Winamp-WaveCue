// Winamp WaveCue — a lightweight general-purpose Winamp 5.x (32-bit) plugin.
// Windows 7 SP1 or later; decoder: Windows Media Foundation (no audio engine replacement).
// Copyright 2026. Distributed under the MIT license (see LICENSE).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlobj.h>
#include <windowsx.h>
#include <commctrl.h>
#include <limits>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "core.hpp"
#include "harmonic_probe.hpp"
#include "BTrack.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
// Ogg Vorbis, decoded in the background without changing Winamp playback.
#include "stb_vorbis.c"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// Small, self-contained definitions from the Winamp 5 general-purpose plugin SDK.
// Read the full reference: https://github.com/hugovk/winamp/blob/master/wa_ipc.h
#define WM_WA_IPC (WM_USER)
#define IPC_ISPLAYING 104
#define IPC_GETOUTPUTTIME 105
#define IPC_JUMPTOTIME 106
#define IPC_GETLISTPOS 125
#define IPC_GETLISTLENGTH 124
#define IPC_SETPLAYLISTPOS 121
#define WINAMP_BUTTON2 40045
#define IPC_GETPLAYLISTFILE 211
#define IPC_GETPLAYLISTFILEW 214
#define IPC_PLAYLIST_GET_NEXT_SELECTED 3029
#define WINAMP_BUTTON5 40048
#define WM_WAVE_RESULT (WM_APP + 24)
#define TIMER_POLL 11
#define IPC_CB_MISC 603
#define IPC_CB_MISC_STATUS 2

struct winampGeneralPurposePlugin {
    int version;
    char* description;
    int (*init)();
    void (*config)();
    void (*quit)();
    HWND hwndParent;
    HINSTANCE hDllInstance;
};

static int PluginInit();
static void PluginConfig();
static void PluginQuit();
static char kDescription[] = "WaveCue - Waveform et Cue Editor 1.0";
static winampGeneralPurposePlugin g_plugin = {
    0x10, kDescription, PluginInit, PluginConfig, PluginQuit, nullptr, nullptr
};
extern "C" __declspec(dllexport) winampGeneralPurposePlugin* __cdecl
winampGetGeneralPurposePlugin() { return &g_plugin; }

namespace {
using wavecue::Cues;

// Plugin state on the Winamp/UI thread.
HWND g_hwnd = nullptr;
HWND g_propertiesHwnd = nullptr;
constexpr UINT ID_PROPERTIES_MENU = 0x1F10;
constexpr int ID_PROPERTIES_CLOSE = 1101;
constexpr int ID_PREF_GENERAL = 1201, ID_PREF_ANALYSIS = 1202, ID_PREF_ABOUT = 1203;
constexpr int ID_PREF_BATCH = 1211, ID_PREF_BATCH_CANCEL = 1212, ID_PREF_AUTO = 1213;
int g_propertiesPage = 0;
bool g_autoAnalyzeSelection = false;
std::vector<HWND> g_propertyPageControls;
HBRUSH g_propertiesBrush = nullptr;
constexpr int BATCH_LIMIT = 30;
std::atomic<HWND> g_noticeHwnd{nullptr};
HWND g_winamp = nullptr;
bool g_winampSubclassInstalled = false;
HFONT g_font = nullptr;
HFONT g_bigFont = nullptr;
HBRUSH g_lcdDotBrush = nullptr; // cached Winamp-style LCD background texture
std::wstring g_dataDir;
std::wstring g_cacheDir;
std::wstring g_selectedPath;
int g_selectedIndex = -1;
int g_playingIndex = -1;
std::wstring g_playingPath;
int64_t g_playheadMs = -1;
Cues g_cues;
Cues g_playbackCues;
bool g_skipSent = false;
bool g_masterAuto = true; // Global safety override, independent of track cues.
bool g_streamDelayEnabled = false;
int g_streamDelaySeconds = 10;
bool g_delayDragging = false;
RECT g_delaySlider = {327, 19, 700, 37};
// Store only positions/timestamps, never audio. Covers at least 60 seconds.
struct PlaybackSample {
    ULONGLONG tick;
    std::wstring path;
    int64_t position;
};
std::deque<PlaybackSample> g_recentPlayback;
int64_t g_delayedPlayhead = -1;
bool g_delayedOnPreviousTrack = false;
bool g_startPending = false; // Wait until Winamp actually starts the new file.
std::wstring g_switchPendingPath;
int64_t g_switchPendingMs = -1;
ULONGLONG g_switchPendingUntil = 0;
bool g_wasStopped = true;
bool g_isLoading = false;
std::wstring g_status = L"Selectionnez un morceau dans la playlist de Winamp.";

struct WaveData {
    uint64_t duration_ms = 0;
    std::vector<uint16_t> peaks;
    std::vector<uint16_t> rms; // 50ms loudness envelope
    double rmsDisplayGain = 1.0; // visual scaling only; never affects audio
    int harmonicKey = -2; // -2 unknown; -1 uncertain; 0..23 pitch class + mode
    int bpm = -1; // experimental BTrack result; -1 uncertain
    bool bpmDouble = false; // persisted alternative tempo view (x2 or /2), per track
};
std::shared_ptr<WaveData> g_wave;
std::shared_ptr<WaveData> g_playingWave;
bool g_playingLoading = false;
RECT g_playingPlot = {28, 83, 1000, 218};

// Decoding runs exclusively in a background MTA thread.
struct FileStamp { uint64_t size = 0; uint64_t modified = 0; };
struct Task { std::wstring path; FileStamp stamp; uint64_t generation = 0; bool playing = false; bool batch = false; };
struct Result {
    Task task;
    std::shared_ptr<WaveData> wave;
    std::wstring error;
};
std::mutex g_taskMutex;
std::condition_variable g_taskWake;
std::unique_ptr<Task> g_pendingTask;
std::unique_ptr<Task> g_pendingPlayingTask;
std::deque<Task> g_batchTasks;
std::atomic<uint64_t> g_batchGeneration{0};
std::atomic<bool> g_batchPreempt{false};
std::atomic<int> g_batchCompleted{0};
std::atomic<int> g_batchTotal{0};
std::unique_ptr<Result> g_completedPlaying;
std::atomic<uint64_t> g_playGeneration{0};
std::unique_ptr<Result> g_completed;
std::atomic<uint64_t> g_generation{0};
std::atomic<int> g_progress{0};
std::thread g_worker;
bool g_workerStop = false;

// Winamp classic skin: graphite/violet panels with LCD green accents.
constexpr COLORREF BG = RGB(25, 25, 37);
constexpr COLORREF PANEL = RGB(37, 37, 53);
constexpr COLORREF BORDER = RGB(83, 79, 102);
constexpr COLORREF TEXT = RGB(221, 224, 229);
constexpr COLORREF MUTED = RGB(155, 156, 174);
constexpr COLORREF LCD = RGB(124, 225, 66);
constexpr COLORREF LCD_WAVE = RGB(111, 199, 186); // original cyan
constexpr COLORREF LCD_DOT = RGB(27, 29, 33); // faint neutral dotted grid
constexpr COLORREF LCD_BG = RGB(0, 0, 0); // pure black
constexpr COLORREF LCD_FRAME = RGB(139, 142, 155); // light gray steel
constexpr COLORREF SECTION_YELLOW = RGB(239, 216, 105);
constexpr int LCD_DOT_PITCH = 7; // easy to retune; one tiny cached tile
constexpr COLORREF ACCENT = RGB(111, 199, 186);
constexpr COLORREF CUE = RGB(220, 182, 103);
constexpr COLORREF START = RGB(127, 220, 105);
constexpr COLORREF END = RGB(234, 112, 126);

RECT g_plot = {34, 130, 1040, 435};
enum Mode { ModeNone, ModeSeek, ModeStart, ModeEnd, ModeDelete, ModeSwitch };
Mode g_mode = ModeNone;
enum Marker { MarkerNone, MarkerCue, MarkerStart, MarkerEnd };
Marker g_marker = MarkerNone;
int g_markerIndex = -1;
bool g_dragging = false;
bool g_moved = false;
POINT g_down = {0,0};

struct Button { int id; RECT bounds; const wchar_t* text; bool enabled; };
std::vector<Button> g_buttons;
int g_hover = 0;
constexpr int BUTTON_LOAD = 1, BUTTON_SEEK = 2, BUTTON_START = 3,
              BUTTON_END = 4, BUTTON_DELETE = 5, BUTTON_AUTO = 6,
              BUTTON_PLAYING_AUTO = 7, BUTTON_PLAYING_LOAD = 8, BUTTON_MASTER_AUTO = 9,
              BUTTON_STREAM_DELAY = 10, BUTTON_SWITCH = 11;

std::wstring FileLabel(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring TimeLabel(int64_t ms) {
    if (ms < 0) return L"--:--";
    wchar_t buf[64]{};
    uint64_t v = static_cast<uint64_t>(ms);
    swprintf_s(buf, 64, L"%llu:%02llu.%03llu", v / 60000,
               (v / 1000) % 60, v % 1000);
    return buf;
}

bool ReadFileStamp(const std::wstring& path, FileStamp& out) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    out.size = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    out.modified = (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32)
                 | d.ftLastWriteTime.dwLowDateTime;
    return true;
}

std::wstring HexHash(const std::wstring& path) {
    wchar_t buf[32]{};
    swprintf_s(buf, 32, L"%016llx", wavecue::path_hash(path));
    return buf;
}

std::wstring CuePath(const std::wstring& path) {
    return g_dataDir + L"\\cues_" + HexHash(path) + L".txt";
}
std::wstring CachePath(const std::wstring& path) {
    return g_cacheDir + L"\\" + HexHash(path) + L".wfc";
}

Cues LoadCues(const std::wstring& path) {
    Cues result;
    if (path.empty()) return result;
    FILE* f = nullptr;
    if (_wfopen_s(&f, CuePath(path).c_str(), L"rb") != 0 || !f) return result;
    char line[128]{};
    while (fgets(line, sizeof line, f)) {
        long long ms = -1;
        if (sscanf_s(line, "S=%lld", &ms) == 1 && ms >= 0) result.start_ms = ms;
        else if (sscanf_s(line, "E=%lld", &ms) == 1 && ms >= 0) result.end_ms = ms;
        else if (sscanf_s(line, "A=%lld", &ms) == 1) result.auto_apply = (ms == 1);
        else if (sscanf_s(line, "C=%lld", &ms) == 1 && ms >= 0 && result.marks.size() < 10000)
            result.marks.push_back(ms);
    }
    fclose(f);
    wavecue::tidy(result);
    return result;
}

void SaveCues(const std::wstring& path, const Cues& c) {
    if (path.empty()) return;
    const std::wstring dest = CuePath(path), temp = dest + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, temp.c_str(), L"wb") != 0 || !f) return;
    fputs("# WaveCue v1\n", f);
    if (c.start_ms >= 0) fprintf(f, "S=%lld\n", static_cast<long long>(c.start_ms));
    if (c.end_ms >= 0) fprintf(f, "E=%lld\n", static_cast<long long>(c.end_ms));
    fprintf(f, "A=%d\n", c.auto_apply ? 1 : 0);
    for (auto ms : c.marks) fprintf(f, "C=%lld\n", static_cast<long long>(ms));
    if (fclose(f) == 0) {
        if (!MoveFileExW(temp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            DeleteFileW(temp.c_str());
    } else DeleteFileW(temp.c_str());
}

void LoadMasterAuto() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, (g_dataDir + L"\\master_auto.txt").c_str(), L"rb") != 0 || !f) return;
    int enabled = 1;
    if (fscanf_s(f, "%d", &enabled) == 1)
        g_masterAuto = enabled != 0;
    fclose(f);
}

void SaveMasterAuto() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, (g_dataDir + L"\\master_auto.txt").c_str(), L"wb") != 0 || !f) return;
    fprintf(f, "%d\n", g_masterAuto ? 1 : 0);
    fclose(f);
}

void LoadStreamDelay() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, (g_dataDir + L"\\stream_delay.txt").c_str(), L"rb") != 0 || !f) return;
    int enabled = 0, seconds = 10;
    if (fscanf_s(f, "%d %d", &enabled, &seconds) == 2) {
        g_streamDelayEnabled = enabled != 0;
        g_streamDelaySeconds = std::clamp(seconds, 0, 60);
    }
    fclose(f);
}

void SaveStreamDelay() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, (g_dataDir + L"\\stream_delay.txt").c_str(), L"wb") != 0 || !f) return;
    fprintf(f, "%d %d\n", g_streamDelayEnabled ? 1 : 0, g_streamDelaySeconds);
    fclose(f);
}

void UpdateDelayedPlayhead() {
    g_delayedPlayhead = -1;
    g_delayedOnPreviousTrack = false;
    if (!g_streamDelayEnabled || g_playingPath.empty() || g_playheadMs < 0)
        return;
    if (!g_streamDelaySeconds) {
        g_delayedPlayhead = g_playheadMs;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG target = now > static_cast<ULONGLONG>(g_streamDelaySeconds) * 1000
        ? now - static_cast<ULONGLONG>(g_streamDelaySeconds) * 1000 : 0;
    for (auto it = g_recentPlayback.rbegin(); it != g_recentPlayback.rend(); ++it) {
        if (it->tick > target) continue;
        if (_wcsicmp(it->path.c_str(), g_playingPath.c_str()) == 0)
            g_delayedPlayhead = it->position;
        else
            g_delayedOnPreviousTrack = true;
        break;
    }
}

void CommitCues() {
    wavecue::tidy(g_cues);
    SaveCues(g_selectedPath, g_cues);
    if (g_selectedPath == g_playingPath) g_playbackCues = g_cues;
}

#pragma pack(push,1)
struct CacheHeader {
    uint32_t magic;       // WCUE
    uint32_t version;
    uint64_t file_size;
    uint64_t last_write;
    uint64_t duration_ms;
    uint32_t interval_ms;
    uint32_t peak_count;
    int32_t bpm; // cache v3: BPM stored directly with waveform, no legacy migration
};
#pragma pack(pop)
static_assert(sizeof(CacheHeader) == 44, "Cache header must be stable");
constexpr uint32_t CACHE_MAGIC = 0x45554357u;
constexpr uint32_t RMS_CACHE_VERSION = 5; // BPM enabled, re-analyze constructor-only diagnostic caches

// Keep each track's original/alternative tempo preference independent from analysis.
std::wstring BpmChoicePath(const std::wstring& path) { return CachePath(path) + L".fast"; }
bool ReadBpmChoice(const std::wstring& path) {
    FILE* f = nullptr;
    if (path.empty() || _wfopen_s(&f, BpmChoicePath(path).c_str(), L"rb") != 0 || !f)
        return false;
    const int c = fgetc(f);
    fclose(f);
    return c == '1';
}
void WriteBpmChoice(const std::wstring& path, bool alternative) {
    if (path.empty()) return;
    const std::wstring dest = BpmChoicePath(path);
    const std::wstring temp = dest + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, temp.c_str(), L"wb") != 0 || !f) return;
    bool ok = fputc(alternative ? '1' : '0', f) != EOF;
    if (fclose(f) != 0) ok = false;
    if (!ok || !MoveFileExW(temp.c_str(), dest.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileW(temp.c_str());
}
std::wstring KeyCachePath(const std::wstring& path) { return CachePath(path) + L".key"; }
int ReadKeyCache(const Task& task) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, KeyCachePath(task.path).c_str(), L"rb") != 0 || !f) return -2;
    unsigned long long size = 0, stamp = 0;
    int key = -2, version = 0;
    const int got = fscanf_s(f, "%d %llu %llu %d", &version, &size, &stamp, &key);
    fclose(f);
    if (got != 4 || version != 4 || size != task.stamp.size ||
        stamp != task.stamp.modified || key < -1 || key >= 24) return -2;
    return key;
}
void WriteKeyCache(const Task& task, int key) {
    FileStamp stamp{};
    if (!ReadFileStamp(task.path, stamp) || stamp.size != task.stamp.size ||
        stamp.modified != task.stamp.modified) return;
    const auto dest = KeyCachePath(task.path);
    const auto temp = dest + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, temp.c_str(), L"wb") != 0 || !f) return;
    fprintf(f, "4 %llu %llu %d\n",
        static_cast<unsigned long long>(stamp.size),
        static_cast<unsigned long long>(stamp.modified), key);
    if (fclose(f) == 0) {
        if (!MoveFileExW(temp.c_str(), dest.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) DeleteFileW(temp.c_str());
    } else DeleteFileW(temp.c_str());
}

bool TaskCurrent(const Task& task) {
    if (task.batch)
        return task.generation == g_batchGeneration.load() && !g_batchPreempt.load();
    return task.generation == (task.playing ? g_playGeneration.load() : g_generation.load());
}

// Compute just once when the waveform is loaded/decoded, not every paint.
// Preserve the RMS dynamics while making quiet masters easier to inspect.
// Cap gain so almost-silent files and residual noise stay quiet.
void SetRmsDisplayGain(WaveData& wave) {
    if (wave.rms.empty()) { wave.rmsDisplayGain = 1.0; return; }
    // The upper percentile represents the actual body of the track rather
    // than a single loud transient. Once per load; no extra work on repaint.
    std::vector<uint16_t> levels = wave.rms;
    const size_t position = (levels.size() - 1) * 90 / 100;
    std::nth_element(levels.begin(), levels.begin() + position, levels.end());
    const uint16_t reference = levels[position];
    // Allow up to 8x for quieter masters. Cap displayed heights at the panel
    // boundary; silence stays silent and the audio itself is untouched.
    wave.rmsDisplayGain = reference
        ? std::max(1.0, std::min(8.0, (32767.0 * 0.92) / reference))
        : 1.0;
}

// Experimental BTrack BPM analysis using the existing 50ms RMS envelope.
// BTrack's internal tempo mathematics assumes 44.1kHz: hop 2205 = 50ms.
// Analyze a maximum of 90 seconds starting at 1:00, avoiding intros.
// The result is limited to BTrack's conventional 80-160 BPM range.
int EstimateBpmFromRms(const WaveData& wave, const Task& task) {
    constexpr size_t startBin = 60 * 20;
    constexpr size_t durationBins = 60 * 20;
    // RMS is sampled each 50ms; BTrack itself expects a 512-sample hop at
    // 44.1kHz (~11.6ms), so interpolate the low-resolution envelope.
    if (wave.rms.size() < startBin + 20 * 15) return -1;
    BTrack tracker(512, 1024); // the library's documented standard settings
    const size_t stop = std::min(wave.rms.size(), startBin + durationBins);
    constexpr double stepsPerBucket = 2205.0 / 512.0;
    const size_t steps = static_cast<size_t>((stop - startBin) * stepsPerBucket);
    double previous = static_cast<double>(wave.rms[startBin - 1]) / 32768.0;
    double smooth = previous;
    std::vector<double> tempos;
    tempos.reserve(60);
    for (size_t step = 0; step < steps && TaskCurrent(task); ++step) {
        const double position = static_cast<double>(step) / stepsPerBucket;
        const size_t offset = static_cast<size_t>(position);
        const double fraction = position - static_cast<double>(offset);
        const size_t index = startBin + offset;
        if (index >= stop) break;
        const size_t next = std::min(index + 1, stop - 1);
        const double left = static_cast<double>(wave.rms[index]) / 32768.0;
        const double right = static_cast<double>(wave.rms[next]) / 32768.0;
        const double energy = left + (right - left) * fraction;
        smooth += 0.025 * (energy - smooth);
        const double onset = std::max(0.0, energy - previous) +
                             0.25 * std::max(0.0, energy - smooth);
        tracker.processOnsetDetectionFunctionSample(onset);
        previous = energy;
        // Give BTrack 15 seconds to settle, then sample its estimates.
        if (step > 15 * 44100 / 512 && step % 86 == 0) {
            const double tempo = tracker.getCurrentTempoEstimate();
            if (std::isfinite(tempo) && tempo >= 80 && tempo <= 160)
                tempos.push_back(tempo);
        }
    }
    if (!TaskCurrent(task) || tempos.size() < 5) return -1;
    const auto median = tempos.begin() + tempos.size() / 2;
    std::nth_element(tempos.begin(), median, tempos.end());
    const int bpm = static_cast<int>(std::lround(*median));
    return bpm >= 80 && bpm <= 160 ? bpm : -1;
}

std::shared_ptr<WaveData> LoadCache(const Task& task) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, CachePath(task.path).c_str(), L"rb") != 0 || !f) return {};
    CacheHeader hdr{};
    if (fread(&hdr, sizeof hdr, 1, f) != 1 || hdr.magic != CACHE_MAGIC ||
        hdr.version != RMS_CACHE_VERSION || hdr.file_size != task.stamp.size ||
        hdr.last_write != task.stamp.modified || hdr.interval_ms != 50 ||
        hdr.peak_count == 0 || hdr.peak_count > 1000000 || hdr.duration_ms == 0) {
        fclose(f);
        return {};
    }
    auto wave = std::make_shared<WaveData>();
    wave->duration_ms = hdr.duration_ms;
    wave->bpm = hdr.bpm >= 80 && hdr.bpm <= 160 ? hdr.bpm : -1;
    wave->bpmDouble = ReadBpmChoice(task.path);
    wave->peaks.resize(hdr.peak_count);
    if (fread(wave->peaks.data(), sizeof(uint16_t), hdr.peak_count, f) != hdr.peak_count) {
        fclose(f);
        return {};
    }
    wave->rms.resize(hdr.peak_count);
    if (fread(wave->rms.data(), sizeof(uint16_t), hdr.peak_count, f) != hdr.peak_count) {
        fclose(f);
        return {};
    }
    fclose(f);
    SetRmsDisplayGain(*wave);
    wave->harmonicKey = ReadKeyCache(task);
    return wave;
}

void SaveCache(const Task& task, const WaveData& wave) {
    if (wave.peaks.empty() || wave.peaks.size() > 1000000 ||
        wave.rms.size() != wave.peaks.size()) return;
    FileStamp current{};
    if (!ReadFileStamp(task.path, current) || current.size != task.stamp.size ||
        current.modified != task.stamp.modified) return;
    std::wstring dest = CachePath(task.path), temp = dest + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, temp.c_str(), L"wb") != 0 || !f) return;
    CacheHeader hdr{CACHE_MAGIC, RMS_CACHE_VERSION, current.size, current.modified,
                    wave.duration_ms, 50, static_cast<uint32_t>(wave.peaks.size()), wave.bpm};
    bool ok = fwrite(&hdr, sizeof hdr, 1, f) == 1 &&
        fwrite(wave.peaks.data(), sizeof(uint16_t), wave.peaks.size(), f) == wave.peaks.size() &&
        fwrite(wave.rms.data(), sizeof(uint16_t), wave.rms.size(), f) == wave.rms.size();
    if (fclose(f) != 0) ok = false;
    if (!ok || !MoveFileExW(temp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileW(temp.c_str());
    else if (wave.harmonicKey >= -1) WriteKeyCache(task, wave.harmonicKey);
}

// Streaming 50ms RMS calculated once on the decode thread, then cached.
struct RmsAccumulator {
    size_t bucket = 0;
    double sumSquares = 0.0;
    uint64_t count = 0;
    void Flush(WaveData& wave) {
        if (!count) return;
        if (bucket >= wave.rms.size()) wave.rms.resize(bucket + 1, 0);
        const double amplitude = std::sqrt(sumSquares / static_cast<double>(count));
        wave.rms[bucket] = static_cast<uint16_t>(
            std::min(32767.0, std::max(0.0, amplitude * 32767.0)));
    }
    void Add(WaveData& wave, size_t bin, double normalizedMono) {
        if (count && bin != bucket) {
            Flush(wave);
            sumSquares = 0.0;
            count = 0;
        }
        bucket = bin;
        if (!std::isfinite(normalizedMono)) normalizedMono = 0.0;
        sumSquares += normalizedMono * normalizedMono;
        ++count;
    }
};

// Native FLAC decoding, independent of Windows codecs; background and cache-aware.
std::shared_ptr<WaveData> DecodeFlac(const Task& task, std::wstring& error) {
    drflac* flac = drflac_open_file_w(task.path.c_str(), nullptr);
    if (!flac) { error = L"Impossible d'ouvrir ce fichier FLAC."; return {}; }
    const drflac_uint32 channels = flac->channels, rate = flac->sampleRate;
    if (channels == 0 || channels > 32 || rate == 0) {
        error = L"En-tete FLAC invalide."; drflac_close(flac); return {};
    }
    constexpr drflac_uint64 CHUNK = 4096;
    std::vector<drflac_int16> samples(static_cast<size_t>(CHUNK) * channels);
    auto wave = std::make_shared<WaveData>();
    harmonic::Probe harmonicProbe(static_cast<int>(rate));
    RmsAccumulator rms;
    const uint64_t bucketFrames = std::max<uint32_t>(1, rate / 20);
    uint64_t frameCount = 0;
    while (TaskCurrent(task)) {
        drflac_uint64 got = drflac_read_pcm_frames_s16(flac, CHUNK, samples.data());
        if (got == 0) break;
        for (drflac_uint64 i = 0; i < got; ++i, ++frameCount) {
            size_t bin = static_cast<size_t>(frameCount / bucketFrames);
            if (bin >= 1000000) { error = L"FLAC trop long pour cette version."; break; }
            if (bin >= wave->peaks.size()) wave->peaks.resize(bin + 1, 0);
            int maximum = 0;
            double mixed = 0;
            for (drflac_uint32 ch = 0; ch < channels; ++ch) {
                int sample = static_cast<int>(samples[static_cast<size_t>(i) * channels + ch]);
                maximum = std::max(maximum, sample < 0 ? -sample : sample);
                mixed += static_cast<double>(sample);
            }
            wave->peaks[bin] = static_cast<uint16_t>(std::max<int>(
                wave->peaks[bin], std::min(maximum, 32767)));
            harmonicProbe.Add(static_cast<float>(mixed / (32768.0 * channels)));
            rms.Add(*wave, bin, mixed / (32768.0 * channels));
        }
        if (!error.empty()) break;
        if (flac->totalPCMFrameCount > 0)
            if (!task.playing) g_progress.store(static_cast<int>(std::min<uint64_t>(99, frameCount * 100 / flac->totalPCMFrameCount)));
        Sleep(0);
    }
    drflac_close(flac);
    if (!TaskCurrent(task) || !error.empty()) return {};
    if (frameCount == 0 || wave->peaks.empty()) {
        error = L"FLAC sans echantillons audio."; return {};
    }
    rms.Flush(*wave);
    wave->rms.resize(wave->peaks.size(), 0);
    SetRmsDisplayGain(*wave);
    wave->duration_ms = std::max<uint64_t>(1, frameCount * 1000 / rate);
    wave->harmonicKey = harmonicProbe.Estimate();
    return wave;
}

// OGG Vorbis waveform analysis uses a streaming decoder. It never allocates
// a full decoded song, and shares the existing ~50ms peak-cache format.
std::shared_ptr<WaveData> DecodeOgg(const Task& task, std::wstring& error) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, task.path.c_str(), L"rb") != 0 || !file) {
        error = L"Impossible d'ouvrir ce fichier OGG."; return {};
    }
    int decoderError = 0;
    stb_vorbis* vorbis = stb_vorbis_open_file(file, 1, &decoderError, nullptr);
    if (!vorbis) {
        // On failure stb_vorbis does not consistently take ownership.
        // Avoid a double fclose: the decoder's documented ownership applies
        // to successfully opened streams.
        fclose(file);
        error = L"OGG Vorbis invalide ou codec OGG non pris en charge.";
        return {};
    }
    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    const int channels = info.channels, rate = info.sample_rate;
    if (channels < 1 || channels > 32 || rate <= 0) {
        stb_vorbis_close(vorbis);
        error = L"Format audio OGG invalide."; return {};
    }
    constexpr int chunkFrames = 4096;
    std::vector<short> samples(static_cast<size_t>(chunkFrames) * channels);
    auto wave = std::make_shared<WaveData>();
    harmonic::Probe harmonicProbe(rate);
    RmsAccumulator rms;
    const uint64_t bucketFrames = std::max<uint32_t>(1, static_cast<uint32_t>(rate) / 20);
    const uint64_t expectedFrames = stb_vorbis_stream_length_in_samples(vorbis);
    uint64_t frames = 0;
    while (TaskCurrent(task)) {
        const int got = stb_vorbis_get_samples_short_interleaved(
            vorbis, channels, samples.data(), static_cast<int>(samples.size()));
        if (got <= 0) break;
        for (int i = 0; i < got; ++i, ++frames) {
            const size_t bin = static_cast<size_t>(frames / bucketFrames);
            if (bin >= 1000000) { error = L"OGG trop long pour cette version."; break; }
            if (bin >= wave->peaks.size()) wave->peaks.resize(bin + 1, 0);
            int peak = 0;
            double mixed = 0;
            for (int ch = 0; ch < channels; ++ch) {
                const int sample = samples[static_cast<size_t>(i) * channels + ch];
                peak = std::max(peak, sample < 0 ? -sample : sample);
                mixed += static_cast<double>(sample);
            }
            wave->peaks[bin] = static_cast<uint16_t>(
                std::max<int>(wave->peaks[bin], std::min(peak, 32767)));
            harmonicProbe.Add(static_cast<float>(mixed / (32768.0 * channels)));
            rms.Add(*wave, bin, mixed / (32768.0 * channels));
        }
        if (!error.empty()) break;
        if (expectedFrames > 0 && !task.playing)
            g_progress.store(static_cast<int>(std::min<uint64_t>(99, frames * 100 / expectedFrames)));
        Sleep(0);
    }
    stb_vorbis_close(vorbis);
    if (!TaskCurrent(task) || !error.empty()) return {};
    if (!frames || wave->peaks.empty()) {
        error = L"Aucun echantillon audio Vorbis exploitable."; return {};
    }
    rms.Flush(*wave);
    wave->rms.resize(wave->peaks.size(), 0);
    SetRmsDisplayGain(*wave);
    wave->duration_ms = std::max<uint64_t>(1, frames * 1000 / static_cast<uint32_t>(rate));
    wave->harmonicKey = harmonicProbe.Estimate();
    return wave;
}

// AIFF and AIFC are IFF containers with big-endian chunk lengths. Supported
// audio: signed PCM (8/16/24/32-bit), AIFC sowt (little-endian PCM),
// and AIFC fl32 / fl64 floating point.
uint32_t AiffU32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint16_t AiffU16(const unsigned char* p) {
    return static_cast<uint16_t>((uint16_t(p[0]) << 8) | p[1]);
}
bool AiffTag(const unsigned char* p, const char* tag) {
    return memcmp(p, tag, 4) == 0;
}
double AiffRate(const unsigned char* p) {
    const uint16_t exponent = AiffU16(p);
    if ((exponent & 0x7fff) == 0 || (exponent & 0x7fff) == 0x7fff ||
        (exponent & 0x8000)) return 0;
    uint64_t mantissa = 0;
    for (int i = 2; i < 10; ++i) mantissa = (mantissa << 8) | p[i];
    return std::ldexp(static_cast<double>(mantissa), (exponent & 0x7fff) - 16383 - 63);
}
std::shared_ptr<WaveData> DecodeAiff(const Task& task, std::wstring& error) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, task.path.c_str(), L"rb") != 0 || !f) {
        error = L"Impossible d'ouvrir ce fichier AIFF."; return {};
    }
    unsigned char form[12]{};
    if (fread(form, 1, sizeof(form), f) != sizeof(form) ||
        !AiffTag(form, "FORM") ||
        (!AiffTag(form + 8, "AIFF") && !AiffTag(form + 8, "AIFC"))) {
        fclose(f); error = L"En-tete AIFF invalide."; return {};
    }
    const bool aifc = AiffTag(form + 8, "AIFC");
    const uint64_t formEnd = uint64_t(AiffU32(form + 4)) + 8;
    if (formEnd < 12 || formEnd > task.stamp.size) {
        fclose(f); error = L"Taille AIFF invalide."; return {};
    }
    uint32_t channels = 0, framesDeclared = 0, bits = 0;
    double sampleRate = 0;
    int encoding = 0; // 0 = big endian integer, 1 = little endian integer, 2/3 = IEEE float
    uint64_t audioOffset = 0, audioBytes = 0;
    for (uint64_t pos = 12; pos + 8 <= formEnd;) {
        unsigned char header[8]{};
        if (_fseeki64(f, static_cast<__int64>(pos), SEEK_SET) != 0 ||
            fread(header, 1, 8, f) != 8) break;
        const uint64_t size = AiffU32(header + 4), start = pos + 8;
        if (start + size > formEnd) {
            fclose(f); error = L"Bloc AIFF incomplet."; return {};
        }
        if (AiffTag(header, "COMM")) {
            unsigned char comm[22]{};
            if (size < (aifc ? 22u : 18u) ||
                fread(comm, 1, aifc ? 22 : 18, f) != (aifc ? 22u : 18u)) {
                fclose(f); error = L"Bloc COMM AIFF invalide."; return {};
            }
            channels = AiffU16(comm);
            framesDeclared = AiffU32(comm + 2);
            bits = AiffU16(comm + 6);
            sampleRate = AiffRate(comm + 8);
            if (aifc) {
                if (AiffTag(comm + 18, "sowt")) encoding = 1;
                else if (AiffTag(comm + 18, "NONE") ||
                         AiffTag(comm + 18, "twos")) encoding = 0;
                else if (AiffTag(comm + 18, "fl32") ||
                         AiffTag(comm + 18, "FL32")) encoding = 2;
                else if (AiffTag(comm + 18, "fl64") ||
                         AiffTag(comm + 18, "FL64")) encoding = 3;
                else {
                    fclose(f);
                    error = L"Compression AIFF-C non prise en charge."; return {};
                }
            }
        } else if (AiffTag(header, "SSND")) {
            unsigned char sound[8]{};
            if (size < 8 || fread(sound, 1, 8, f) != 8) {
                fclose(f); error = L"Bloc SSND AIFF invalide."; return {};
            }
            const uint64_t offset = AiffU32(sound);
            if (offset > size - 8) {
                fclose(f); error = L"Decalage SSND AIFF invalide."; return {};
            }
            audioOffset = start + 8 + offset;
            audioBytes = size - 8 - offset;
        }
        pos = start + size + (size & 1u);
    }
    const uint32_t bytesPerSample = (bits + 7) / 8;
    const bool supported = (encoding <= 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
                           (encoding == 2 && bits == 32) || (encoding == 3 && bits == 64);
    if (!supported || channels < 1 || channels > 32 ||
        !(sampleRate >= 1000 && sampleRate <= 768000) ||
        !audioOffset || !audioBytes) {
        fclose(f); error = L"Format ou donnees AIFF non pris en charge."; return {};
    }
    const uint64_t frameBytes = uint64_t(channels) * bytesPerSample;
    const uint64_t availableFrames = audioBytes / frameBytes;
    const uint64_t totalFrames = framesDeclared ?
        std::min<uint64_t>(framesDeclared, availableFrames) : availableFrames;
    if (!totalFrames || _fseeki64(f, static_cast<__int64>(audioOffset), SEEK_SET) != 0) {
        fclose(f); error = L"Aucune trame audio AIFF exploitable."; return {};
    }
    const uint32_t rate = static_cast<uint32_t>(std::llround(sampleRate));
    const uint64_t bucketFrames = std::max<uint32_t>(1, rate / 20);
    constexpr size_t chunkFrames = 4096;
    std::vector<unsigned char> samples(chunkFrames * static_cast<size_t>(frameBytes));
    auto wave = std::make_shared<WaveData>();
    harmonic::Probe harmonicProbe(rate);
    RmsAccumulator rms;
    uint64_t frames = 0;
    while (TaskCurrent(task) && frames < totalFrames) {
        const size_t count = static_cast<size_t>(
            std::min<uint64_t>(chunkFrames, totalFrames - frames));
        const size_t got = fread(samples.data(), static_cast<size_t>(frameBytes), count, f);
        if (!got) break;
        for (size_t i = 0; i < got; ++i, ++frames) {
            const size_t bin = static_cast<size_t>(frames / bucketFrames);
            if (bin >= 1000000) { error = L"AIFF trop long pour cette version."; break; }
            if (bin >= wave->peaks.size()) wave->peaks.resize(bin + 1, 0);
            int peak = 0;
            double mixed = 0;
            for (uint32_t ch = 0; ch < channels; ++ch) {
                const unsigned char* p = samples.data() +
                    (i * channels + ch) * bytesPerSample;
                int amplitude = 0;
                if (encoding <= 1) {
                    uint64_t raw = 0;
                    for (uint32_t j = 0; j < bytesPerSample; ++j)
                        raw = encoding == 1 ? (raw | (uint64_t(p[j]) << (j * 8)))
                                            : ((raw << 8) | p[j]);
                    const uint32_t width = bytesPerSample * 8;
                    int64_t signedSample = static_cast<int64_t>(raw);
                    if (raw & (uint64_t(1) << (width - 1)))
                        signedSample -= int64_t(1) << width;
                    const int64_t sample16 = width >= 16 ?
                        (signedSample / (int64_t(1) << (width - 16))) :
                        (signedSample * (int64_t(1) << (16 - width)));
                    amplitude = static_cast<int>(std::min<int64_t>(32767,
                        sample16 < 0 ? -sample16 : sample16));
                    mixed += static_cast<double>(sample16) / 32768.0;
                } else {
                    uint64_t raw = 0;
                    for (uint32_t j = 0; j < bytesPerSample; ++j)
                        raw = (raw << 8) | p[j];
                    double sample = 0;
                    if (encoding == 2) {
                        uint32_t bits32 = static_cast<uint32_t>(raw);
                        float decoded = 0;
                        memcpy(&decoded, &bits32, sizeof(decoded));
                        sample = decoded;
                    } else {
                        memcpy(&sample, &raw, sizeof(sample));
                    }
                    if (std::isfinite(sample)) {
                        amplitude = static_cast<int>(std::min(32767.0,
                            std::fabs(sample) * 32767.0));
                        mixed += sample;
                    }
                }
                peak = std::max(peak, amplitude);
            }
            wave->peaks[bin] = static_cast<uint16_t>(
                std::max<int>(wave->peaks[bin], peak));
            harmonicProbe.Add(static_cast<float>(mixed / channels));
            rms.Add(*wave, bin, mixed / channels);
        }
        if (!error.empty()) break;
        if (!task.playing)
            g_progress.store(static_cast<int>(std::min<uint64_t>(99,
                frames * 100 / std::max<uint64_t>(1, totalFrames))));
        Sleep(0);
    }
    fclose(f);
    if (!TaskCurrent(task) || !error.empty()) return {};
    if (!frames || wave->peaks.empty()) {
        error = L"Lecture des echantillons AIFF impossible."; return {};
    }
    rms.Flush(*wave);
    wave->rms.resize(wave->peaks.size(), 0);
    SetRmsDisplayGain(*wave);
    wave->duration_ms = std::max<uint64_t>(1, frames * 1000 / rate);
    wave->harmonicKey = harmonicProbe.Estimate();
    return wave;
}

// Strictly sequential 16-bit PCM decode. Peak data uses only ~72 KB per 30 minutes.
std::shared_ptr<WaveData> DecodeWithMF(const Task& task, std::wstring& error) {
    IMFSourceReader* reader = nullptr;
    IMFMediaType* desired = nullptr;
    IMFMediaType* actual = nullptr;
    const DWORD stream = MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    std::shared_ptr<WaveData> wave;
    HRESULT hr = S_OK;
    std::wstring stage = L"ouverture du fichier";
    hr = MFCreateSourceReaderFromURL(task.path.c_str(), nullptr, &reader);
    if (SUCCEEDED(hr)) { stage = L"creation du format"; hr = MFCreateMediaType(&desired); }
    if (SUCCEEDED(hr)) { stage = L"type audio"; hr = desired->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio); }
    if (SUCCEEDED(hr)) { stage = L"format PCM"; hr = desired->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM); }
    if (SUCCEEDED(hr)) { stage = L"PCM 16 bits"; hr = desired->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16); }
    if (SUCCEEDED(hr)) { stage = L"activation du decodeur PCM"; hr = reader->SetCurrentMediaType(stream, nullptr, desired); }
    if (SUCCEEDED(hr)) { stage = L"lecture du format PCM"; hr = reader->GetCurrentMediaType(stream, &actual); }
    UINT32 rate = 0, channels = 0, bits = 0;
    if (SUCCEEDED(hr)) { stage = L"frequence audio"; hr = actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate); }
    if (SUCCEEDED(hr)) { stage = L"canaux audio"; hr = actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels); }
    if (SUCCEEDED(hr)) { stage = L"bits audio"; hr = actual->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits); }
    if (FAILED(hr) || rate == 0 || channels == 0 || channels > 32 || bits != 16) {
        wchar_t hrCode[32]{};
        swprintf_s(hrCode, 32, L"0x%08lX", static_cast<unsigned long>(hr));
        error = L"Echec Media Foundation (" + stage + L", " + hrCode + L").";
    } else {
        wave = std::make_shared<WaveData>();
        harmonic::Probe harmonicProbe(static_cast<int>(rate));
    RmsAccumulator rms;
        const uint64_t bucketFrames = std::max<UINT32>(1, rate / 20); // ~50 ms
        uint64_t frameCount = 0;
        uint64_t estimatedFrames = 0;
        PROPVARIANT prop;
        PropVariantInit(&prop);
        if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &prop)) &&
            prop.vt == VT_UI8) estimatedFrames = prop.uhVal.QuadPart * rate / 10000000u;
        PropVariantClear(&prop);
        bool finished = false;
        while (!finished && TaskCurrent(task)) {
            DWORD flags = 0;
            IMFSample* sample = nullptr;
            hr = reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample);
            if (FAILED(hr)) {
                error = L"Erreur de lecture audio pendant l'analyse.";
                if (sample) sample->Release();
                wave.reset();
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) finished = true;
            if (sample) {
                IMFMediaBuffer* buffer = nullptr;
                hr = sample->ConvertToContiguousBuffer(&buffer);
                if (SUCCEEDED(hr) && buffer) {
                    BYTE* bytes = nullptr;
                    DWORD maxLength = 0, length = 0;
                    hr = buffer->Lock(&bytes, &maxLength, &length);
                    if (SUCCEEDED(hr)) {
                        const auto* pcm = reinterpret_cast<const int16_t*>(bytes);
                        const uint64_t frames = length / (sizeof(int16_t) * channels);
                        for (uint64_t i = 0; i < frames; ++i, ++frameCount) {
                            size_t bin = static_cast<size_t>(frameCount / bucketFrames);
                            if (bin >= 1000000) { error = L"Morceau trop long pour cette version."; break; }
                            if (bin >= wave->peaks.size()) wave->peaks.resize(bin + 1, 0);
                            int maximum = 0;
                            double mixed = 0;
                            for (UINT32 ch = 0; ch < channels; ++ch) {
                                const int sample = static_cast<int>(pcm[i * channels + ch]);
                                maximum = std::max(maximum, sample < 0 ? -sample : sample);
                                mixed += static_cast<double>(sample);
                            }
                            wave->peaks[bin] = static_cast<uint16_t>(
                                std::max<int>(wave->peaks[bin], std::min(maximum, 32767)));
                            harmonicProbe.Add(static_cast<float>(mixed / (32768.0 * channels)));
            rms.Add(*wave, bin, mixed / (32768.0 * channels));
                        }
                        buffer->Unlock();
                        if (!error.empty()) wave.reset();
                    }
                    buffer->Release();
                }
                sample->Release();
                if (FAILED(hr)) { error = L"Erreur de conversion PCM."; wave.reset(); break; }
                if (!wave) break;
            }
            if (estimatedFrames > 0) {
                const int pct = static_cast<int>(std::min<uint64_t>(99, (frameCount / std::max<uint64_t>(1, estimatedFrames / 100))));
                if (!task.playing) g_progress.store(pct);
            }
            // Yield occasionally to avoid competing aggressively with Winamp's audio thread.
            Sleep(0);
        }
        if (!TaskCurrent(task)) wave.reset();
        if (wave && frameCount && !wave->peaks.empty()) {
            rms.Flush(*wave);
    wave->rms.resize(wave->peaks.size(), 0);
    SetRmsDisplayGain(*wave);
    wave->duration_ms = std::max<uint64_t>(1, frameCount * 1000 / rate);
            wave->harmonicKey = harmonicProbe.Estimate();
        }
        else if (wave) { error = L"Aucune donnee audio exploitable."; wave.reset(); }
    }
    if (actual) actual->Release();
    if (desired) desired->Release();
    if (reader) reader->Release();
    return wave;
}

void WorkerMain() {
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comReady = SUCCEEDED(comHr);
    const HRESULT mfHr = comReady ? MFStartup(MF_VERSION, MFSTARTUP_LITE) : E_FAIL;
    const bool mfReady = SUCCEEDED(mfHr);
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(g_taskMutex);
            g_taskWake.wait(lk, [] { return g_workerStop || g_pendingTask != nullptr ||
                g_pendingPlayingTask != nullptr || !g_batchTasks.empty(); });
            if (g_workerStop) break;
            if (g_pendingTask) {
                task = *g_pendingTask; g_pendingTask.reset();
                g_batchPreempt.store(false);
            } else if (g_pendingPlayingTask) {
                task = *g_pendingPlayingTask; g_pendingPlayingTask.reset();
                g_batchPreempt.store(false);
            } else {
                task = g_batchTasks.front(); g_batchTasks.pop_front();
                g_batchPreempt.store(false);
            }
        }
        auto result = std::make_unique<Result>();
        result->task = task;
        const auto hasExt = [&](const wchar_t* ext) {
            const size_t n = wcslen(ext);
            return task.path.size() >= n &&
                _wcsicmp(task.path.c_str() + task.path.size() - n, ext) == 0;
        };
        if (hasExt(L".flac")) result->wave = DecodeFlac(task, result->error);
        else if (hasExt(L".ogg") || hasExt(L".oga"))
            result->wave = DecodeOgg(task, result->error);
        else if (hasExt(L".aif") || hasExt(L".aiff") || hasExt(L".aifc"))
            result->wave = DecodeAiff(task, result->error);
        else if (mfReady) result->wave = DecodeWithMF(task, result->error);
        else result->error = L"Windows Media Foundation n'est pas disponible.";
        if (!TaskCurrent(task)) {
            if (task.batch && task.generation == g_batchGeneration.load() &&
                g_batchPreempt.load()) {
                std::lock_guard<std::mutex> lk(g_taskMutex);
                g_batchTasks.push_front(task);
            }
            continue;
        }
        if (result->wave) {
            // BTrack class ABI is consistent across the plugin and static library.
            // Run the experimental BPM estimator after decoding on the worker thread.
            result->wave->bpm = EstimateBpmFromRms(*result->wave, task);
            result->wave->bpmDouble = ReadBpmChoice(task.path);
            if (TaskCurrent(task)) SaveCache(task, *result->wave);
        }
        if (!TaskCurrent(task)) {
            if (task.batch && task.generation == g_batchGeneration.load() &&
                g_batchPreempt.load()) {
                std::lock_guard<std::mutex> lk(g_taskMutex);
                g_batchTasks.push_front(task);
            }
            continue;
        }
        if (task.batch) {
            ++g_batchCompleted;
        } else {
            std::lock_guard<std::mutex> lk(g_taskMutex);
            if (task.playing) g_completedPlaying = std::move(result);
            else g_completed = std::move(result);
        }
        if (HWND target = g_noticeHwnd.load()) PostMessageW(target, WM_WAVE_RESULT, 0, 0);
    }
    if (mfReady) MFShutdown();
    if (comReady) CoUninitialize();
}

std::wstring PlaylistPath(int index) {
    if (index < 0 || !g_winamp || !IsWindow(g_winamp)) return L"";
    auto p = SendMessageW(g_winamp, WM_WA_IPC, static_cast<WPARAM>(index), IPC_GETPLAYLISTFILEW);
    if (p > 1) return reinterpret_cast<const wchar_t*>(p);
    auto narrow = SendMessageW(g_winamp, WM_WA_IPC, static_cast<WPARAM>(index), IPC_GETPLAYLISTFILE);
    if (narrow <= 1) return L"";
    auto s = reinterpret_cast<const char*>(narrow);
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &wide[0], n);
    wide.pop_back();
    return wide;
}

void CancelAnalysis() {
    ++g_generation;
    std::lock_guard<std::mutex> lk(g_taskMutex);
    g_pendingTask.reset();
    g_completed.reset();
    g_isLoading = false;
    g_progress = 0;
}

void Layout(int width, int height);
void RequestLoad();
void UpdatePropertiesProgress();

void RefreshButtonLayout() {
    if (!g_hwnd || !IsWindow(g_hwnd)) return;
    RECT client{};
    if (GetClientRect(g_hwnd, &client))
        Layout(client.right - client.left, client.bottom - client.top);
}

// Share one decoded waveform whenever the selected file is also playing.
// This is a UI-thread pointer handoff, not a second analysis or a seek.
void SyncSelectedWaveToPlaying() {
    if (!g_wave || g_selectedPath.empty() || g_playingPath.empty() ||
        _wcsicmp(g_selectedPath.c_str(), g_playingPath.c_str()) != 0) return;
    g_playingWave = g_wave;
    if (g_hwnd) InvalidateRect(g_hwnd, &g_playingPlot, FALSE);
}

void ChangeSelected(int index, const std::wstring& path) {
    if (g_selectedIndex == index && g_selectedPath == path) return;
    CancelAnalysis();
    g_selectedIndex = index;
    g_selectedPath = path;
    g_cues = LoadCues(path);
    g_wave.reset();
    g_mode = ModeNone;
    g_marker = MarkerNone;
    g_markerIndex = -1;
    g_dragging = false;
    if (path.empty()) {
        g_status = L"Selectionnez un titre dans le Playlist Editor.";
    } else {
        // Playlist browsing is read-only: show previously analysed waveform
        // and Camelot key immediately, without starting a new decoder task.
        FileStamp stamp{};
        if (ReadFileStamp(path, stamp)) {
            Task cacheTask{path, stamp, g_generation.load(), false};
            g_wave = LoadCache(cacheTask);
        }
        if (g_wave) {
            SyncSelectedWaveToPlaying();
            g_status = g_wave->harmonicKey >= -1
                ? L"Waveform et tonalite lues depuis le cache."
                : L"Waveform en cache. Cliquez sur Load pour calculer la tonalite.";
        } else {
            g_status = L"Waveform vide. Cliquez sur Show/Load Waveform.";
        }
    }
    RefreshButtonLayout();
    InvalidateRect(g_hwnd, nullptr, FALSE);
    if (g_autoAnalyzeSelection && !g_wave && !g_selectedPath.empty())
        RequestLoad();
}

void RequestLoad() {
    if (g_selectedPath.empty()) return;
    FileStamp stamp{};
    if (!ReadFileStamp(g_selectedPath, stamp)) {
        g_status = L"Fichier local introuvable (les streams ne sont pas pris en charge).";
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    Task task{g_selectedPath, stamp, ++g_generation};
    if (auto cached = LoadCache(task)) {
        const bool keyReady = cached->harmonicKey >= -1;
        g_wave = std::move(cached);
        SyncSelectedWaveToPlaying();
        if (keyReady) {
            g_isLoading = false;
            g_status = L"Waveform et tonalite lues depuis le cache.";
            RefreshButtonLayout();
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return;
        }
        // Existing waveform remains visible while user-requested analysis runs.
        g_status = L"Waveform en cache. Detection harmonique experimentale...";
    } else g_wave.reset();
    g_progress = 0;
    g_isLoading = true;
    g_batchPreempt.store(true);
    if (!g_wave) g_status = L"Analyse audio et detection harmonique en cours...";
    {
        std::lock_guard<std::mutex> lk(g_taskMutex);
        g_pendingTask = std::make_unique<Task>(std::move(task));
    }
    g_taskWake.notify_one();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void AcceptWorkerResult() {
    UpdatePropertiesProgress();
    std::unique_ptr<Result> playingDone;
    {
        std::lock_guard<std::mutex> lk(g_taskMutex);
        playingDone = std::move(g_completedPlaying);
    }
    if (playingDone && playingDone->task.generation == g_playGeneration.load() &&
        _wcsicmp(playingDone->task.path.c_str(), g_playingPath.c_str()) == 0) {
        g_playingLoading = false;
        g_playingWave = std::move(playingDone->wave);
        InvalidateRect(g_hwnd, &g_playingPlot, FALSE);
    }
    std::unique_ptr<Result> done;
    {
        std::lock_guard<std::mutex> lk(g_taskMutex);
        done = std::move(g_completed);
    }
    if (!done || !TaskCurrent(done->task) || done->task.path != g_selectedPath) return;
    g_isLoading = false;
    g_wave = std::move(done->wave);
    SyncSelectedWaveToPlaying();
    RefreshButtonLayout();
    g_status = g_wave ? L"Waveform prete. Tonalite experimentale : " + std::wstring(harmonic::Camelot(g_wave->harmonicKey))
                      : (done->error.empty() ? L"Impossible d'analyser ce fichier." : done->error);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

int64_t PlotDuration() {
    return g_wave ? static_cast<int64_t>(g_wave->duration_ms) : 0;
}

int XAt(int64_t ms) {
    const int w = std::max(1L, g_plot.right - g_plot.left - 20L);
    int64_t len = std::max<int64_t>(1, PlotDuration());
    return g_plot.left + 10 + static_cast<int>((wavecue::clamp_ms(ms, len) * w) / len);
}

int64_t MsAt(int x) {
    const int w = std::max(1L, g_plot.right - g_plot.left - 20L);
    int px = std::clamp<int>(x - g_plot.left - 10, 0, w);
    return static_cast<int64_t>(px) * PlotDuration() / w;
}

bool IsInPlot(POINT pt) {
    return PtInRect(&g_plot, pt) != 0;
}

void SeekPlaying(int64_t ms) {
    // Never interrupt the currently playing track to preview a different selection.
    if (!g_winamp || g_selectedPath != g_playingPath || g_playingIndex < 0) return;
    int state = static_cast<int>(SendMessageW(g_winamp, WM_WA_IPC, 0, IPC_ISPLAYING));
    if (state == 1 || state == 3)
        SendMessageW(g_winamp, WM_WA_IPC, static_cast<WPARAM>(ms), IPC_JUMPTOTIME);
}

void ResetPlayingWave() {
    ++g_playGeneration;
    g_playingWave.reset();
    g_playingLoading = false;
    {
        std::lock_guard<std::mutex> lk(g_taskMutex);
        g_pendingPlayingTask.reset();
        g_completedPlaying.reset();
    }
}

void RequestPlayingLoad() {
    if (g_playingPath.empty() || g_playingIndex < 0 || g_playingLoading) return;
    const std::wstring path = g_playingPath;
    // Sharing an already-loaded selection avoids decoding twice, but only
    // after the user explicitly clicks the playing waveform's Load button.
    if (_wcsicmp(path.c_str(), g_selectedPath.c_str()) == 0 && g_wave) {
        g_playingWave = g_wave;
        InvalidateRect(g_hwnd, &g_playingPlot, FALSE);
        if (g_playingWave->harmonicKey >= -1) return;
        // An old cached waveform can still need harmonic analysis.
    }
    FileStamp stamp{};
    if (!ReadFileStamp(path, stamp)) return;
    Task task{path, stamp, g_playGeneration.load(), true};
    if (auto cached = LoadCache(task)) {
        g_playingWave = std::move(cached);
        InvalidateRect(g_hwnd, &g_playingPlot, FALSE);
        if (g_playingWave->harmonicKey >= -1) return;
    }
    // Selected Track may already be decoding this exact file.
    // Its completed waveform will be shared with Actually Playing.
    if (g_isLoading && _wcsicmp(path.c_str(), g_selectedPath.c_str()) == 0)
        return;
    g_playingLoading = true;
    g_batchPreempt.store(true);
    {
        std::lock_guard<std::mutex> lk(g_taskMutex);
        g_pendingPlayingTask = std::make_unique<Task>(std::move(task));
    }
    g_taskWake.notify_one();
    InvalidateRect(g_hwnd, &g_playingPlot, FALSE);
}

void PollWinamp() {
    if (!g_winamp || !IsWindow(g_winamp)) return;
    const int playing = static_cast<int>(SendMessageW(g_winamp, WM_WA_IPC, 0, IPC_GETLISTPOS));
    const std::wstring playPath = playing >= 0 ? PlaylistPath(playing) : L"";
    const int state = static_cast<int>(SendMessageW(g_winamp, WM_WA_IPC, 0, IPC_ISPLAYING));
    const bool active = state == 1 || state == 3;
    // The playlist position can change when items are reordered while the SAME
    // audio file continues playing. A new index is not a new playback event.
    const bool fileChanged = _wcsicmp(g_playingPath.c_str(), playPath.c_str()) != 0;
    const int64_t observedMs = active
        ? static_cast<int64_t>(SendMessageW(g_winamp, WM_WA_IPC, 0, IPC_GETOUTPUTTIME)) : -1;
    g_playingIndex = playing; // Always keep the current index for Seek.
    if (fileChanged) {
        g_playingPath = playPath;
        g_playbackCues = LoadCues(playPath);
        ResetPlayingWave();
        // Reuse cache first, then optionally decode the playing track when
        // Auto Load is enabled. Playback and IN/OUT are unaffected.
        if (!playPath.empty()) {
            if (_wcsicmp(playPath.c_str(), g_selectedPath.c_str()) == 0 && g_wave) {
                g_playingWave = g_wave;
            } else {
                FileStamp stamp{};
                if (ReadFileStamp(playPath, stamp)) {
                    Task cacheTask{playPath, stamp, g_playGeneration.load(), true};
                    g_playingWave = LoadCache(cacheTask);
                }
            }
        }
        if (g_hwnd) {
            RefreshButtonLayout();
            InvalidateRect(g_hwnd, nullptr, FALSE);
        }
        if (g_autoAnalyzeSelection && !playPath.empty() && !g_playingWave)
            RequestPlayingLoad();
        g_skipSent = false;
        // The first notification may arrive before the decoder is ready or while
        // playback is still stopped/paused. Keep IN pending until it can be applied.
        g_startPending = !playPath.empty();
    } else if (g_wasStopped && state == 1 && !playPath.empty()) {
        // Restarting the same track after Stop is a fresh playback event.
        g_startPending = true;
        g_skipSent = false;
    }
    // One-time SWITCH position takes precedence over automatic IN.
    if (g_switchPendingMs >= 0) {
        if (GetTickCount64() > g_switchPendingUntil) {
            g_switchPendingMs = -1;
            g_switchPendingPath.clear();
        } else if (state == 1 && observedMs >= 0 &&
                   _wcsicmp(playPath.c_str(), g_switchPendingPath.c_str()) == 0) {
            const int64_t targetMs = g_switchPendingMs;
            g_switchPendingMs = -1;
            g_switchPendingPath.clear();
            g_startPending = false;
            SendMessageW(g_winamp, WM_WA_IPC,
                         static_cast<WPARAM>(targetMs), IPC_JUMPTOTIME);
            // Prevent an immediate OUT skip when entering beyond an OUT cue.
            g_skipSent = g_playbackCues.end_ms >= 0 &&
                         targetMs >= g_playbackCues.end_ms;
        }
    }
    if (g_startPending && state == 1 && observedMs >= 0) {
        g_startPending = false;
        // Never jump backwards if the plug-in was opened mid-track, or if the
        // current playlist entry merely moved to another position.
        if (g_masterAuto && observedMs <= 2000 && g_playbackCues.auto_apply &&
            g_playbackCues.start_ms > 0) {
            SendMessageW(g_winamp, WM_WA_IPC,
                         static_cast<WPARAM>(g_playbackCues.start_ms), IPC_JUMPTOTIME);
        }
    }
    g_wasStopped = (state == 0);
    g_playheadMs = observedMs;
    // Bounded wall-clock history; supports live delay changes, pauses, seeks
    // and boundaries between different playlist tracks.
    const ULONGLONG stamp = GetTickCount64();
    if (active && observedMs >= 0 && !playPath.empty() &&
        (g_recentPlayback.empty() || stamp - g_recentPlayback.back().tick >= 70))
        g_recentPlayback.push_back({stamp, playPath, observedMs});
    while (!g_recentPlayback.empty() && stamp - g_recentPlayback.front().tick > 65000)
        g_recentPlayback.pop_front();
    UpdateDelayedPlayhead();
    if (g_masterAuto && g_playbackCues.auto_apply && state == 1 && g_playbackCues.end_ms >= 0 && !g_skipSent &&
        g_playheadMs >= g_playbackCues.end_ms) {
        g_skipSent = true;
        PostMessageW(g_winamp, WM_COMMAND, WINAMP_BUTTON5, 0);
    }
    if (!active) g_skipSent = false;
}

// Winamp sends playback-status notifications immediately rather than waiting
// for our 170 ms display timer. Keep timer polling as a fallback.
LRESULT CALLBACK WinampEventProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp,
                                  UINT_PTR subclassId, DWORD_PTR refData) {
    const bool playbackEvent = message == WM_WA_IPC &&
        lp == IPC_CB_MISC && wp == IPC_CB_MISC_STATUS;
    LRESULT result = DefSubclassProc(hwnd, message, wp, lp);
    if (playbackEvent && g_hwnd && IsWindow(g_hwnd)) PollWinamp();
    return result;
}

void SelectPlaylistItem() {
    if (!g_winamp || !IsWindow(g_winamp)) return;
    int idx = static_cast<int>(SendMessageW(g_winamp, WM_WA_IPC,
                                            static_cast<WPARAM>(-1), IPC_PLAYLIST_GET_NEXT_SELECTED));
    if (idx < 0) ChangeSelected(-1, L"");
    else ChangeSelected(idx, PlaylistPath(idx));
}

} // anonymous namespace

namespace {

bool EqualPaths(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

void RectFill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void RectOutline(HDC dc, RECT rect, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void Line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int thickness = 1) {
    HPEN pen = CreatePen(PS_SOLID, thickness, color);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void Text(HDC dc, const std::wstring& value, RECT rect, COLORREF color, UINT flags,
          HFONT font = nullptr) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ prev = SelectObject(dc, font ? font : g_font);
    DrawTextW(dc, value.c_str(), -1, &rect, flags | DT_NOPREFIX);
    SelectObject(dc, prev);
}

int ButtonUnder(POINT pt) {
    for (const auto& b : g_buttons)
        if (b.enabled && PtInRect(&b.bounds, pt)) return b.id;
    return 0;
}

void Layout(int width, int height) {
    // One compact row for LOAD, SEEK, IN and OUT, even at minimum window width.
    const int left = 22, right = std::max(left + 280, width - left);
    g_playingPlot = {left, 80, right, 203};
    g_buttons.clear();
    const int masterLeft = std::max(left + 445, right - 153);
    const int delayLeft = left + 143;
    g_delaySlider = {delayLeft + 124, 13,
        std::max(delayLeft + 155, masterLeft - 84), 31};
    g_buttons.push_back({BUTTON_STREAM_DELAY,
        {delayLeft, 9, delayLeft + 117, 36},
        g_streamDelayEnabled ? L"DELAY ON" : L"DELAY OFF", true});
    g_buttons.push_back({BUTTON_MASTER_AUTO,
        {masterLeft, 9, masterLeft + 153, 36},
        g_masterAuto ? L"WAVECUE AUTO ON" : L"WAVECUE AUTO OFF", true});
    const int autoLeft = right - 121, seekLeft = autoLeft - 79, loadLeft = seekLeft - 160;
    g_buttons.push_back({BUTTON_PLAYING_LOAD, {loadLeft, 45, loadLeft + 153, 73},
        L"LOAD WAVEFORM", !g_playingPath.empty() && g_playingIndex >= 0});
    g_buttons.push_back({BUTTON_SEEK, {seekLeft, 45, seekLeft + 72, 73},
        L"SEEK", !g_playingPath.empty() && !!g_playingWave && g_playheadMs >= 0});
    g_buttons.push_back({BUTTON_PLAYING_AUTO, {autoLeft, 45, autoLeft + 121, 73},
        L"AUTO IN/OUT", !g_playingPath.empty() && g_playingIndex >= 0});
    const struct { int id, w; const wchar_t* label; } specs[] = {
        {BUTTON_LOAD, 163, L"LOAD WAVEFORM"}, {BUTTON_SWITCH, 72, L"SWITCH"},
        {BUTTON_START, 82, L"SET IN"}, {BUTTON_END, 82, L"SET OUT"},
        {BUTTON_DELETE, 80, L"DELETE"}, {BUTTON_AUTO, 125, L"AUTO IN/OUT"},
    };
    int x = left;
    for (const auto& b : specs) {
        g_buttons.push_back({b.id, {x, 281, x + b.w, 309},
            b.label, (b.id == BUTTON_LOAD || b.id == BUTTON_AUTO)
                ? !g_selectedPath.empty() : !g_selectedPath.empty() && !!g_wave});
        x += b.w + 7;
    }
    g_plot = {left, 322, right, std::max(426, height - 82)};
}

bool ButtonActive(const Button& b) {
    return (b.id == BUTTON_MASTER_AUTO && g_masterAuto) ||
        (b.id == BUTTON_STREAM_DELAY && g_streamDelayEnabled) ||
        (b.id == BUTTON_SEEK && g_mode == ModeSeek) ||
        (b.id == BUTTON_SWITCH && g_mode == ModeSwitch) ||
        (b.id == BUTTON_START && g_mode == ModeStart) ||
        (b.id == BUTTON_END && g_mode == ModeEnd) ||
        (b.id == BUTTON_DELETE && g_mode == ModeDelete) ||
        (b.id == BUTTON_AUTO && g_cues.auto_apply) ||
        (b.id == BUTTON_PLAYING_AUTO && g_playbackCues.auto_apply);
}
COLORREF ButtonColor(const Button& b) {
    if (!b.enabled) return RGB(29, 29, 40);
    if (ButtonActive(b)) return RGB(53, 76, 53);
    return b.id == g_hover ? RGB(75, 70, 90) : RGB(53, 51, 68);
}

void DrawDelaySlider(HDC dc) {
    // Compact 0–60 second slider, adjustable without stopping the stream.
    RECT rail{g_delaySlider.left, 26, g_delaySlider.right, 30};
    RectFill(dc, rail, BORDER);
    const int width = std::max<int>(1, rail.right - rail.left);
    const int x = rail.left + g_streamDelaySeconds * width / 60;
    RECT used{rail.left, rail.top, x, rail.bottom};
    RectFill(dc, used, g_streamDelayEnabled ? LCD : MUTED);
    RECT thumb{x - 4, 19, x + 5, 37};
    RectFill(dc, thumb, g_streamDelayEnabled ? LCD : TEXT);
    RECT readout{g_delaySlider.right + 9, 13, g_delaySlider.right + 78, 44};
    Text(dc, std::to_wstring(g_streamDelaySeconds) + L" s", readout,
         g_streamDelayEnabled ? LCD : MUTED,
         DT_SINGLELINE | DT_VCENTER);
}

bool InDelaySlider(POINT pt) {
    RECT rect = g_delaySlider;
    rect.top = 10;
    rect.bottom = 46;
    return PtInRect(&rect, pt) != 0;
}

void SetDelayFromX(int x) {
    const int width = std::max<int>(1, g_delaySlider.right - g_delaySlider.left);
    const int distance = std::clamp<int>(x - g_delaySlider.left, 0, width);
    g_streamDelaySeconds = std::clamp((distance * 60 + width / 2) / width, 0, 60);
    UpdateDelayedPlayhead();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void DrawButtons(HDC dc) {
    // Restrained raised/sunken beveled buttons from the classic Winamp era.
    for (const auto& b : g_buttons) {
        RectFill(dc, b.bounds, ButtonColor(b));
        const bool pressed = b.enabled && ButtonActive(b);
        COLORREF light = pressed ? RGB(20, 22, 30) : RGB(123, 119, 142);
        COLORREF dark = pressed ? RGB(122, 129, 112) : RGB(16, 17, 25);
        Line(dc, b.bounds.left, b.bounds.bottom - 1, b.bounds.left, b.bounds.top, light);
        Line(dc, b.bounds.left, b.bounds.top, b.bounds.right - 1, b.bounds.top, light);
        Line(dc, b.bounds.right - 1, b.bounds.top, b.bounds.right - 1, b.bounds.bottom - 1, dark);
        Line(dc, b.bounds.right - 1, b.bounds.bottom - 1, b.bounds.left, b.bounds.bottom - 1, dark);
        RECT label = b.bounds;
        if (pressed) OffsetRect(&label, 1, 1);
        Text(dc, b.text, label, !b.enabled ? RGB(94, 93, 113) :
             pressed ? LCD : TEXT, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }
}

void DrawMarker(HDC dc, int64_t ms, COLORREF color, const wchar_t* label, bool large = true) {
    if (ms < 0 || !g_wave) return;
    int x = XAt(ms);
    Line(dc, x, g_plot.top + 2, x, g_plot.bottom - 2, color, large ? 2 : 1);
    RECT tag{x + 3, g_plot.top + 4, x + 74, g_plot.top + 24};
    if (tag.right > g_plot.right) { tag.left = x - 73; tag.right = x - 2; }
    Text(dc, label, tag, color, DT_LEFT | DT_SINGLELINE);
}

HBRUSH GetLcdDotBrush(HDC dc) {
    if (g_lcdDotBrush) return g_lcdDotBrush;
    // Draw only ONE tile, then let GDI repeat it in hardware-optimized FillRect.
    HDC tileDc = CreateCompatibleDC(dc);
    if (!tileDc) return nullptr;
    HBITMAP tile = CreateCompatibleBitmap(dc, LCD_DOT_PITCH, LCD_DOT_PITCH);
    if (tile) {
        HGDIOBJ old = SelectObject(tileDc, tile);
        RECT tileRect{0, 0, LCD_DOT_PITCH, LCD_DOT_PITCH};
        HBRUSH background = CreateSolidBrush(LCD_BG);
        FillRect(tileDc, &tileRect, background);
        DeleteObject(background);
        SetPixelV(tileDc, 2, 2, LCD_DOT);
        SelectObject(tileDc, old);
        g_lcdDotBrush = CreatePatternBrush(tile);
        DeleteObject(tile);
    }
    DeleteDC(tileDc);
    return g_lcdDotBrush;
}

void DrawWave(HDC dc) {
    // Cached neutral LCD dots over pure black, with original cyan RMS envelope.
    RectFill(dc, g_plot, LCD_BG);
    RectOutline(dc, g_plot, LCD_FRAME);
    RECT inner = g_plot;
    InflateRect(&inner, -10, -9);
    const int cy = (inner.top + inner.bottom) / 2;
    const COLORREF edge = RGB(94, 97, 109);
    // GDI repeats a tiny bitmap across both waveform windows in one call.
    if (HBRUSH dots = GetLcdDotBrush(dc)) FillRect(dc, &inner, dots);
    // Recessed outer edge with short calibrated ticks, like Winamp's EQ.
    Line(dc, g_plot.left + 2, g_plot.top + 2, g_plot.right - 3, g_plot.top + 2,
         RGB(63, 66, 76));
    Line(dc, g_plot.left + 2, g_plot.bottom - 3,
         g_plot.right - 3, g_plot.bottom - 3, RGB(63, 66, 76));
    // Keep only the calibrated frame ticks; the background grid is dotted.
    for (int x = inner.left + 35; x < inner.right; x += 35) {
        Line(dc, x, g_plot.top + 2, x, g_plot.top + 6, edge);
        Line(dc, x, g_plot.bottom - 7, x, g_plot.bottom - 3, edge);
    }
    for (int y = g_plot.top + 12; y < g_plot.bottom - 8; y += 16) {
        Line(dc, g_plot.left + 2, y, g_plot.left + 6, y, edge);
        Line(dc, g_plot.right - 7, y, g_plot.right - 3, y, edge);
    }
    Line(dc, inner.left, cy, inner.right, cy, RGB(35, 38, 45));
    if (!g_wave || g_wave->rms.empty()) {
        std::wstring msg = g_isLoading
            ? L"ANALYSE EN COURS : " + std::to_wstring(g_progress.load()) + L" %"
            : (g_status.find(L"Echec") != std::wstring::npos ||
               g_status.find(L"Erreur") != std::wstring::npos ||
               g_status.find(L"indisponible") != std::wstring::npos ||
               g_status.find(L"Impossible") != std::wstring::npos ||
               g_status.find(L"introuvable") != std::wstring::npos ||
               g_status.find(L"Aucune") != std::wstring::npos)
                  ? g_status : L"WAVEFORM NON CHARGEE";
        Text(dc, msg, inner, MUTED, DT_CENTER | DT_WORDBREAK | DT_VCENTER, g_bigFont);
        if (g_isLoading) {
            RECT bar{inner.left + 20, cy + 45, inner.right - 20, cy + 51};
            RectFill(dc, bar, BORDER);
            bar.right = bar.left + (bar.right - bar.left) * g_progress.load() / 100;
            RectFill(dc, bar, ACCENT);
        }
        return;
    }
    const int innerW = std::max<int>(1, inner.right - inner.left);
    const int half = std::max<int>(1, (inner.bottom - inner.top) / 2 - 7);
    const size_t n = g_wave->rms.size();
    // RMS-only envelope; no extra audio work during the repaint.
    HPEN pen = CreatePen(PS_SOLID, 1, LCD_WAVE);
    HGDIOBJ previous = SelectObject(dc, pen);
    for (int x = 0; x < innerW; ++x) {
        size_t a = static_cast<size_t>((static_cast<uint64_t>(x) * n) / innerW);
        size_t b = std::min(n, static_cast<size_t>(
            (static_cast<uint64_t>(x + 1) * n) / innerW));
        b = std::max(b, a + 1);
        uint16_t rmsValue = 0;
        for (size_t i = a; i < b; ++i) rmsValue = std::max(rmsValue, g_wave->rms[i]);
        const int amp = std::max(1, std::min(half, static_cast<int>(
            (static_cast<double>(rmsValue) * half * g_wave->rmsDisplayGain) / 32768.0)));
        MoveToEx(dc, inner.left + x, cy - amp, nullptr);
        LineTo(dc, inner.left + x, cy + amp + 1);
    }
    SelectObject(dc, previous);
    DeleteObject(pen);

    if (g_wave->bpm >= 80) {
        RECT bpmRect{g_plot.left + 10, g_plot.top + 8,
                     g_plot.left + 74, g_plot.top + 39};
        RectFill(dc, bpmRect, RGB(22, 23, 29));
        // BTrack estimates 80-160 BPM. On click, show the other musically
        // plausible interpretation: half-time for 110+, double-time below.
        // Represent odd half-tempos exactly (e.g. 135 -> 67.5).
        std::wstring tempoLabel = std::to_wstring(g_wave->bpm);
        if (g_wave->bpmDouble) {
            if (g_wave->bpm >= 110) {
                tempoLabel = std::to_wstring(g_wave->bpm / 2);
                if (g_wave->bpm % 2) tempoLabel += L".5";
            } else {
                tempoLabel = std::to_wstring(g_wave->bpm * 2);
            }
        }
        Text(dc, tempoLabel, bpmRect,
             RGB(239, 216, 105), DT_CENTER | DT_VCENTER | DT_SINGLELINE, g_bigFont);
    }
    if (g_wave->harmonicKey >= 0) {
        RECT keyRect{g_plot.right - 85, g_plot.top + 8, g_plot.right - 10, g_plot.top + 39};
        RectFill(dc, keyRect, RGB(22, 23, 29));
        Text(dc, harmonic::Camelot(g_wave->harmonicKey), keyRect, RGB(239, 216, 105),
             DT_CENTER | DT_VCENTER | DT_SINGLELINE, g_bigFont);
    }
    if (g_cues.start_ms >= 0)
        DrawMarker(dc, g_cues.start_ms, START, L"IN");
    if (g_cues.end_ms >= 0)
        DrawMarker(dc, g_cues.end_ms, END, L"OUT");
    for (size_t i = 0; i < g_cues.marks.size(); ++i) {
        int x = XAt(g_cues.marks[i]);
        Line(dc, x, inner.top, x, inner.bottom, CUE);
        RECT tag{x + 2, inner.bottom - 22, x + 48, inner.bottom - 2};
        Text(dc, std::to_wstring(i + 1), tag, CUE, DT_SINGLELINE);
    }
    // Only show playback position for the file currently playing in Winamp.
    if (g_playheadMs >= 0 && EqualPaths(g_selectedPath, g_playingPath)) {
        int x = XAt(g_playheadMs);
        Line(dc, x, g_plot.top + 1, x, g_plot.bottom - 1, RGB(246, 246, 248), 2);
    }
    RECT base{inner.left, g_plot.bottom + 10, inner.right, g_plot.bottom + 34};
    Text(dc, L"0:00", base, MUTED, DT_SINGLELINE | DT_LEFT);
    Text(dc, TimeLabel(PlotDuration()), base, MUTED, DT_SINGLELINE | DT_RIGHT);
}

void DrawPlayingWave(HDC dc) {
    // The renderer is shared with the editing panel; restore all UI state
    // before processing any further input, so only the lower panel is editable.
    auto oldPlot = g_plot;
    auto oldWave = g_wave;
    auto oldCues = g_cues;
    auto oldSelectedPath = g_selectedPath;
    bool oldLoading = g_isLoading;
    g_plot = g_playingPlot;
    g_wave = g_playingWave;
    g_cues = g_playbackCues;
    g_selectedPath = g_playingPath;
    g_isLoading = false;
    DrawWave(dc);
    if (g_streamDelayEnabled) {
        if (g_delayedPlayhead >= 0 && g_wave && g_wave->duration_ms > 0) {
            const int width = std::max(1L, g_plot.right - g_plot.left - 20L);
            const int x = g_plot.left + 10 + static_cast<int>(
                wavecue::clamp_ms(g_delayedPlayhead,
                    static_cast<int64_t>(g_wave->duration_ms)) * width /
                    static_cast<int64_t>(g_wave->duration_ms));
            Line(dc, x, g_plot.top + 1, x, g_plot.bottom - 1,
                 RGB(242, 79, 91), 2);
            RECT label{x + 5, g_plot.bottom - 26, x + 90, g_plot.bottom - 3};
            Text(dc, L"STREAM", label, RGB(242, 79, 91), DT_SINGLELINE);
        } else if (g_delayedOnPreviousTrack) {
            RECT msg{g_plot.left + 14, g_plot.top + 9,
                     g_plot.right - 10, g_plot.top + 34};
            Text(dc, L"STREAM : morceau precedent", msg, RGB(242, 79, 91),
                 DT_RIGHT | DT_SINGLELINE);
        }
    }
    if (!g_wave && g_playingLoading) {
        RECT msg = g_playingPlot;
        Text(dc, L"ANALYSE DU MORCEAU EN LECTURE...", msg, ACCENT,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    g_plot = oldPlot;
    g_wave = oldWave;
    g_cues = oldCues;
    g_selectedPath = oldSelectedPath;
    g_isLoading = oldLoading;
}

void Paint(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC target = BeginPaint(hwnd, &ps);
    RECT full{};
    GetClientRect(hwnd, &full);
    int w = std::max(1L, full.right - full.left), h = std::max(1L, full.bottom - full.top);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, w, h);
    HGDIOBJ previousBmp = SelectObject(dc, bitmap);
    RectFill(dc, full, BG);
    RECT toolbar{0, 0, w, 41};
    RectFill(dc, toolbar, PANEL);
    Line(dc, 0, 40, w, 40, BORDER);
    RECT title{22, 8, 170, 36};
    Text(dc, L"WAVECUE", title, LCD, DT_SINGLELINE | DT_LEFT | DT_VCENTER, g_bigFont);
    RECT playingLabel{22, 45, 174, 74};
    Text(dc, L"ACTUALLY PLAYING", playingLabel, SECTION_YELLOW, DT_SINGLELINE | DT_VCENTER);
    RECT playingFile{176, 45, std::max(177, w - 395), 74};
    Text(dc, g_playingPath.empty() ? L"Aucune lecture" : FileLabel(g_playingPath),
         playingFile, LCD, DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
    DrawPlayingWave(dc);
    Line(dc, 22, 242, w - 22, 242, BORDER);
    RECT selectedLabel{22, 249, 174, 278};
    Text(dc, L"SELECTED TRACK", selectedLabel, SECTION_YELLOW, DT_SINGLELINE | DT_VCENTER);
    RECT selectedFile{176, 249, w - 22, 278};
    Text(dc, g_selectedPath.empty() ? L"Aucun morceau selectionne" : FileLabel(g_selectedPath),
         selectedFile, LCD, DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
    DrawButtons(dc);
    DrawDelaySlider(dc);
    DrawWave(dc);
    RECT status{22, h - 53, w - 22, h - 31};
    Text(dc, g_status, status, MUTED, DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
    RECT tip{22, h - 29, w - 22, h - 7};
    std::wstring hint = g_mode == ModeSeek ? L"SEEK : cliquez sur ACTUALLY PLAYING pour deplacer la lecture."
        : g_mode == ModeSwitch ? L"SWITCH : cliquez sur SELECTED TRACK pour lancer ce morceau a cet instant."
        : g_mode == ModeStart ? L"Cliquez sur la waveform pour placer IN."
        : g_mode == ModeEnd ? L"Cliquez sur la waveform pour placer OUT."
        : g_mode == ModeDelete ? L"DELETE : cliquez sur un repere pour supprimer SANS deplacer la lecture."
        : L"Lecture protegee : activez SEEK pour sauter. Les anciens reperes restent disponibles pour suppression.";
    Line(dc, 22, h - 58, w - 22, h - 58, BORDER);
    Text(dc, hint, tip, MUTED, DT_SINGLELINE | DT_END_ELLIPSIS);
    BitBlt(target, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previousBmp);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

void ClearMarker() { g_marker = MarkerNone; g_markerIndex = -1; }

bool MarkAtX(int x) {
    ClearMarker();
    if (!g_wave) return false;
    int bestDistance = 9;
    auto inspect = [&](int64_t ms, Marker marker, int index) {
        if (ms < 0) return;
        int distance = abs(x - XAt(ms));
        if (distance < bestDistance) {
            bestDistance = distance;
            g_marker = marker;
            g_markerIndex = index;
        }
    };
    inspect(g_cues.start_ms, MarkerStart, -1);
    inspect(g_cues.end_ms, MarkerEnd, -1);
    for (size_t i = 0; i < g_cues.marks.size(); ++i)
        inspect(g_cues.marks[i], MarkerCue, static_cast<int>(i));
    return g_marker != MarkerNone;
}

void RemoveMarker() {
    switch (g_marker) {
    case MarkerStart: g_cues.start_ms = -1; break;
    case MarkerEnd: g_cues.end_ms = -1; break;
    case MarkerCue:
        if (g_markerIndex >= 0 && static_cast<size_t>(g_markerIndex) < g_cues.marks.size())
            g_cues.marks.erase(g_cues.marks.begin() + g_markerIndex);
        break;
    default: return;
    }
    ClearMarker();
    CommitCues();
    g_status = L"Repere supprime.";
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

bool MoveMarker(int64_t ms) {
    if (!g_wave) return false;
    bool valid = false;
    switch (g_marker) {
    case MarkerStart: valid = wavecue::set_start(g_cues, ms, PlotDuration()); break;
    case MarkerEnd: valid = wavecue::set_end(g_cues, ms, PlotDuration()); break;
    case MarkerCue:
        if (g_markerIndex >= 0 && static_cast<size_t>(g_markerIndex) < g_cues.marks.size()) {
            g_cues.marks[static_cast<size_t>(g_markerIndex)] = ms;
            valid = true;
        }
        break;
    default: break;
    }
    return valid;
}

void ActivateButton(int id) {
    switch (id) {
    case BUTTON_STREAM_DELAY:
        g_streamDelayEnabled = !g_streamDelayEnabled;
        UpdateDelayedPlayhead();
        SaveStreamDelay();
        RefreshButtonLayout();
        g_status = g_streamDelayEnabled
            ? L"Indicateur Twitch actif : delai visuel estime, sans traitement audio."
            : L"Indicateur Twitch desactive.";
        break;
    case BUTTON_MASTER_AUTO:
        g_masterAuto = !g_masterAuto;
        SaveMasterAuto();
        // Toggling the master switch in mid-song must never rewind or
        // immediately skip because the playhead passed OUT while disabled.
        g_startPending = false;
        g_skipSent = g_masterAuto && g_playbackCues.end_ms >= 0 &&
                     g_playheadMs >= g_playbackCues.end_ms;
        g_status = g_masterAuto
            ? L"Auto IN/OUT global actif. Les reglages des chansons sont conserves."
            : L"Auto IN/OUT global suspendu. Lecture Winamp normale.";
        RefreshButtonLayout();
        break;
    case BUTTON_LOAD: RequestLoad(); break;
    case BUTTON_PLAYING_LOAD: RequestPlayingLoad(); break;
    case BUTTON_SEEK:
        g_mode = g_mode == ModeSeek ? ModeNone : ModeSeek;
        g_status = g_mode == ModeSeek
            ? L"SEEK : cliquez dans ACTUALLY PLAYING pour deplacer la lecture."
            : L"SEEK desactive.";
        break;
    case BUTTON_SWITCH:
        g_mode = g_mode == ModeSwitch ? ModeNone : ModeSwitch;
        g_status = g_mode == ModeSwitch
            ? L"SWITCH : cliquez dans SELECTED TRACK pour jouer ce morceau a cet endroit."
            : L"SWITCH annule.";
        break;
    case BUTTON_START: g_mode = g_mode == ModeStart ? ModeNone : ModeStart; break;
    case BUTTON_END: g_mode = g_mode == ModeEnd ? ModeNone : ModeEnd; break;
    case BUTTON_DELETE:
        g_mode = g_mode == ModeDelete ? ModeNone : ModeDelete;
        g_status = g_mode == ModeDelete
            ? L"Mode suppression : cliquez sur une ligne IN, OUT ou CUE."
            : L"Mode suppression termine.";
        break;
    case BUTTON_PLAYING_AUTO:
        if (!g_playingPath.empty() && g_playingIndex >= 0) {
            g_playbackCues.auto_apply = !g_playbackCues.auto_apply;
            SaveCues(g_playingPath, g_playbackCues);
            // Selected-track editor and playing panel must show the same
            // persisted setting when they refer to the same audio file.
            if (_wcsicmp(g_selectedPath.c_str(), g_playingPath.c_str()) == 0)
                g_cues.auto_apply = g_playbackCues.auto_apply;
            // Enabling Auto mid-track must not instantly skip an already
            // passed OUT boundary; do not rewind to IN either.
            if (g_playbackCues.auto_apply)
                g_skipSent = g_playbackCues.end_ms >= 0 &&
                             g_playheadMs >= g_playbackCues.end_ms;
            g_status = g_playbackCues.auto_apply
                ? L"Auto IN/OUT actif pour le morceau en lecture."
                : L"Auto IN/OUT desactive pour le morceau en lecture.";
        }
        break;
    case BUTTON_AUTO:
        if (!g_selectedPath.empty()) {
            g_cues.auto_apply = !g_cues.auto_apply;
            CommitCues();
            // A new activation on the current track must not cause an accidental
            // immediate OUT skip; it applies at the next boundary crossing.
            if (g_cues.auto_apply && _wcsicmp(g_selectedPath.c_str(), g_playingPath.c_str()) == 0)
                g_skipSent = (g_cues.end_ms >= 0 && g_playheadMs >= g_cues.end_ms);
            g_status = g_cues.auto_apply
                ? L"Auto IN/OUT actif pour ce morceau seulement."
                : L"Auto IN/OUT desactive pour ce morceau.";
        }
        break;
    default: break;
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// The tempo badge is interactive in either panel. It never triggers seek
// or cue editing, and the chosen half/double-time view is saved per song.
bool ClickBpmBadge(POINT pt) {
    const bool selected = PtInRect(&g_plot, pt) != 0;
    const bool playing = PtInRect(&g_playingPlot, pt) != 0;
    if (!selected && !playing) return false;
    const RECT& plot = selected ? g_plot : g_playingPlot;
    std::shared_ptr<WaveData> wave = selected ? g_wave : g_playingWave;
    const std::wstring& path = selected ? g_selectedPath : g_playingPath;
    if (!wave || wave->bpm < 80 || path.empty()) return false;
    RECT badge{plot.left + 10, plot.top + 8, plot.left + 74, plot.top + 39};
    if (!PtInRect(&badge, pt)) return false;
    wave->bpmDouble = !wave->bpmDouble;
    WriteBpmChoice(path, wave->bpmDouble);
    if (g_wave && _wcsicmp(path.c_str(), g_selectedPath.c_str()) == 0)
        g_wave->bpmDouble = wave->bpmDouble;
    if (g_playingWave && _wcsicmp(path.c_str(), g_playingPath.c_str()) == 0)
        g_playingWave->bpmDouble = wave->bpmDouble;
    g_status = !wave->bpmDouble ? L"Tempo detecte restaure." :
        (wave->bpm >= 110 ? L"Tempo divise par deux et memorise." :
                            L"Tempo multiplie par deux et memorise.");
    InvalidateRect(g_hwnd, &g_plot, FALSE);
    InvalidateRect(g_hwnd, &g_playingPlot, FALSE);
    return true;
}

void ClickPlot(POINT pt) {
    if (!g_wave || !IsInPlot(pt)) return;
    // SEEK is armed for the playing panel only.
    if (g_mode == ModeSeek) return;
    const int64_t ms = MsAt(pt.x);
    bool ok = true;
    if (g_mode == ModeDelete) {
        if (MarkAtX(pt.x)) RemoveMarker();
        else g_status = L"Pour supprimer, cliquez sur une ligne coloree.";
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (g_mode == ModeSwitch) {
        // Explicit two-step action; never alter stored IN/OUT markers.
        g_mode = ModeNone;
        if (!g_winamp || !IsWindow(g_winamp) || g_selectedIndex < 0 ||
            g_selectedPath.empty()) {
            g_status = L"SWITCH indisponible : selection invalide.";
        } else {
            // Keep the seek pending until Winamp reports the new file as
            // playing: applying it before the Play command would target
            // the previous track.
            g_switchPendingPath = g_selectedPath;
            g_switchPendingMs = ms;
            g_switchPendingUntil = GetTickCount64() + 5000;
            SendMessageW(g_winamp, WM_WA_IPC,
                         static_cast<WPARAM>(g_selectedIndex), IPC_SETPLAYLISTPOS);
            SendMessageW(g_winamp, WM_COMMAND, WINAMP_BUTTON2, 0);
            g_status = L"SWITCH : lancement au point " + TimeLabel(ms) + L".";
            PollWinamp();
        }
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (g_mode == ModeStart) {
        ok = wavecue::set_start(g_cues, ms, PlotDuration());
    } else if (g_mode == ModeEnd) {
        ok = wavecue::set_end(g_cues, ms, PlotDuration());
    } else {
        if (MarkAtX(pt.x)) {
            g_dragging = true;
            g_moved = false;
            g_down = pt;
            SetCapture(g_hwnd);
        } else {
            ClearMarker();
            g_status = L"Lecture protegee : activez SEEK avant un deplacement.";
        }
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (ok) {
        CommitCues();
        g_status = L"Repere sauvegarde : " + TimeLabel(ms);
        g_mode = ModeNone;
    } else g_status = L"Repere invalide ou trop proche d'un autre cue.";
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// A separate lightweight native Win32 settings window. It deliberately
// contains no working settings yet: the user can choose them later without
// changing the carefully packed waveform and cue editor layout.
// The properties panel uses Win32 controls only: no web runtime or GPU.
HWND g_batchProgressLabel = nullptr;

void SaveAutoAnalyzePreference() {
    const std::wstring file = g_dataDir + L"\\auto_analyze.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"wb") == 0 && f) {
        fputc(g_autoAnalyzeSelection ? '1' : '0', f);
        fclose(f);
    }
}

void LoadAutoAnalyzePreference() {
    const std::wstring file = g_dataDir + L"\\auto_analyze.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"rb") == 0 && f) {
        g_autoAnalyzeSelection = fgetc(f) == '1';
        fclose(f);
    }
}

void CancelBatch() {
    ++g_batchGeneration;
    {
        std::lock_guard<std::mutex> lk(g_taskMutex);
        g_batchTasks.clear();
    }
    g_batchTotal = 0;
    g_batchCompleted = 0;
    UpdatePropertiesProgress();
}

void StartBatch() {
    if (!g_winamp || !IsWindow(g_winamp)) return;
    std::vector<Task> pending;
    std::vector<std::wstring> seen;
    int playlistLength = static_cast<int>(
        SendMessageW(g_winamp, WM_WA_IPC, 0, IPC_GETLISTLENGTH));
    const uint64_t generation = ++g_batchGeneration;
    // Only scan a bounded number of playlist entries, and only enqueue
    // non-cached local files. Never keep 30 decoded waveforms in RAM.
    const int scanCount = std::min(playlistLength, 2000);
    for (int i = 0; i < scanCount && static_cast<int>(pending.size()) < BATCH_LIMIT; ++i) {
        const std::wstring path = PlaylistPath(i);
        if (path.empty()) continue;
        bool duplicated = false;
        for (const auto& prior : seen) {
            if (_wcsicmp(prior.c_str(), path.c_str()) == 0) { duplicated = true; break; }
        }
        if (duplicated) continue;
        seen.push_back(path);
        FileStamp stamp{};
        if (!ReadFileStamp(path, stamp)) continue;
        Task task{path, stamp, generation, false, true};
        if (LoadCache(task)) continue;
        pending.push_back(task);
    }
    {
        std::lock_guard<std::mutex> lk(g_taskMutex);
        g_batchTasks.clear();
        for (const auto& task : pending) g_batchTasks.push_back(task);
    }
    g_batchCompleted = 0;
    g_batchTotal = static_cast<int>(pending.size());
    g_taskWake.notify_one();
    UpdatePropertiesProgress();
}

HWND AddPrefControl(DWORD style, const wchar_t* cls, const wchar_t* label,
                    int x, int y, int w, int h, int id = 0) {
    HWND control = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style,
        x, y, w, h, g_propertiesHwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        g_plugin.hDllInstance, nullptr);
    if (control) {
        if (g_font)
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        g_propertyPageControls.push_back(control);
    }
    return control;
}

void UpdatePropertiesProgress() {
    if (!g_batchProgressLabel || !IsWindow(g_batchProgressLabel)) return;
    const int total = g_batchTotal.load(), done = g_batchCompleted.load();
    const std::wstring line = total == 0
        ? L"Aucun titre en attente."
        : (std::to_wstring(done) + L" / " + std::to_wstring(total) +
           (done >= total ? L" - termine" : L" - analyse en cours"));
    SetWindowTextW(g_batchProgressLabel, line.c_str());
}

void ShowPropertiesPage(int page) {
    g_propertiesPage = page;
    if (!g_propertiesHwnd || !IsWindow(g_propertiesHwnd)) return;
    for (HWND control : g_propertyPageControls)
        if (IsWindow(control)) DestroyWindow(control);
    g_propertyPageControls.clear();
    g_batchProgressLabel = nullptr;

    // Left navigation and recessed content panel follow classic Winamp.
    if (page == 0) {
        AddPrefControl(SS_LEFT, L"STATIC", L"General Preferences", 218, 20, 420, 24);
        AddPrefControl(BS_AUTOCHECKBOX | WS_TABSTOP, L"BUTTON",
            L"Auto Load waveform (Selected + Actually Playing)",
            219, 67, 420, 25, ID_PREF_AUTO);
        HWND check = g_propertyPageControls.back();
        SendMessageW(check, BM_SETCHECK,
            g_autoAnalyzeSelection ? BST_CHECKED : BST_UNCHECKED, 0);
        AddPrefControl(SS_LEFT, L"STATIC",
            L"Analyse automatiquement les nouveaux titres locaux.\r\n"
            L"Les fichiers deja en cache s'affichent immediatement.",
            237, 96, 415, 48);
        AddPrefControl(SS_LEFT, L"STATIC", L"Playlist preloading", 218, 165, 420, 24);
        AddPrefControl(SS_LEFT, L"STATIC",
            L"Maximum : 30 titres locaux non analyses par lot.",
            219, 197, 425, 24);
        AddPrefControl(BS_OWNERDRAW, L"BUTTON", L"Batch Load (30 max)",
            219, 237, 199, 29, ID_PREF_BATCH);
        AddPrefControl(BS_OWNERDRAW, L"BUTTON", L"Cancel",
            429, 237, 100, 29, ID_PREF_BATCH_CANCEL);
        g_batchProgressLabel = AddPrefControl(SS_LEFT, L"STATIC",
            L"", 219, 283, 400, 24);
        UpdatePropertiesProgress();
    } else if (page == 1) {
        AddPrefControl(SS_LEFT, L"STATIC", L"Analysis Preferences",
            218, 20, 420, 24);
        AddPrefControl(SS_LEFT, L"STATIC",
            L"Waveform / RMS\r\n"
            L"Enveloppe RMS a 50 ms; cache sur disque.\r\n\r\n"
            L"Key / Camelot\r\n"
            L"Detection harmonique experimentale (libKeyFinder).\r\n\r\n"
            L"BPM / Tempo\r\n"
            L"Estimation BTrack; affichage x2 / demi-tempo par titre.\r\n\r\n"
            L"Ces calculs sont lies au chargement de la waveform.",
            219, 68, 423, 265);
    } else {
        AddPrefControl(SS_LEFT, L"STATIC", L"WaveCue - About",
            218, 20, 420, 24);
        AddPrefControl(SS_LEFT, L"STATIC",
            L"WaveCue est ne du besoin de Le Porte-Noir,\r\n"
            L"DJ independant, de preparer des morceaux dans\r\n"
            L"Winamp classique : waveform, cues IN/OUT et\r\n"
            L"controle immediat en concert ou en diffusion.\r\n\r\n"
            L"Conception et essais : Le Porte-Noir et les\r\n"
            L"contributeurs du projet WaveCue.\r\n\r\n"
            L"Composants : libKeyFinder, FFTW, BTrack,\r\n"
            L"libsamplerate, dr_flac, stb_vorbis.\r\n\r\n"
            L"Code source WaveCue : MIT (voir LICENSE).\r\n"
            L"Distribution binaire : respecter egalement les\r\n"
            L"licences des bibliotheques integrees.",
            219, 68, 425, 350);
    }
    InvalidateRect(g_propertiesHwnd, nullptr, TRUE);
}

LRESULT CALLBACK PropertiesWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == ID_PROPERTIES_CLOSE) { DestroyWindow(hwnd); return 0; }
        if (id == ID_PREF_GENERAL) { ShowPropertiesPage(0); return 0; }
        if (id == ID_PREF_ANALYSIS) { ShowPropertiesPage(1); return 0; }
        if (id == ID_PREF_ABOUT) { ShowPropertiesPage(2); return 0; }
        if (id == ID_PREF_BATCH) { StartBatch(); return 0; }
        if (id == ID_PREF_BATCH_CANCEL) { CancelBatch(); return 0; }
        if (id == ID_PREF_AUTO && HIWORD(wparam) == BN_CLICKED) {
            g_autoAnalyzeSelection =
                SendMessageW(reinterpret_cast<HWND>(lparam), BM_GETCHECK, 0, 0) == BST_CHECKED;
            SaveAutoAnalyzePreference();
            if (g_autoAnalyzeSelection) {
                if (!g_selectedPath.empty() && !g_wave) RequestLoad();
                if (!g_playingPath.empty() && !g_playingWave) RequestPlayingLoad();
            }
            return 0;
        }
        break;
    }
    case WM_DRAWITEM: {
        auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (!item || item->CtlType != ODT_BUTTON) break;
        const bool selected = item->CtlID == static_cast<UINT>(
            g_propertiesPage == 0 ? ID_PREF_GENERAL :
            g_propertiesPage == 1 ? ID_PREF_ANALYSIS : ID_PREF_ABOUT);
        HBRUSH background = CreateSolidBrush(
            (item->itemState & ODS_SELECTED) ? RGB(69, 69, 89) :
            selected ? RGB(58, 58, 76) : RGB(47, 47, 64));
        FillRect(item->hDC, &item->rcItem, background);
        DeleteObject(background);
        HBRUSH edge = CreateSolidBrush(RGB(124, 124, 150));
        FrameRect(item->hDC, &item->rcItem, edge);
        DeleteObject(edge);
        wchar_t label[128]{};
        GetWindowTextW(item->hwndItem, label, 128);
        SetTextColor(item->hDC, RGB(124, 225, 66));
        SetBkMode(item->hDC, TRANSPARENT);
        HGDIOBJ old = g_font ? SelectObject(item->hDC, g_font) : nullptr;
        RECT r = item->rcItem;
        DrawTextW(item->hDC, label, -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (old) SelectObject(item->hDC, old);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, RGB(0, 255, 0));
        SetBkColor(dc, RGB(47, 47, 64));
        return reinterpret_cast<LRESULT>(g_propertiesBrush);
    }
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        HDC dc = reinterpret_cast<HDC>(wparam);
        FillRect(dc, &rect, g_propertiesBrush);
        RECT sidebar{11, 11, 191, rect.bottom - 13};
        RECT content{203, 11, rect.right - 12, rect.bottom - 13};
        HBRUSH dark = CreateSolidBrush(RGB(27, 27, 39));
        HBRUSH mid = CreateSolidBrush(RGB(47, 47, 64));
        FillRect(dc, &sidebar, dark);
        FillRect(dc, &content, mid);
        DeleteObject(dark);
        DeleteObject(mid);
        HBRUSH border = CreateSolidBrush(RGB(118, 118, 145));
        FrameRect(dc, &sidebar, border);
        FrameRect(dc, &content, border);
        DeleteObject(border);
        return 1;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_propertyPageControls.clear();
        g_batchProgressLabel = nullptr;
        if (g_propertiesHwnd == hwnd) g_propertiesHwnd = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void ShowPropertiesWindow() {
    if (g_propertiesHwnd && IsWindow(g_propertiesHwnd)) {
        ShowWindow(g_propertiesHwnd, SW_RESTORE);
        SetForegroundWindow(g_propertiesHwnd);
        return;
    }
    const wchar_t* className = L"WinampWaveCueProperties1";
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = PropertiesWndProc;
    cls.hInstance = g_plugin.hDllInstance;
    cls.lpszClassName = className;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return;
    RECT owner{};
    if (g_hwnd) GetWindowRect(g_hwnd, &owner);
    if (!g_propertiesBrush) g_propertiesBrush = CreateSolidBrush(RGB(47, 47, 64));
    HWND dialog = CreateWindowExW(WS_EX_TOOLWINDOW, className,
        L"WaveCue Preferences", WS_POPUP | WS_CAPTION | WS_SYSMENU,
        owner.left + 90, owner.top + 90, 690, 475, g_hwnd, nullptr,
        g_plugin.hDllInstance, nullptr);
    if (!dialog) return;
    g_propertiesHwnd = dialog;
    const struct { const wchar_t* title; int id; } tabs[] = {
        {L"General Preferences", ID_PREF_GENERAL},
        {L"Analysis", ID_PREF_ANALYSIS},
        {L"About WaveCue", ID_PREF_ABOUT}
    };
    for (int i = 0; i < 3; ++i) {
        HWND button = CreateWindowExW(0, L"BUTTON", tabs[i].title,
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            17, 39 + i * 35, 168, 29, dialog,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(tabs[i].id)),
            g_plugin.hDllInstance, nullptr);
        if (button && g_font)
            SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    }
    ShowPropertiesPage(g_propertiesPage);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
}

void ShowPropertiesMenu(HWND hwnd, POINT pt) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, ID_PROPERTIES_MENU, L"Proprietes...");
    SetForegroundWindow(hwnd);
    const UINT chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                       pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (chosen == ID_PROPERTIES_MENU) ShowPropertiesWindow();
}

LRESULT CALLBACK WaveWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = 760;
        info->ptMinTrackSize.y = 560;
        return 0;
    }
    case WM_CREATE:
        SetTimer(hwnd, TIMER_POLL, 170, nullptr);
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0) == ID_PROPERTIES_MENU) {
            ShowPropertiesWindow();
            return 0;
        }
        break;
    case WM_CONTEXTMENU: {
        // Right-click on the title/toolbar only; waveform and cues stay untouched.
        if (reinterpret_cast<HWND>(wparam) != hwnd) break;
        POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (screen.x == -1 && screen.y == -1) {
            RECT bounds{};
            GetWindowRect(hwnd, &bounds);
            screen = {bounds.left + 35, bounds.top + 28};
        } else {
            POINT client = screen;
            ScreenToClient(hwnd, &client);
            if (client.y < 0 || client.y >= 41 || client.x < 0 || client.x > 140)
                break;
        }
        ShowPropertiesMenu(hwnd, screen);
        return 0;
    }
    case WM_SIZE:
        Layout(LOWORD(lparam), HIWORD(lparam));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (wparam == TIMER_POLL) {
            const std::wstring selectedBefore = g_selectedPath;
            const auto oldPlayhead = g_playheadMs;
            SelectPlaylistItem();
            PollWinamp();
            if (selectedBefore == g_selectedPath &&
                (oldPlayhead != g_playheadMs || g_isLoading))
                InvalidateRect(hwnd, &g_plot, FALSE);
            if (oldPlayhead != g_playheadMs)
                InvalidateRect(hwnd, &g_playingPlot, FALSE);
            return 0;
        }
        break;
    case WM_WAVE_RESULT:
        AcceptWorkerResult();
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (pt.x >= 22 && pt.x < 142 && pt.y >= 5 && pt.y < 39) {
            ShowPropertiesWindow();
            return 0;
        }
        SetFocus(hwnd);
        int button = ButtonUnder(pt);
        if (button) ActivateButton(button);
        else if (ClickBpmBadge(pt)) return 0;
        else if (g_mode == ModeSeek && PtInRect(&g_playingPlot, pt) &&
                 g_playingWave && g_playingWave->duration_ms > 0) {
            const int width = std::max(1L, g_playingPlot.right - g_playingPlot.left - 20L);
            const int relX = std::clamp<int>(pt.x - g_playingPlot.left - 10, 0, width);
            const int64_t ms = static_cast<int64_t>(relX) *
                static_cast<int64_t>(g_playingWave->duration_ms) / width;
            g_mode = ModeNone;
            if (g_playingIndex >= 0 && g_playheadMs >= 0) {
                SendMessageW(g_winamp, WM_WA_IPC,
                             static_cast<WPARAM>(ms), IPC_JUMPTOTIME);
                g_status = L"SEEK : lecture deplacee a " + TimeLabel(ms) + L".";
            } else g_status = L"SEEK indisponible pendant l'arret.";
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        else if (InDelaySlider(pt)) {
            g_delayDragging = true;
            SetCapture(hwnd);
            SetDelayFromX(pt.x);
        } else ClickPlot(pt);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (g_delayDragging) {
            SetDelayFromX(pt.x);
            g_delayDragging = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            SaveStreamDelay();
            return 0;
        }
        if (g_dragging) {
            if (GetCapture() == hwnd) ReleaseCapture();
            g_dragging = false;
            if (g_moved) {
                if (MoveMarker(MsAt(pt.x))) {
                    CommitCues();
                    g_status = L"Repere deplace et sauvegarde.";
                } else {
                    g_cues = LoadCues(g_selectedPath);
                    g_status = L"Limites IN/OUT incompatibles.";
                }
            } else if (g_marker != MarkerNone) {
                // Selecting an existing marker must never seek implicitly.
                g_status = L"Repere selectionne. Activez SEEK pour deplacer la lecture.";
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (g_delayDragging && (wparam & MK_LBUTTON)) {
            SetDelayFromX(pt.x);
            return 0;
        }
        int hover = ButtonUnder(pt);
        if (hover != g_hover) {
            g_hover = hover;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (g_dragging && (wparam & MK_LBUTTON)) {
            if (abs(pt.x - g_down.x) > 3) g_moved = true;
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            g_mode = ModeNone;
            ClearMarker();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wparam == VK_DELETE) {
            if (g_marker != MarkerNone) RemoveMarker();
            else {
                g_mode = ModeDelete;
                g_status = L"Mode suppression : cliquez sur un repere, sans seek.";
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_POLL);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool MakeStorage() {
    wchar_t appdata[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA | CSIDL_FLAG_CREATE, nullptr,
                                SHGFP_TYPE_CURRENT, appdata))) return false;
    g_dataDir = std::wstring(appdata) + L"\\Winamp_WaveCue";
    g_cacheDir = g_dataDir + L"\\cache";
    CreateDirectoryW(g_dataDir.c_str(), nullptr);
    CreateDirectoryW(g_cacheDir.c_str(), nullptr);
    return GetFileAttributesW(g_cacheDir.c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // anonymous namespace

static int PluginInit() {
    g_winamp = g_plugin.hwndParent;
    if (!g_winamp || !MakeStorage()) return 1;
    LoadMasterAuto();
    LoadStreamDelay();
    LoadAutoAnalyzePreference();
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = WaveWndProc;
    cls.hInstance = g_plugin.hDllInstance;
    cls.lpszClassName = L"WinampWaveCueWindow1";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = nullptr;
    if (!RegisterClassExW(&cls)) return 1;
    g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_bigFont = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, cls.lpszClassName, L"WaveCue - Winamp",
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                              1030, 670, nullptr, nullptr, g_plugin.hDllInstance, nullptr);
    if (!g_hwnd) {
        DeleteObject(g_font);
        DeleteObject(g_bigFont);
        g_font = g_bigFont = nullptr;
        UnregisterClassW(cls.lpszClassName, g_plugin.hDllInstance);
        return 1;
    }
    if (HMENU systemMenu = GetSystemMenu(g_hwnd, FALSE)) {
        AppendMenuW(systemMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(systemMenu, MF_STRING, ID_PROPERTIES_MENU, L"Proprietes...");
    }
    g_noticeHwnd.store(g_hwnd);
    try { g_worker = std::thread(WorkerMain); }
    catch (...) {
        g_noticeHwnd.store(nullptr);
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        DeleteObject(g_font);
        DeleteObject(g_bigFont);
        g_font = g_bigFont = nullptr;
        UnregisterClassW(cls.lpszClassName, g_plugin.hDllInstance);
        return 1;
    }
    // Hook the official 5.x playback-status notification on the same UI thread.
    // This removes up to 170 ms of polling latency at the beginning of a song.
    g_winampSubclassInstalled = SetWindowSubclass(g_winamp, WinampEventProc, 1, 0) != FALSE;
    SelectPlaylistItem();
    PollWinamp();
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    return 0;
}

static void PluginConfig() {
    if (!g_hwnd || !IsWindow(g_hwnd)) return;
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
}

static void PluginQuit() {
    if (g_winampSubclassInstalled && g_winamp && IsWindow(g_winamp)) {
        RemoveWindowSubclass(g_winamp, WinampEventProc, 1);
        g_winampSubclassInstalled = false;
    }
    g_noticeHwnd.store(nullptr);
    ++g_generation;
    ++g_playGeneration;
    {
        std::lock_guard<std::mutex> lk(g_taskMutex);
        g_workerStop = true;
        g_pendingTask.reset();
        g_pendingPlayingTask.reset();
        g_batchTasks.clear();
        ++g_batchGeneration;
    }
    g_taskWake.notify_one();
    if (g_worker.joinable()) g_worker.join();
    if (g_propertiesHwnd && IsWindow(g_propertiesHwnd))
        DestroyWindow(g_propertiesHwnd);
    g_propertiesHwnd = nullptr;
    if (g_propertiesBrush) DeleteObject(g_propertiesBrush);
    g_propertiesBrush = nullptr;
    if (g_hwnd) DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    if (g_font) DeleteObject(g_font);
    if (g_bigFont) DeleteObject(g_bigFont);
    if (g_lcdDotBrush) DeleteObject(g_lcdDotBrush);
    g_lcdDotBrush = nullptr;
    g_font = g_bigFont = nullptr;
    UnregisterClassW(L"WinampWaveCueWindow1", g_plugin.hDllInstance);
}
