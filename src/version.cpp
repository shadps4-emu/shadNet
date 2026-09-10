// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "version.h"

#include <QDate>
#include <QLatin1String>
#include <QStringList>

namespace {

constexpr const char* m_version = "0.0.28";

QString IsoBuildDate() {
    static const QStringList months = {
        QStringLiteral("Jan"), QStringLiteral("Feb"), QStringLiteral("Mar"), QStringLiteral("Apr"),
        QStringLiteral("May"), QStringLiteral("Jun"), QStringLiteral("Jul"), QStringLiteral("Aug"),
        QStringLiteral("Sep"), QStringLiteral("Oct"), QStringLiteral("Nov"), QStringLiteral("Dec")};

    const QString raw = QString::fromLatin1(__DATE__);
    const QStringList parts = raw.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() != 3) {
        return raw; // unexpected compiler format; hand it back untouched
    }

    const int month = months.indexOf(parts.at(0)) + 1;
    const int day = parts.at(1).toInt();
    const int year = parts.at(2).toInt();
    const QDate date(year, month, day);
    if (!date.isValid()) {
        return raw;
    }
    return date.toString(QStringLiteral("yyyy-MM-dd"));
}

} // namespace

namespace ShadNet {

QString Version() {
    return QString::fromLatin1(m_version);
}

QString BuildDate() {
    static const QString cached = IsoBuildDate();
    return cached;
}

QString BuildTime() {
    return QString::fromLatin1(__TIME__);
}

QString BuildTimestamp() {
    return BuildDate() + QLatin1Char('T') + BuildTime();
}

} // namespace ShadNet
