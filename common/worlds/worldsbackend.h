// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include "worldsconfig.h"

class WorldsBackend : public QObject {
    Q_OBJECT
public:
    enum class Kind { Remote, Local };
    explicit WorldsBackend(QObject* parent = nullptr) : QObject(parent) {}
    virtual Kind kind() const = 0;
    virtual QString location() const = 0;
    virtual void load() = 0;
    virtual void save(const QString& content, const QString& revision, bool reload) = 0;
    virtual void reload(const QString& revision) = 0;
signals:
    void received(const WorldsSnapshot& snapshot);
    // A stale revision or uncertain transport outcome requires reading again.
    void failed(const QString& message, bool readRequired);
};
