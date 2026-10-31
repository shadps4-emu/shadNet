// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "game_custom_data_db.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrl>
#include <QVariant>

namespace {

constexpr const char* kColumns =
    "i.item_id,i.from_user_id,a.username,i.communication_id,i.title_id,i.message,i.data_name,"
    "i.data_description,i.localized_names,i.localized_descriptions,i.attachment_type,i.url,"
    "i.platforms,i.created_at,i.expires_at,r.data_used";

QString Key(const QString& s) {
    return s.isNull() ? QStringLiteral("") : s;
}

QString ToJson(const QHash<QString, QString>& map) {
    QJsonObject o;
    for (auto it = map.cbegin(); it != map.cend(); ++it)
        o.insert(it.key(), it.value());
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QHash<QString, QString> FromJson(const QString& text) {
    QHash<QString, QString> out;
    const QJsonObject o = QJsonDocument::fromJson(text.toUtf8()).object();
    for (auto it = o.constBegin(); it != o.constEnd(); ++it)
        out.insert(it.key(), it.value().toString());
    return out;
}

GameCustomDataItem FromRow(const QSqlQuery& q) {
    GameCustomDataItem it;
    it.itemId = q.value(0).toLongLong();
    it.fromUserId = q.value(1).toLongLong();
    it.fromNpid = q.value(2).toString();
    it.communicationId = q.value(3).toString();
    it.titleId = q.value(4).toString();
    it.message = q.value(5).toString();
    it.dataName = q.value(6).toString();
    it.dataDescription = q.value(7).toString();
    it.localizedNames = FromJson(q.value(8).toString());
    it.localizedDescriptions = FromJson(q.value(9).toString());
    it.attachmentType = q.value(10).toString();
    it.url = q.value(11).toString();
    it.platforms = q.value(12).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
    it.createdAt = q.value(13).toLongLong();
    if (!q.value(14).isNull())
        it.expiresAt = q.value(14).toLongLong();
    it.used = q.value(15).toInt() != 0;
    return it;
}

QString ScopeWhere(const GameCustomDataScope& scope, QVariantList& binds) {
    if (scope.communicationId.isEmpty() && scope.titleId.isEmpty())
        return QStringLiteral("1");
    binds << Key(scope.communicationId) << Key(scope.titleId);
    return QStringLiteral("((i.communication_id <> '' AND i.communication_id = ?) OR "
                          "(i.title_id <> '' AND i.title_id = ?))");
}

QString AppendItemId(const QString& url, int64_t itemId) {
    QString out = url;
    const int hash = out.indexOf(QLatin1Char('#'));
    const QString fragment = hash >= 0 ? out.mid(hash) : QString();
    if (hash >= 0)
        out.truncate(hash);
    out += out.contains(QLatin1Char('?')) ? QLatin1Char('&') : QLatin1Char('?');
    out += QStringLiteral("itemId=") + QString::number(itemId);
    return out + fragment;
}

} // namespace

std::optional<int64_t> GameCustomDataDb::Create(GameCustomDataItem item, bool withItemId,
                                                const QByteArray& gameData,
                                                const QByteArray& thumbnail,
                                                const QVector<int64_t>& recipients) {
    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        return std::nullopt;
    }
    const auto fail = [this](const QSqlQuery& q) -> std::optional<int64_t> {
        m_lastError = q.lastError().text();
        qCritical() << "GameCustomDataDb: insert failed:" << m_lastError;
        m_db.rollback();
        return std::nullopt;
    };

