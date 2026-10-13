// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include <QVector>

struct ScoreboardEntry {
	QString label; // the "#Game name" comment line directly above, without '#'
	QString comId; // NPWR12345_00
	QVector<quint32> boardIds;
	quint32 rankLimit = 100;
	bool forceUpdate = false; // FORCE_UPDATE; otherwise NORMAL_UPDATE (keep the best score)
	bool ascending = false;   // ASCENDING_ORDER (lower is better); otherwise DESCENDING_ORDER
	quint32 uploadNumLimit = 10;
	quint64 uploadSizeLimit = 6000000;
	int line = 0;
};

struct ScoreboardsConfigIssue {
	int line = 0;
	QString message;
};

struct ScoreboardsConfig {
	QVector<ScoreboardEntry> entries;
	QVector<ScoreboardsConfigIssue> errors;

	bool isValid() const {
		return errors.isEmpty();
	}
};

constexpr qsizetype kMaxScoreboardsBytes = 512 * 1024;
ScoreboardsConfig parseScoreboardsConfig(const QString& text);
QString serializeScoreboardsConfig(const ScoreboardsConfig& config);
QString formatBoardIds(const QVector<quint32>& ids);
bool parseBoardIds(const QString& text, QVector<quint32>& ids, QString* error = nullptr);
bool isValidCommunicationId(const QString& comId);
