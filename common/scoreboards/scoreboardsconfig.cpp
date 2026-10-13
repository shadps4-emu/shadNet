// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "scoreboardsconfig.h"

#include <QCoreApplication>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <limits>

namespace {

	QString tr(const char* text) {
		return QCoreApplication::translate("ScoreboardsConfig", text);
	}

	bool unsignedNumber(const QString& text, quint64 maximum, quint64& result) {
		const QString trimmed = text.trimmed();
		if (trimmed.isEmpty() || trimmed.size() > 20)
			return false;
		for (QChar c : trimmed) {
			if (c < QLatin1Char('0') || c > QLatin1Char('9'))
				return false;
		}
		bool ok = false;
		const quint64 value = trimmed.toULongLong(&ok);
		if (!ok || value > maximum)
			return false;
		result = value;
		return true;
	}

	constexpr quint32 kMaxBoardId = std::numeric_limits<quint32>::max();
	constexpr quint64 kMaxRangeSpan = 10000;

} // namespace

bool isValidCommunicationId(const QString& comId) {
	static const QRegularExpression re(QStringLiteral("^[A-Z]{4}[0-9]{5}_[0-9]{2}$"));
	return re.match(comId).hasMatch();
}

bool parseBoardIds(const QString& text, QVector<quint32>& ids, QString* error) {
	ids.clear();
	QSet<quint32> seen;
	const QStringList parts = text.split(QLatin1Char(','));
	for (const QString& raw : parts) {
		const QString part = raw.trimmed();
		quint64 first = 0, last = 0;
		const int dash = part.indexOf(QLatin1Char('-'));
		const bool ok = dash < 0 ? unsignedNumber(part, kMaxBoardId, first)
			: unsignedNumber(part.left(dash), kMaxBoardId, first) &&
			unsignedNumber(part.mid(dash + 1), kMaxBoardId, last);
		if (dash < 0)
			last = first;
		if (!ok || last < first || last - first >= kMaxRangeSpan) {
			if (error)
				*error = tr("Board IDs must be whole numbers or ranges such as 1,3,5-7.");
			return false;
		}
		for (quint64 id = first; id <= last; ++id) {
			if (seen.contains(static_cast<quint32>(id))) {
				if (error)
					*error = tr("Board %1 is listed more than once.").arg(id);
				return false;
			}
			seen.insert(static_cast<quint32>(id));
			ids.append(static_cast<quint32>(id));
		}
	}
	if (ids.isEmpty()) {
		if (error)
			*error = tr("List at least one board ID.");
		return false;
	}
	return true;
}

QString formatBoardIds(const QVector<quint32>& ids) {
	QStringList out;
	out.reserve(ids.size());
	for (quint32 id : ids)
		out.append(QString::number(id));
	return out.join(QLatin1Char(','));
}

