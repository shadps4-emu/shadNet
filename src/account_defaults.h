// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDate>
#include <QString>

// Account attributes shadNet does not store yet. Every place that reports them (profile and
// account WebAPI responses, access token validation, ID tokens) uses these, so a game sees the
// same values whichever API it asks.
namespace AccountDefaults {

// Lower-case ISO 3166-1 alpha-2. Some responses want it upper-case.
inline constexpr char Region[] = "us";
inline constexpr char Language[] = "en";
inline constexpr char CommunityDomain[] = "a0";
// Placeholder date of birth: January 1st of this year.
inline constexpr int BirthYear = 1990;

inline QDate DateOfBirth() {
    return QDate(BirthYear, 1, 1);
}

inline int Age() {
    const QDate today = QDate::currentDate();
    const QDate dob = DateOfBirth();
    int age = today.year() - dob.year();
    if (today.month() < dob.month() || (today.month() == dob.month() && today.day() < dob.day()))
        --age;
    return age;
}

} // namespace AccountDefaults
