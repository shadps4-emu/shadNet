// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "clientversionsconfig.h"

namespace {

	const QString kSeparator = QStringLiteral(" | ");

	bool isComment(const QString& trimmed) {
		return trimmed.startsWith(QLatin1Char('#'));
	}

	QString commentText(const QString& trimmed) {
		QString text = trimmed.mid(1);
		if (text.startsWith(QLatin1Char(' ')))
			text = text.mid(1);
		return text;
	}

} // namespace

QString clientVersionMatchName(ClientVersionRule::Match match) {
	switch (match) {
	case ClientVersionRule::Match::Empty:
		return QStringLiteral("empty");
	case ClientVersionRule::Match::Any:
		return QStringLiteral("any");
	case ClientVersionRule::Match::Exact:
		return QStringLiteral("exact");
	case ClientVersionRule::Match::Prefix:
		return QStringLiteral("prefix");
	case ClientVersionRule::Match::Regex:
		return QStringLiteral("regex");
	}
	return {};
}

QString defaultClientVersionDenyMessage() {
	return QStringLiteral(
		"This shadNet server doesn't accept your shadPS4 build. Please update shadPS4.");
}

QString defaultClientVersionsContent() {
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

QString validateClientVersionRule(ClientVersionRule& rule) {
	const bool takesText = rule.match == ClientVersionRule::Match::Exact ||
		rule.match == ClientVersionRule::Match::Prefix ||
		rule.match == ClientVersionRule::Match::Regex;
	if (takesText && rule.text.trimmed().isEmpty())
		return rule.match == ClientVersionRule::Match::Regex
		? QStringLiteral("'regex' needs a pattern.")
		: QStringLiteral("'%1' needs the text to compare with.")
		.arg(clientVersionMatchName(rule.match));
	if (!takesText && !rule.text.isEmpty())
		return QStringLiteral("'%1' takes no argument.").arg(clientVersionMatchName(rule.match));
	if (rule.text != rule.text.trimmed())
		return QStringLiteral("The text can't start or end with a space.");
	if (rule.text.contains(kSeparator) || rule.text.contains(QLatin1Char('\n')))
		return QStringLiteral("The text can't contain \" | \" or a line break.");
	if (rule.message.contains(QLatin1Char('\n')) || rule.message.contains(QLatin1Char('\r')))
		return QStringLiteral("The message must be a single line.");
	if (rule.match == ClientVersionRule::Match::Regex) {
		rule.regex = QRegularExpression(rule.text);
		if (!rule.regex.isValid())
			return QStringLiteral("Invalid regular expression: %1").arg(rule.regex.errorString());
	}
	return {};
}

ClientVersionsConfig parseClientVersionsConfig(const QString& input) {
	ClientVersionsConfig config;
	QString text = input;
	if (text.startsWith(QChar(0xfeff)))
		text = text.mid(1);
	const QStringList lines = text.split(QLatin1Char('\n'));

	QStringList pending; // comment/blank lines seen since the last rule, verbatim
	bool sawRule = false;
	for (int i = 0; i < lines.size(); ++i) {
		QString raw = lines[i];
		if (raw.endsWith(QLatin1Char('\r')))
			raw.chop(1);
		const QString line = raw.trimmed();
		if (line.isEmpty() || isComment(line)) {
			pending.append(raw);
			continue;
		}
		auto bad = [&](const QString& why) { config.errors.append({ i + 1, why }); };
		int noteStart = pending.size();
		while (noteStart > 0 && isComment(pending[noteStart - 1].trimmed()))
			--noteStart;
		QStringList noteLines;
		for (int n = noteStart; n < pending.size(); ++n)
			noteLines.append(commentText(pending[n].trimmed()));
		if (!sawRule) {
			config.header = pending.mid(0, noteStart);
			while (!config.header.isEmpty() && config.header.last().trimmed().isEmpty())
				config.header.removeLast();
		}
		pending.clear();
		sawRule = true;

		ClientVersionRule rule;
		rule.line = i + 1;
		rule.note = noteLines.join(QLatin1Char('\n'));
		QString head = line;
		const int bar = line.indexOf(kSeparator);
		if (bar >= 0) {
			rule.message = line.mid(bar + kSeparator.size()).trimmed();
			head = line.left(bar).trimmed();
		}
		const QString action = head.section(QLatin1Char(' '), 0, 0, QString::SectionSkipEmpty);
		const QString match =
			head.section(QLatin1Char(' '), 1, 1, QString::SectionSkipEmpty).toLower();
		int argStart = head.indexOf(action) + action.size();
		argStart = head.indexOf(match, argStart, Qt::CaseInsensitive) + match.size();
		rule.text = match.isEmpty() ? QString() : head.mid(argStart).trimmed();

		if (action.compare(QLatin1String("allow"), Qt::CaseInsensitive) == 0) {
			rule.allow = true;
		}
		else if (action.compare(QLatin1String("deny"), Qt::CaseInsensitive) == 0) {
			rule.allow = false;
		}
		else {
			bad(QStringLiteral("start the rule with allow or deny."));
			continue;
		}
		if (match == QLatin1String("empty"))
			rule.match = ClientVersionRule::Match::Empty;
		else if (match == QLatin1String("any"))
			rule.match = ClientVersionRule::Match::Any;
		else if (match == QLatin1String("exact"))
			rule.match = ClientVersionRule::Match::Exact;
		else if (match == QLatin1String("prefix"))
			rule.match = ClientVersionRule::Match::Prefix;
		else if (match == QLatin1String("regex"))
			rule.match = ClientVersionRule::Match::Regex;
		else {
			bad(QStringLiteral("match with empty, any, exact, prefix or regex."));
			continue;
		}
		if (const QString why = validateClientVersionRule(rule); !why.isEmpty()) {
			bad(why);
			continue;
		}
		config.rules.append(rule);
	}
	if (!sawRule) {
		config.header = pending;
		while (!config.header.isEmpty() && config.header.last().trimmed().isEmpty())
			config.header.removeLast();
	}
	else {
		for (const QString& raw : pending)
			if (isComment(raw.trimmed()))
				config.trailer.append(raw);
	}
	return config;
}

QString clientVersionRuleLine(const ClientVersionRule& rule) {
	QString line = rule.allow ? QStringLiteral("allow ") : QStringLiteral("deny ");
	line += clientVersionMatchName(rule.match);
	if (!rule.text.isEmpty())
		line += QLatin1Char(' ') + rule.text;
	if (!rule.message.trimmed().isEmpty())
		line += kSeparator + rule.message.trimmed();
	return line;
}

QString serializeClientVersionsConfig(const ClientVersionsConfig& config) {
	QStringList out = config.header;
	if (!out.isEmpty())
		out.append(QString());
	for (const ClientVersionRule& rule : config.rules) {
		if (!rule.note.isEmpty()) {
			for (const QString& noteLine : rule.note.split(QLatin1Char('\n')))
				out.append(noteLine.isEmpty() ? QStringLiteral("#")
					: QStringLiteral("# ") + noteLine);
		}
		out.append(clientVersionRuleLine(rule));
	}
	if (!config.trailer.isEmpty()) {
		out.append(QString());
		out.append(config.trailer);
	}
	return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

ClientVersionDecision evaluateClientVersion(const QVector<ClientVersionRule>& rules,
	const QString& version) {
	for (int i = 0; i < rules.size(); ++i) {
		const ClientVersionRule& rule = rules[i];
		bool hit = false;
		switch (rule.match) {
		case ClientVersionRule::Match::Empty:
			hit = version.isEmpty();
			break;
		case ClientVersionRule::Match::Any:
			hit = true;
			break;
		case ClientVersionRule::Match::Exact:
			hit = version == rule.text;
			break;
		case ClientVersionRule::Match::Prefix:
			hit = version.startsWith(rule.text);
			break;
		case ClientVersionRule::Match::Regex:
			hit = rule.regex.match(version).hasMatch();
			break;
		}
		if (!hit)
			continue;
		ClientVersionDecision d;
		d.allowed = rule.allow;
		if (!rule.allow)
			d.message = rule.message.isEmpty() ? defaultClientVersionDenyMessage() : rule.message;
		d.rule = i;
		d.line = rule.line;
		return d;
	}
	return {};
}
