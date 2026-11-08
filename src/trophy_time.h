// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

inline int64_t TrophyUnixSeconds(uint64_t value, int64_t nowUnix) {
    constexpr uint64_t UnixToCeSeconds = 62'135'596'800ULL;
    constexpr uint64_t MillisMin = 100'000'000'000ULL;
    constexpr uint64_t MicrosMin = 100'000'000'000'000ULL;
    constexpr uint64_t MicrosMax = 10'000'000'000'000'000ULL;
    constexpr uint64_t CeMicrosMin = UnixToCeSeconds * 1'000'000ULL;
    constexpr uint64_t CeMicrosMax = 70'000'000'000'000'000ULL;
    constexpr uint64_t NanosMin = 1'000'000'000'000'000'000ULL;
    constexpr uint64_t NanosMax = 10'000'000'000'000'000'000ULL;

    if (value == 0)
        return nowUnix;
    if (value < MillisMin)
        return static_cast<int64_t>(value);
    if (value < MicrosMin)
        return static_cast<int64_t>(value / 1'000ULL);
    if (value < MicrosMax)
        return static_cast<int64_t>(value / 1'000'000ULL);
    if (value >= CeMicrosMin && value < CeMicrosMax)
        return static_cast<int64_t>(value / 1'000'000ULL - UnixToCeSeconds);
    if (value >= NanosMin && value < NanosMax)
        return static_cast<int64_t>(value / 1'000'000'000ULL);
    return nowUnix;
}
