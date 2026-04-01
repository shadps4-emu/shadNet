#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <QByteArray>
#include <QString>

// Board configuration

struct ScoreBoardConfig {
    uint32_t rankLimit = 100;
    uint32_t updateMode = 0; // 0 = NORMAL_UPDATE (keep best), 1 = FORCE_UPDATE
    uint32_t sortMode = 0;   // 0 = DESCENDING (higher=better), 1 = ASCENDING (lower=better)
    uint32_t uploadNumLimit = 10;
    uint32_t uploadSizeLimit = 6'000'000;
};

// Score entry

struct ScoreEntry {
    int64_t userId = 0;
    int32_t characterId = 0;
    int64_t score = 0;
    QString comment;
    QByteArray gameInfo;
    std::optional<uint64_t> dataId; // set after RecordScoreData
    uint64_t timestamp = 0;

    // Resolved from account table,populated when loaded into cache
    QString npid;
    QString onlineName;
};

// timestamp
inline uint64_t ShadNetTimestamp() {
    constexpr uint64_t UNIX_TO_CE_US = 62'135'596'800ULL * 1'000'000ULL;
    auto now = std::chrono::system_clock::now();
    uint64_t us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
    return us + UNIX_TO_CE_US;
}