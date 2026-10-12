// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "client_version_rules.h"

#include <QFile>
#include <QSaveFile>
#include <QStringDecoder>

namespace {

const QString kDefaultDenyMessage =
    QStringLiteral("This shadNet server doesn't accept your shadPS4 build. Please update shadPS4.");

} // namespace

QString ClientVersionRules::DefaultFileContent() {
    return QStringLiteral(
        "# clientversions.cfg - which client versions may sign in to this shadNet server.\n"
        "# One rule per line, checked top to bottom; the first match decides. No match: "
        "allowed.\n"
        "#\n"
        "#   allow|deny  empty|any|exact TEXT|prefix TEXT|regex PATTERN  [| message to the "
        "player]\n"
        "#\n"
        "# The version is what the emulator reports after signing in, for example\n"
        "#   shadPS4 v0.19.1 WIP main-abc1234\n"
        "# A client that reports nothing (older builds) counts as empty.\n"
        "#\n"
        "# Examples:\n"
        "#   deny prefix shadPS4 v0.18 | shadPS4 0.18 is no longer supported. Please update.\n"
        "#   deny regex ^shadPS4 v0\\.1[0-7]\\. | Please update shadPS4.\n"
        "\n"
        "deny empty | Your shadPS4 build is too old for this shadNet server. Please update "
        "shadPS4.\n"
        "allow any\n");
}

bool ClientVersionRules::Load(const QString& path, QStringList& errors) {
    m_rules.clear();
    QFile file(path);
    if (!file.exists()) {
        QSaveFile out(path);
        const QByteArray bytes = DefaultFileContent().toUtf8();
        if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
            errors.append(QStringLiteral("Cannot create %1: %2").arg(path, out.errorString()));
            return false;
        }
    }
    if (!file.open(QIODevice::ReadOnly)) {
        errors.append(QStringLiteral("Cannot read %1: %2").arg(path, file.errorString()));
        return false;
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder(file.readAll());
    if (decoder.hasError()) {
        errors.append(QStringLiteral("%1 must be UTF-8 text.").arg(path));
        return false;
    }

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i].trimmed();
        if (i == 0 && line.startsWith(QChar(0xfeff)))
            line = line.mid(1).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        auto bad = [&](const QString& why) {
            errors.append(QStringLiteral("%1 line %2: %3").arg(path).arg(i + 1).arg(why));
        };

        Rule rule;
        rule.line = i + 1;
        // "... | message": the message is everything after the first " | ".
        const int bar = line.indexOf(QStringLiteral(" | "));
        QString head = line;
        if (bar >= 0) {
            rule.message = line.mid(bar + 3).trimmed();
            head = line.left(bar).trimmed();
        }
        const QString action = head.section(QLatin1Char(' '), 0, 0, QString::SectionSkipEmpty);
        const QString match =
            head.section(QLatin1Char(' '), 1, 1, QString::SectionSkipEmpty).toLower();
        const int argStart = [&]() {
            int pos = head.indexOf(action) + action.size();
            pos = head.indexOf(match, pos, Qt::CaseInsensitive) + match.size();
            return pos;
        }();
        const QString argument = head.mid(argStart).trimmed();

        if (action.compare(QLatin1String("allow"), Qt::CaseInsensitive) == 0)
            rule.allow = true;
        else if (action.compare(QLatin1String("deny"), Qt::CaseInsensitive) == 0)
            rule.allow = false;
        else {
            bad(QStringLiteral("start the rule with allow or deny."));
            continue;
        }
        if (match == QLatin1String("empty") || match == QLatin1String("any")) {
            rule.match = match == QLatin1String("empty") ? Match::Empty : Match::Any;
            if (!argument.isEmpty()) {
                bad(QStringLiteral("'%1' takes no argument.").arg(match));
                continue;
            }
        } else if (match == QLatin1String("exact") || match == QLatin1String("prefix")) {
            rule.match = match == QLatin1String("exact") ? Match::Exact : Match::Prefix;
            if (argument.isEmpty()) {
                bad(QStringLiteral("'%1' needs the text to compare with.").arg(match));
                continue;
            }
            rule.text = argument;
        } else if (match == QLatin1String("regex")) {
            rule.match = Match::Regex;
            rule.regex = QRegularExpression(argument);
            if (argument.isEmpty() || !rule.regex.isValid()) {
                bad(QStringLiteral("invalid regular expression: %1")
                        .arg(argument.isEmpty() ? QStringLiteral("(empty)")
                                                : rule.regex.errorString()));
                continue;
            }
        } else {
            bad(QStringLiteral("match with empty, any, exact, prefix or regex."));
            continue;
        }
        m_rules.append(rule);
    }
    return true;
}

ClientVersionRules::Decision ClientVersionRules::Evaluate(const QString& version) const {
    for (const Rule& rule : m_rules) {
        bool hit = false;
        switch (rule.match) {
        case Match::Empty:
            hit = version.isEmpty();
            break;
        case Match::Any:
            hit = true;
            break;
        case Match::Exact:
            hit = version == rule.text;
            break;
        case Match::Prefix:
            hit = version.startsWith(rule.text);
            break;
        case Match::Regex:
            hit = rule.regex.match(version).hasMatch();
            break;
        }
        if (hit)
            return {rule.allow,
                    rule.allow ? QString()
                               : (rule.message.isEmpty() ? kDefaultDenyMessage : rule.message),
                    rule.line};
    }
    return {};
}
