// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>

// Title Small Storage file store.
//<NP Communication ID>-<slot id decimal>.tss     e.g. ABCD01234_00-15.tss
// Drop files into tss_data/ to serve them.
class TssFiles {
public:
    static constexpr const char* DIR = "tss_data";
    static constexpr int64_t MAX_SIZE_SLOT0 = 64 * 1024;
    static constexpr int64_t MAX_SIZE_EXTRA = 4 * 1024 * 1024;
    static constexpr int32_t MAX_SLOT = 15;

    static bool Init() {
        if (!QDir().mkpath(DIR)) {
            qCritical() << "TssFiles: cannot create" << DIR;
            return false;
        }
        const int n = QDir(DIR).entryList({"*.tss"}, QDir::Files).size();
        qInfo() << "TssFiles: ready," << n << "file(s) in" << DIR;
        return true;
    }

    static int64_t MaxSizeForSlot(int32_t slot) {
        return slot == 0 ? MAX_SIZE_SLOT0 : MAX_SIZE_EXTRA;
    }

    struct Entry {
        QByteArray data;
        uint64_t lastModified = 0; // seconds since epoch, 0 when absent
        bool exists = false;
    };

    static Entry Read(const QString& comId, int32_t slot) {
        Entry e;
        const QString p = Path(comId, slot);
        QFileInfo fi(p);
        if (!fi.exists() || !fi.isFile()) {
            return e;
        }
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning() << "TssFiles: cannot open" << p;
            return e;
        }
        e.data = f.readAll();
        e.lastModified = static_cast<uint64_t>(fi.lastModified().toSecsSinceEpoch());
        e.exists = true;
        return e;
    }

    static QString Path(const QString& comId, int32_t slot) {
        QString safe = comId;
        safe.remove(QRegularExpression("[^A-Za-z0-9_]"));
        return QString("%1/%2-%3.tss").arg(DIR, safe).arg(slot);
    }
};
