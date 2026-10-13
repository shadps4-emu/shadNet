// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringDecoder>
#include "worldsstore.h"

QString worldsRevision(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString worldsFilePath(const QString& path) {
    QFileInfo info(path);
    QString normalized = info.canonicalFilePath();
    if (normalized.isEmpty()) {
        const QString parent = QDir(info.absolutePath()).canonicalPath();
        normalized = parent.isEmpty() ? info.absoluteFilePath()
                                      : parent + QLatin1Char('/') + info.fileName();
    }
    return normalized;
}

QString worldsSocketName(const QString& path) {
    QString normalized = worldsFilePath(path);
#ifdef Q_OS_WIN
    normalized = normalized.toLower();
#endif
    return QStringLiteral("shadnet-worlds-") + worldsRevision(normalized.toUtf8()).left(32);
}

WorldsFileTransaction::WorldsFileTransaction(const QString& path)
    : WorldsFileTransaction(path, QStringLiteral("worlds.cfg"), [](const QString& content) {
          const auto parsed = parseWorldsConfig(content);
          return parsed.isValid() ? QString()
                                  : QStringLiteral("Line %1: %2")
                                        .arg(parsed.errors.first().line)
                                        .arg(parsed.errors.first().message);
      }) {}

WorldsFileTransaction::WorldsFileTransaction(const QString& path, const QString& label,
                                             Validator validate)
    : m_path(worldsFilePath(path)), m_label(label), m_validate(std::move(validate)),
      m_lock(m_path + QStringLiteral(".lock")), m_locked(m_lock.tryLock(0)) {}

bool WorldsFileTransaction::read(WorldsSnapshot& snapshot, QString& error) const {
    if (!m_locked) {
        error = QStringLiteral("%1 is locked by another writer, or its directory is not writable.")
                    .arg(m_label);
        return false;
    }
    QFile file(m_path);
    if (!file.exists()) {
        snapshot = {QString(), QStringLiteral("missing"), QString(), QString(), false, false};
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return false;
    }
    const QByteArray bytes = file.read(kMaxWorldsBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > kMaxWorldsBytes) {
        error = QStringLiteral("Cannot read %1, or it exceeds the 512 KiB limit.").arg(m_label);
        return false;
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder(bytes);
    if (decoder.hasError()) {
        error = QStringLiteral("%1 must contain valid UTF-8 text.").arg(m_label);
        return false;
    }
    snapshot = {text, worldsRevision(bytes), QString(), QString(), false, true};
    return true;
}

bool WorldsFileTransaction::write(const QString& content, const QString& expectedRevision,
                                  WorldsSnapshot& snapshot, QString& error, int& status) {
    status = 500;
    if (const QString invalid = m_validate ? m_validate(content) : QString(); !invalid.isEmpty()) {
        status = 422;
        error = invalid;
        return false;
    }
    WorldsSnapshot current;
    if (!read(current, error))
        return false;
    if (expectedRevision.isEmpty() || current.revision != expectedRevision) {
        status = 409;
        error = QStringLiteral("%1 changed since it was read. Read the latest "
                               "version before saving.")
                    .arg(m_label);
        return false;
    }
    const QByteArray bytes = content.toUtf8();
    if (current.revision == worldsRevision(bytes)) {
        snapshot = current;
        return true;
    }
    if (current.exists) {
        // Copy the original bytes, including its encoding marker and line endings.
        QFile source(m_path);
        if (!source.open(QIODevice::ReadOnly)) {
            error = source.errorString();
            return false;
        }
        const QByteArray original = source.read(kMaxWorldsBytes + 1);
        if (source.error() != QFileDevice::NoError ||
            worldsRevision(original) != expectedRevision) {
            status = 409;
            error =
                QStringLiteral("%1 changed while preparing the save. Read it again.").arg(m_label);
            return false;
        }
        QSaveFile backup(m_path + QStringLiteral(".bak"));
        backup.setDirectWriteFallback(false);
        if (!backup.open(QIODevice::WriteOnly) || backup.write(original) != original.size() ||
            !backup.commit()) {
            error = QStringLiteral("Cannot create %1.bak: %2").arg(m_label, backup.errorString());
            return false;
        }
    }
    // Recheck after preparing the backup to reduce the race with external
    // editors.
    if (!read(current, error))
        return false;
    if (current.revision != expectedRevision) {
        status = 409;
        error = QStringLiteral("%1 changed while preparing the save. Read it again.").arg(m_label);
        return false;
    }
    QSaveFile file(m_path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = QStringLiteral("Cannot save %1: %2").arg(m_label, file.errorString());
        return false;
    }
    snapshot = {content, worldsRevision(bytes), QString(), QString(), false, true};
    return true;
}
