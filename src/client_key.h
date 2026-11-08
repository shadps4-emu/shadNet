// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QHostAddress>
#include <QRegularExpression>
#include <QString>

inline QString ThrottleKey(const QString& address) {
    const QString text = address.trimmed();
    static const QRegularExpression canonicalV4(
        QStringLiteral("^(?:(?:25[0-5]|2[0-4]\\d|1\\d\\d|[1-9]?\\d)\\.){3}(?:25[0-5]|2[0-4]\\d|"
                       "1\\d\\d|[1-9]?\\d)$"));
    if (!text.contains(QLatin1Char(':')) && !canonicalV4.match(text).hasMatch())
        return QString();

    const QHostAddress a(text);
    if (a.isNull())
        return QString();

    bool isV4 = false;
    const quint32 v4 = a.toIPv4Address(&isV4); // also true for IPv4-mapped IPv6
    if (isV4)
        return QHostAddress(v4).toString();

    Q_IPV6ADDR bytes = a.toIPv6Address();
    for (int i = 8; i < 16; ++i)
        bytes[i] = 0;
    return QHostAddress(bytes).toString() + QStringLiteral("/64");
}

inline QString ResolveClientKey(const QString& tcpPeer, const QString& forwarded,
                                bool forwardedTrusted) {
    if (forwardedTrusted) {
        const QString key = ThrottleKey(forwarded);
        if (!key.isEmpty())
            return key;
    }
    const QString key = ThrottleKey(tcpPeer);
    return key.isEmpty() ? tcpPeer : key;
}
