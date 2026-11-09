#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <cwctype>

namespace wavecue {

struct Cues {
    int64_t start_ms = -1;
    int64_t end_ms = -1;
    std::vector<int64_t> marks;
};

// Stable, case-insensitive hash of Windows paths (UTF-16 code units).
inline uint64_t path_hash(const std::wstring& path) {
    uint64_t h = UINT64_C(14695981039346656037);
    for (wchar_t original : path) {
        uint32_t c = static_cast<uint32_t>(std::towlower(original));
        // On Windows wchar_t contains a single UTF-16 code unit.
        h ^= (c & 0xFFFFu);
        h *= UINT64_C(1099511628211);
    }
    return h;
}

inline bool valid_range(const Cues& cues, int64_t duration_ms) {
    return cues.start_ms >= -1 && cues.end_ms >= -1 &&
        (cues.start_ms < 0 || cues.start_ms <= duration_ms) &&
        (cues.end_ms < 0 || cues.end_ms <= duration_ms) &&
        (cues.start_ms < 0 || cues.end_ms < 0 || cues.start_ms < cues.end_ms);
}

inline bool set_start(Cues& cues, int64_t ms, int64_t duration_ms) {
    if (ms < 0 || ms > duration_ms || (cues.end_ms >= 0 && ms >= cues.end_ms)) return false;
    cues.start_ms = ms;
    return true;
}

inline bool set_end(Cues& cues, int64_t ms, int64_t duration_ms) {
    if (ms < 0 || ms > duration_ms || (cues.start_ms >= 0 && ms <= cues.start_ms)) return false;
    cues.end_ms = ms;
    return true;
}

inline bool add_mark(Cues& cues, int64_t ms, int64_t duration_ms) {
    if (ms < 0 || ms > duration_ms) return false;
    auto it = std::lower_bound(cues.marks.begin(), cues.marks.end(), ms);
    if (it != cues.marks.end() && (*it - ms) < 20) return false;
    if (it != cues.marks.begin() && (ms - *(it - 1)) < 20) return false;
    cues.marks.insert(it, ms);
    return true;
}

inline void tidy(Cues& cues) {
    std::sort(cues.marks.begin(), cues.marks.end());
    cues.marks.erase(std::unique(cues.marks.begin(), cues.marks.end()), cues.marks.end());
}

inline int64_t clamp_ms(int64_t ms, int64_t duration_ms) {
    return std::clamp<int64_t>(ms, 0, std::max<int64_t>(0, duration_ms));
}

} // namespace wavecue