    QSqlQuery ins(m_db);
    ins.prepare("INSERT INTO game_custom_data(from_user_id,communication_id,title_id,message,"
                "data_name,data_description,localized_names,localized_descriptions,"
                "attachment_type,url,game_data,thumbnail,platforms,created_at,expires_at) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    ins.addBindValue(QVariant::fromValue<qlonglong>(item.fromUserId));
    ins.addBindValue(Key(item.communicationId));
    ins.addBindValue(Key(item.titleId));
    ins.addBindValue(Key(item.message));
    ins.addBindValue(Key(item.dataName));
    ins.addBindValue(Key(item.dataDescription));
    ins.addBindValue(ToJson(item.localizedNames));
    ins.addBindValue(ToJson(item.localizedDescriptions));
    ins.addBindValue(item.attachmentType);
    ins.addBindValue(item.IsUrl() ? QVariant(Key(item.url)) : QVariant());
    ins.addBindValue(item.IsUrl() ? QVariant() : QVariant(gameData));
    ins.addBindValue(thumbnail);
    ins.addBindValue(item.platforms.join(QLatin1Char(',')));
    ins.addBindValue(QVariant::fromValue<qlonglong>(item.createdAt));
    ins.addBindValue(item.expiresAt ? QVariant::fromValue<qlonglong>(*item.expiresAt) : QVariant());
    if (!ins.exec())
        return fail(ins);
    const int64_t itemId = ins.lastInsertId().toLongLong();

    if (item.IsUrl() && withItemId) {
        QSqlQuery up(m_db);
        up.prepare("UPDATE game_custom_data SET url=? WHERE item_id=?");
        up.addBindValue(AppendItemId(item.url, itemId));
        up.addBindValue(QVariant::fromValue<qlonglong>(itemId));
        if (!up.exec())
            return fail(up);
    }

    for (const int64_t to : recipients) {
        QSqlQuery r(m_db);
        r.prepare("INSERT OR IGNORE INTO game_custom_data_recipient(item_id,user_id,data_used) "
                  "VALUES(?,?,0)");
        r.addBindValue(QVariant::fromValue<qlonglong>(itemId));
        r.addBindValue(QVariant::fromValue<qlonglong>(to));
        if (!r.exec())
            return fail(r);
    }
    if (!m_db.commit()) {
        m_lastError = m_db.lastError().text();
        m_db.rollback();
        return std::nullopt;
    }
    for (const int64_t to : recipients)
        PruneReceived(to);
    return itemId;
}

QVector<GameCustomDataItem> GameCustomDataDb::ListReceived(int64_t userId,
                                                           const GameCustomDataScope& scope) {
    QVariantList binds{QVariant::fromValue<qlonglong>(userId)};
    const QString scopeWhere = ScopeWhere(scope, binds);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM game_custom_data_recipient r "
                             "JOIN game_custom_data i ON i.item_id = r.item_id "
                             "LEFT JOIN account a ON a.user_id = i.from_user_id "
                             "WHERE r.user_id = ? AND %2 ORDER BY i.item_id DESC")
                  .arg(QString::fromLatin1(kColumns), scopeWhere));
    for (const QVariant& b : binds)
        q.addBindValue(b);
    QVector<GameCustomDataItem> out;
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        qCritical() << "GameCustomDataDb: list failed:" << m_lastError;
        return out;
    }
    while (q.next())
        out.append(FromRow(q));
    return out;
}

std::optional<GameCustomDataItem> GameCustomDataDb::GetReceived(int64_t userId, int64_t itemId,
                                                                const GameCustomDataScope& scope) {
    QVariantList binds{QVariant::fromValue<qlonglong>(userId),
                       QVariant::fromValue<qlonglong>(itemId)};
    const QString scopeWhere = ScopeWhere(scope, binds);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM game_custom_data_recipient r "
                             "JOIN game_custom_data i ON i.item_id = r.item_id "
                             "LEFT JOIN account a ON a.user_id = i.from_user_id "
                             "WHERE r.user_id = ? AND r.item_id = ? AND %2")
                  .arg(QString::fromLatin1(kColumns), scopeWhere));
    for (const QVariant& b : binds)
        q.addBindValue(b);
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        qCritical() << "GameCustomDataDb: get failed:" << m_lastError;
        return std::nullopt;
    }
    if (!q.next())
        return std::nullopt;
    return FromRow(q);
}

std::optional<QByteArray> GameCustomDataDb::GameData(int64_t itemId) {
    QSqlQuery q(m_db);
    q.prepare("SELECT game_data FROM game_custom_data WHERE item_id=?");
    q.addBindValue(QVariant::fromValue<qlonglong>(itemId));
    if (!q.exec() || !q.next() || q.value(0).isNull())
        return std::nullopt;
    return q.value(0).toByteArray();
}

std::optional<QByteArray> GameCustomDataDb::Thumbnail(int64_t itemId) {
    QSqlQuery q(m_db);
    q.prepare("SELECT thumbnail FROM game_custom_data WHERE item_id=?");
    q.addBindValue(QVariant::fromValue<qlonglong>(itemId));
    if (!q.exec() || !q.next() || q.value(0).isNull())
        return std::nullopt;
    return q.value(0).toByteArray();
}

bool GameCustomDataDb::MarkUsed(int64_t userId, int64_t itemId) {
    QSqlQuery q(m_db);
    q.prepare("UPDATE game_custom_data_recipient SET data_used=1 WHERE user_id=? AND item_id=? "
              "AND data_used=0");
    q.addBindValue(QVariant::fromValue<qlonglong>(userId));
    q.addBindValue(QVariant::fromValue<qlonglong>(itemId));
    if (!q.exec()) {
        m_lastError = q.lastError().text();
        return false;
    }
    return q.numRowsAffected() == 1;
}

void GameCustomDataDb::PruneReceived(int64_t userId) {
    QSqlQuery q(m_db);
    q.prepare("DELETE FROM game_custom_data_recipient WHERE user_id=? AND item_id NOT IN ("
              "SELECT item_id FROM game_custom_data_recipient WHERE user_id=? "
              "ORDER BY item_id DESC LIMIT ?)");
    q.addBindValue(QVariant::fromValue<qlonglong>(userId));
    q.addBindValue(QVariant::fromValue<qlonglong>(userId));
    q.addBindValue(kMaxReceivedPerUser);
    if (!q.exec()) {
        qWarning() << "GameCustomDataDb: prune failed:" << q.lastError().text();
        return;
    }
    if (q.numRowsAffected() > 0) {
        QSqlQuery orphan(m_db);
        orphan.exec("DELETE FROM game_custom_data WHERE item_id NOT IN "
                    "(SELECT item_id FROM game_custom_data_recipient)");
    }
}
