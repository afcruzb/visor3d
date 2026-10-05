#pragma once

#include <charconv>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace v3d {

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

inline const char* skipSpace(const char* p, const char* end) {
    while (p < end && isSpace(*p)) ++p;
    return p;
}

// Parses a float after optional whitespace. Returns nullptr on failure.
// std::from_chars (libstdc++ >= 12) is locale-free and uses the fast_float
// algorithm, but rejects a leading '+', so skip it by hand.
inline const char* parseFloat(const char* p, const char* end, float& out) {
    p = skipSpace(p, end);
    if (p < end && *p == '+') ++p;
    auto r = std::from_chars(p, end, out);
    return r.ec == std::errc() ? r.ptr : nullptr;
}

inline const char* parseUint(const char* p, const char* end, uint32_t& out) {
    p = skipSpace(p, end);
    auto r = std::from_chars(p, end, out);
    return r.ec == std::errc() ? r.ptr : nullptr;
}

inline bool startsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && std::memcmp(s.data(), prefix.data(), prefix.size()) == 0;
}

}  // namespace v3d
