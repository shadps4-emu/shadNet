// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QLockFile>
#include "worldsconfig.h"

QString worldsRevision(const QByteArray& bytes);
QString worldsFilePath(const QString& path);
QString worldsSocketName(const QString& path);

// All cooperating local and remote writers hold the same file lock. A revision
// check also detects changes made by ordinary text editors between operations.
class WorldsFileTransaction {
public:
    explicit WorldsFileTransaction(const QString& path);
    bool locked() const {
        return m_locked;
    }
    bool read(WorldsSnapshot& snapshot, QString& error) const;
    bool write(const QString& content, const QString& expectedRevision, WorldsSnapshot& snapshot,
               QString& error, int& status);

private:
    QString m_path;
    QLockFile m_lock;
    bool m_locked;
};