ScoreboardsConfig parseScoreboardsConfig(const QString& text) {
	ScoreboardsConfig result;
	if (text.toUtf8().size() > kMaxScoreboardsBytes || text.contains(QChar(0))) {
		result.errors.append({ 1, tr("Configuration is too large or contains a NUL character.") });
		return result;
	}
	QHash<QString, QHash<quint32, int>> boardLines; // comId -> board -> line
	QString pendingLabel;
	const QStringList lines = text.split(QLatin1Char('\n'));
	for (int i = 0; i < lines.size(); ++i) {
		QString line = lines[i].trimmed();
		if (i == 0 && line.startsWith(QChar(0xfeff)))
			line = line.mid(1).trimmed();
		if (line.isEmpty()) {
			pendingLabel.clear();
			continue;
		}
		if (line.startsWith(QLatin1Char('#'))) {
			pendingLabel = line.mid(1).trimmed();
			continue;
		}
		const QString label = pendingLabel;
		pendingLabel.clear();
		auto error = [&](const QString& message) { result.errors.append({ i + 1, message }); };

		const QStringList parts = line.split(QLatin1Char('|'));
		if (parts.size() != 7) {
			error(tr("Expected "
				"COMMUNICATION_ID|BOARD_IDS|RANK_LIMIT|UPDATE_MODE|SORT_MODE|"
				"UPLOAD_NUM_LIMIT|UPLOAD_SIZE_LIMIT."));
			continue;
		}
		ScoreboardEntry entry;
		entry.label = label;
		entry.line = i + 1;
		entry.comId = parts[0].trimmed();
		if (!isValidCommunicationId(entry.comId)) {
			error(tr("Communication ID must look like NPWR12345_00."));
			continue;
		}
		QString boardError;
		if (!parseBoardIds(parts[1], entry.boardIds, &boardError)) {
			error(boardError);
			continue;
		}
		quint64 rankLimit = 0, uploadNum = 0, uploadSize = 0;
		if (!unsignedNumber(parts[2], std::numeric_limits<quint32>::max(), rankLimit) ||
			rankLimit == 0) {
			error(tr("Rank limit must be a whole number from 1 to %1.")
				.arg(std::numeric_limits<quint32>::max()));
			continue;
		}
		const QString updateMode = parts[3].trimmed();
		if (updateMode != QLatin1String("NORMAL_UPDATE") &&
			updateMode != QLatin1String("FORCE_UPDATE")) {
			error(tr("Update mode must be NORMAL_UPDATE or FORCE_UPDATE."));
			continue;
		}
		const QString sortMode = parts[4].trimmed();
		if (sortMode != QLatin1String("DESCENDING_ORDER") &&
			sortMode != QLatin1String("ASCENDING_ORDER")) {
			error(tr("Sort mode must be DESCENDING_ORDER or ASCENDING_ORDER."));
			continue;
		}
		if (!unsignedNumber(parts[5], std::numeric_limits<quint32>::max(), uploadNum)) {
			error(tr("Upload limit must be a whole number from 0 to %1.")
				.arg(std::numeric_limits<quint32>::max()));
			continue;
		}
		if (!unsignedNumber(parts[6], std::numeric_limits<quint64>::max(), uploadSize)) {
			error(tr("Upload size limit must be a whole number of bytes."));
			continue;
		}
		bool duplicate = false;
		for (quint32 id : entry.boardIds) {
			const int earlier = boardLines[entry.comId].value(id, 0);
			if (earlier) {
				error(tr("Board %1 of %2 is already configured on line %3.")
					.arg(id)
					.arg(entry.comId)
					.arg(earlier));
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			continue;
		for (quint32 id : entry.boardIds)
			boardLines[entry.comId].insert(id, i + 1);
		entry.rankLimit = static_cast<quint32>(rankLimit);
		entry.forceUpdate = updateMode == QLatin1String("FORCE_UPDATE");
		entry.ascending = sortMode == QLatin1String("ASCENDING_ORDER");
		entry.uploadNumLimit = static_cast<quint32>(uploadNum);
		entry.uploadSizeLimit = uploadSize;
		result.entries.append(entry);
	}
	return result;
}

QString serializeScoreboardsConfig(const ScoreboardsConfig& config) {
	QString text =
		QStringLiteral("#Syntax is "
			"communication_id|board_ids|rank_limit|update_mode|sort_mode|upload_num_"
			"limit|upload_"
			"size_limit\n"
			"#board_ids can be multiple board ids separated by a comma\n"
			"#update_mode values:\n"
			"#NORMAL_UPDATE\n"
			"#FORCE_UPDATE\n"
			"#sort_mode values:\n"
			"#DESCENDING_ORDER\n"
			"#ASCENDING_ORDER\n");
	for (const auto& e : config.entries) {
		text += QLatin1Char('\n');
		QString label = e.label;
		label.replace(QLatin1Char('\r'), QLatin1Char(' '))
			.replace(QLatin1Char('\n'), QLatin1Char(' '));
		if (!label.trimmed().isEmpty())
			text += QLatin1Char('#') + label.trimmed() + QLatin1Char('\n');
		text += QStringLiteral("%1|%2|%3|%4|%5|%6|%7\n")
			.arg(e.comId, formatBoardIds(e.boardIds))
			.arg(e.rankLimit)
			.arg(e.forceUpdate ? QStringLiteral("FORCE_UPDATE")
				: QStringLiteral("NORMAL_UPDATE"),
				e.ascending ? QStringLiteral("ASCENDING_ORDER")
				: QStringLiteral("DESCENDING_ORDER"))
			.arg(e.uploadNumLimit)
			.arg(e.uploadSizeLimit);
	}
	return text;
}
