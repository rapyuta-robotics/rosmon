#pragma once

#include <fmt/format.h>

#include <iostream>

namespace fmtNoThrow {

template <typename S, typename... Args>
inline void print(const S &format_str, const Args &... args) {
    try {
        fmt::print(format_str, args...);
    } catch (const std::exception& e) {
        std::cerr << e.what();
    }
}

template <typename S, typename... Args>
inline void print(std::FILE* f, const S& format_str, const Args&&... args) {
    try {
        fmt::print(f, format_str, args...);
    } catch (const std::exception& e) {
        std::cerr << e.what();
    }
}

} // namespace fmtNoThrow