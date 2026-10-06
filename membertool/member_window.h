// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QWidget>
#include "member_store.h"

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

class MemberWindow : public QWidget {
public:
    explicit MemberWindow(const QString& databasePath = {}, QWidget* parent = nullptr);
    bool OpenDatabase(const QString& path);
    void OpenWorlds(const QString& path);

private:
    void Refresh();
    void Filter();
    void AddMember();
    void RemoveMember();

    MemberStore m_store;
    QLineEdit* m_path;
    QLineEdit* m_username;
    QLineEdit* m_email;
    QLineEdit* m_search;
    QPlainTextEdit* m_credentials;
    QPushButton* m_add;
    QPushButton* m_remove;
    QPushButton* m_refresh;
    QPushButton* m_copy;
    QTableWidget* m_members;
    QLabel* m_status;
};
