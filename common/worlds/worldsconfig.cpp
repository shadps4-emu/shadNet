// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "worldsconfig.h"

#include <QCoreApplication>
#include <QMap>
#include <QSet>
#include <QStringList>

#include <limits>

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("WorldsConfig", text);
}

bool unsignedNumber(const QString& text, quint64 maximum, quint32& result) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return false;
    for (QChar c : trimmed) {
        if (c < QLatin1Char('0') || c > QLatin1Char('9'))
            return false;
    }
    bool ok = false;
    const quint64 value = trimmed.toULongLong(&ok);
    if (!ok || value > maximum)
        return false;
    result = static_cast<quint32>(value);
    return true;
}

} // namespace

WorldsConfig parseWorldsConfig(const QString& text) {
    WorldsConfig result;
    if (text.toUtf8().size() > kMaxWorldsBytes || text.contains(QChar(0))) {
        result.errors.append({1, tr("Configuration is too large or contains a NUL character.")});
        return result;
    }
    enum class Section { None, Groups, Worlds } section = Section::None;
    QSet<QString> titleIds;
    QMap<QString, QSet<quint32>> worldIds;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i].trimmed();
        if (i == 0 && line.startsWith(QChar(0xfeff)))
            line = line.mid(1).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        auto error = [&](const QString& message) { result.errors.append({i + 1, message}); };
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            const QString name = line.mid(1, line.size() - 2).trimmed().toLower();
            if (name == QLatin1String("groups"))
                section = Section::Groups;
            else if (name == QLatin1String("worlds"))
                section = Section::Worlds;
            else {
                section = Section::None;
                error(tr("Unknown section. Use [groups] or [worlds]."));
            }
            continue;
        }

        if (section == Section::Groups) {
            const int equals = line.indexOf(QLatin1Char('='));
            const QString title = equals < 0 ? QString() : line.left(equals).trimmed();
            const QString group = equals < 0 ? QString() : line.mid(equals + 1).trimmed();
            if (title.isEmpty() || group.isEmpty()) {
                error(tr("Expected TITLE_ID = group, with both values filled in."));
                continue;
            }
            if (titleIds.contains(title)) {
                error(tr("Title %1 is mapped more than once.").arg(title));
                continue;
            }
            titleIds.insert(title);
            result.groups.append({title, group, i + 1});
        } else if (section == Section::Worlds) {
            const QStringList parts = line.split(QLatin1Char('|'));
            if (parts.size() != 5 || parts[0].trimmed().isEmpty()) {
                error(tr("Expected GROUP | WORLD_ID | SERVER_ID | LOBBIES_NUM | "
                         "MAX_LOBBY_MEMBERS."));
                continue;
            }
            quint32 values[4] = {};
            const QStringList fields = {tr("World ID"), tr("Server ID"), tr("Lobbies"),
                                        tr("Max lobby members")};
            bool valid = true;
            for (int field = 0; field < 4; ++field) {
                const quint64 max = field == 1 ? std::numeric_limits<quint16>::max()
                                               : std::numeric_limits<quint32>::max();
                if (!unsignedNumber(parts[field + 1], max, values[field]) ||
                    (field == 0 && values[field] == 0)) {
                    error(tr("%1 must be a whole number from %2 to %3.")
                              .arg(fields[field])
                              .arg(field == 0 ? 1 : 0)
                              .arg(max));
                    valid = false;
                }
            }
            if (!valid)
                continue;
            const QString group = parts[0].trimmed();
            // The server indexes live rooms by group + world ID, not server ID.
            if (worldIds[group].contains(values[0])) {
                error(tr("World %1 is declared more than once in %2.").arg(values[0]).arg(group));
                continue;
            }
            worldIds[group].insert(values[0]);
            result.worlds.append(
                {group, values[0], static_cast<quint16>(values[1]), values[2], values[3], i + 1});
        } else {
            error(tr("Place this entry under [groups] or [worlds]."));
        }
    }
    return result;
}

QString serializeWorldsConfig(const WorldsConfig& config) {
    QString text = QStringLiteral("# worlds.cfg - matchmaking worlds\n\n[groups]\n");
    for (const auto& mapping : config.groups)
        text += mapping.titleId + QStringLiteral(" = ") + mapping.group + QLatin1Char('\n');
    text += QStringLiteral("\n[worlds]\n"
                           "# GROUP | WORLD_ID | SERVER_ID | LOBBIES_NUM | MAX_LOBBY_MEMBERS\n");
    for (const auto& world : config.worlds) {
        text += QStringLiteral("%1 | %2 | %3 | %4 | %5\n")
                    .arg(world.group)
                    .arg(world.worldId)
                    .arg(world.serverId)
                    .arg(world.lobbies)
                    .arg(world.maxLobbyMembers);
    }
    return text;
}

QJsonObject worldsSnapshotJson(const WorldsSnapshot& s) {
    return {{QStringLiteral("content"), s.content},
            {QStringLiteral("revision"), s.revision},
            {QStringLiteral("activeContent"), s.activeContent},
            {QStringLiteral("activeRevision"), s.activeRevision},
            {QStringLiteral("canReload"), s.canReload},
            {QStringLiteral("exists"), s.exists}};
}

bool worldsSnapshotFromJson(const QJsonObject& json, WorldsSnapshot& s) {
    for (const auto* key : {"content", "revision", "activeContent", "activeRevision"}) {
        if (!json.value(QLatin1String(key)).isString())
            return false;
    }
    if (!json.value(QStringLiteral("canReload")).isBool() ||
        !json.value(QStringLiteral("exists")).isBool())
        return false;
    s = {json.value(QStringLiteral("content")).toString(),
         json.value(QStringLiteral("revision")).toString(),
         json.value(QStringLiteral("activeContent")).toString(),
         json.value(QStringLiteral("activeRevision")).toString(),
         json.value(QStringLiteral("canReload")).toBool(),
         json.value(QStringLiteral("exists")).toBool()};
    return !s.revision.isEmpty() && (!s.canReload || !s.activeRevision.isEmpty()) &&
           s.content.toUtf8().size() <= kMaxWorldsBytes &&
           s.activeContent.toUtf8().size() <= kMaxWorldsBytes;
}
