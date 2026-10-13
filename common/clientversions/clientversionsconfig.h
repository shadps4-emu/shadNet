// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

struct ClientVersionRule {
	enum class Match { Empty, Any, Exact, Prefix, Regex };
	bool allow = true;
	Match match = Match::Any;
	QString text; // exact / prefix text, or the regex pattern
	QString message;
	QString note; // comment lines directly above the rule, without "# "
	int line = 0; // in the file it was read from; 0 for a rule made in the editor
	QRegularExpression regex;
};

struct ClientVersionsParseError {
	int line = 0;
	QString message;
};

struct ClientVersionsConfig {
	QStringList header; // the comment block at the top of the file, verbatim
	QVector<ClientVersionRule> rules;
	QStringList trailer; // comments after the last rule, verbatim
	QVector<ClientVersionsParseError> errors;
	bool isValid() const {
		return errors.isEmpty();
	}
};

struct ClientVersionDecision {
	bool allowed = true;
	QString message; // for a denial
	int rule = -1;   // index in rules of the rule that decided, -1 = no rule matched
	int line = 0;    // its line in the file, 0 = no rule matched
};

ClientVersionsConfig parseClientVersionsConfig(const QString& text);
QString serializeClientVersionsConfig(const ClientVersionsConfig& config);
QString clientVersionRuleLine(const ClientVersionRule& rule);
QString validateClientVersionRule(ClientVersionRule& rule);
ClientVersionDecision evaluateClientVersion(const QVector<ClientVersionRule>& rules,
	const QString& version);
QString clientVersionMatchName(ClientVersionRule::Match match);
QString defaultClientVersionDenyMessage();
QString defaultClientVersionsContent();
