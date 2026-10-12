// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

// Which client versions may stay signed in, from clientversions.cfg. One rule per line, checked
// top to bottom; the first rule that matches decides. No match: allowed.
//
//   ACTION MATCH [ARGUMENT] [| MESSAGE]
//
//   ACTION   allow | deny
//   MATCH    empty            no version reported (or an empty one)
//            any              every version
//            exact  TEXT      the whole version string equals TEXT
//            prefix TEXT      the version starts with TEXT
//            regex  PATTERN   the version matches PATTERN (Qt/PCRE syntax)
//   MESSAGE  shown to a denied player; a default is used when missing
//
// Lines starting with '#' and blank lines are ignored.
class ClientVersionRules {
public:
    struct Decision {
        bool allowed = true;
        QString message; // for a denial
        int line = 0;    // the rule that decided, 0 = no rule matched
    };

    // Reads path; a missing file is created with the default rules (deny empty, allow the
    // rest). Errors (unreadable file, bad line) are returned; bad lines are skipped.
    bool Load(const QString& path, QStringList& errors);
    Decision Evaluate(const QString& version) const;
    int RuleCount() const {
        return static_cast<int>(m_rules.size());
    }

    static QString DefaultFileContent();

private:
    enum class Match { Empty, Any, Exact, Prefix, Regex };
    struct Rule {
        bool allow = true;
        Match match = Match::Any;
        QString text;
        QRegularExpression regex;
        QString message;
        int line = 0;
    };
    QVector<Rule> m_rules;
};
