// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "worldsbackend.h"

class LocalWorldsBackend : public WorldsBackend {
public:
    explicit LocalWorldsBackend(const QString& path, QObject* parent = nullptr);
    Kind kind() const override {
        return Kind::Local;
    }
    QString location() const override {
        return m_path;
    }
    void load() override;
    void save(const QString& content, const QString& revision, bool reload) override;
    void reload(const QString& revision) override;

private:
    void request(const QString& operation, const QJsonObject& body);
    void offline(const QString& operation, const QJsonObject& body);
    QString m_path;
};
