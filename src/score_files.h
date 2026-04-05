#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSet>

// Manages score_data/*.sdt blobs.
class ScoreFiles {
public:
    static constexpr const char* DIR = "score_data";
    static constexpr const char* EXT = "sdt";

    bool Init() {
        if (!QDir().mkpath(DIR)) {
            qCritical() << "ScoreFiles: cannot create" << DIR;
            return false;
        }
        uint64_t max = 0;
        for (const QString& name : QDir(DIR).entryList({"*." + QString(EXT)}, QDir::Files)) {
            bool ok;
            uint64_t id = name.section('.', 0, 0).toULongLong(&ok);
            if (ok && id > max)
                max = id;
        }
        m_next.store(max + 1, std::memory_order_relaxed);
        qInfo() << "ScoreFiles: ready, next id =" << (max + 1);
        return true;
    }

    uint64_t Create(const QByteArray& data) {
        uint64_t id = m_next.fetch_add(1, std::memory_order_relaxed);
        QFile f(path(id));
        if (!f.open(QIODevice::WriteOnly)) {
            qCritical() << "ScoreFiles: create failed" << f.fileName();
            return 0;
        }
        f.write(data);
        return id;
    }

    std::optional<QByteArray> Read(uint64_t id) const {
        QFile f(path(id));
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning() << "ScoreFiles: read failed" << f.fileName();
            return std::nullopt;
        }
        return f.readAll();
    }

    void Remove(uint64_t id) const {
        if (!QFile::remove(path(id)))
            qWarning() << "ScoreFiles: remove failed" << path(id);
    }

    void CleanOrphans(const QSet<uint64_t>& validIds) const {
        int n = 0;
        for (const QString& name : QDir(DIR).entryList({"*." + QString(EXT)}, QDir::Files)) {
            bool ok;
            uint64_t id = name.section('.', 0, 0).toULongLong(&ok);
            if (ok && !validIds.contains(id)) {
                QFile::remove(QDir(DIR).filePath(name));
                ++n;
            }
        }
        if (n)
            qInfo() << "ScoreFiles: removed" << n << "orphan(s)";
    }

private:
    static QString path(uint64_t id) {
        return QString("%1/%2.%3").arg(DIR).arg(id, 20, 10, QChar('0')).arg(EXT);
    }
    std::atomic<uint64_t> m_next{1};
};
