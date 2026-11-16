// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>

// Account avatars.
//
// The account table stores an avatar *name* such as "default_01". Each named avatar is hosted
// under the avatar base URL in every size the User Profile API defines:
//   <base><name>_s.png   50x50
//   <base><name>_m.png  160x160
//   <base><name>_l.png  240x240
// A stored value containing "://" is a custom image URL (legacy rows, or a user-supplied
// picture), it has only one size, so every size resolves to it unchanged.
//
namespace Avatar {

// Base URL of the hosted avatars, ending in '/'. Set from the config (AvatarBaseUrl) at
// startup, before any request is handled; the toolbox keeps the default.
inline QString g_baseUrl = QStringLiteral("https://shadps4.net/shad_net/assets/avatar/");

inline QString DefaultName() {
    return QStringLiteral("default_01");
}

inline bool IsSize(const QString& size) {
    return size == QStringLiteral("s") || size == QStringLiteral("m") ||
           size == QStringLiteral("l");
}

inline bool IsValidName(const QString& name) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return re.match(name).hasMatch();
}

inline bool IsCustomUrl(const QString& stored) {
    return stored.contains(QStringLiteral("://"));
}

// URL without its scheme, for comparing http and https forms of the same address.
inline QString StripScheme(const QString& url) {
    const int sep = url.indexOf(QStringLiteral("://"));
    return sep < 0 ? url : url.mid(sep + 3);
}

// The avatar name inside a hosted-avatar URL, or an empty string when the URL is not one.
// Recognizes the configured base and the older locations the server used to store.
// A size suffix (_s/_m/_l) is dropped, so any size's URL maps back to the same name.
inline QString NameFromUrl(const QString& url) {
    const QStringList bases = {
        StripScheme(g_baseUrl),
        QStringLiteral("shadps4.net/shad_net/assets/avatar/"),
    };
    const QString bare = StripScheme(url.trimmed());
    for (const QString& base : bases) {
        if (base.isEmpty() || !bare.startsWith(base, Qt::CaseInsensitive))
            continue;
        const QString file = bare.mid(base.size());
        if (file.contains(QLatin1Char('/')) ||
            !file.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
            return QString();
        QString name = file.left(file.size() - 4);
        if (name.size() > 2 && name[name.size() - 2] == QLatin1Char('_') && IsSize(name.right(1)))
            name.chop(2);
        return IsValidName(name) ? name : QString();
    }
    return QString();
}

// What to store for an avatar given by a client: a name, a hosted-avatar URL (stored as its
// name), or a custom URL (stored as is; the caller validates it). Empty means the default.
inline QString ToStored(const QString& input) {
    const QString in = input.trimmed();
    if (in.isEmpty())
        return DefaultName();
    if (!IsCustomUrl(in))
        return in;
    const QString name = NameFromUrl(in);
    return name.isEmpty() ? in : name;
}

// The URL of one size of a stored avatar. Size defaults to l, the size the API return
// when none is requested.
inline QString Url(const QString& stored, const QString& size = QStringLiteral("l")) {
    if (stored.isEmpty())
        return QString();
    if (IsCustomUrl(stored))
        return stored;
    return g_baseUrl + stored + QLatin1Char('_') + (IsSize(size) ? size : QStringLiteral("l")) +
           QStringLiteral(".png");
}

} // namespace Avatar
