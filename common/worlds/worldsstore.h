// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include <QLockFile>
#include "worldsconfig.h"

QString worldsRevision(const QByteArray& bytes);
QString worldsFilePath(const QString& path);
QString worldsSocketName(const QString& path);

// All cooperating local and remote writers hold the same file lock. A revision
// check also detects changes made by ordinary text editors between operations.
// The same transaction serves other line-based config files (scoreboards.cfg):
// pass the file name used in messages and a validator returning "" or the first
// error ("Line N: ...").
class WorldsFileTransaction {
public:
    using Validator = std::function<QString(const QString& content)>;
    explicit WorldsFileTransaction(const QString& path);
    WorldsFileTransaction(const QString& path, const QString& label, Validator validate);
    bool locked() const {
        return m_locked;
    }
    bool read(WorldsSnapshot& snapshot, QString& error) const;
    bool write(const QString& content, const QString& expectedRevision, WorldsSnapshot& snapshot,
               QString& error, int& status);

private:
    QString m_path;
    QString m_label = QStringLiteral("worlds.cfg");
    Validator m_validate;
    QLockFile m_lock;
    bool m_locked;
};
