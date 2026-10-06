// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>

#include "worldsbackend.h"
#include "worldsconfig.h"

class QLabel;
class QPushButton;
class QTableWidget;
class QTabWidget;

// Table editor for a remote server or a local configuration file.
class WorldsDialog : public QDialog {
    Q_OBJECT
public:
    explicit WorldsDialog(WorldsBackend* backend, QWidget* parent = nullptr);
    // Configuration is generated from the data shown in the tables.
    QString draftText() const;

public slots:
    void reject() override;

private:
    void refreshTables();
    void updateState();
    void updateSelection();
    void editEntry(bool adding);
    void removeEntry();
    void save(bool reload);
    void reloadSaved();
    void revertDraft();
    void setError(const QString& message);
    void readLatest();
    void receive(const WorldsSnapshot& snapshot);
    void beginRequest();
    QTableWidget* currentTable() const;

    QTabWidget* m_tabs;
    QTableWidget* m_worlds;
    QTableWidget* m_groups;
    QLabel* m_error;
    QPushButton* m_add;
    QPushButton* m_edit;
    QPushButton* m_remove;
    QPushButton* m_save;
    QPushButton* m_apply;
    QPushButton* m_reload;
    QPushButton* m_revert;
    QPushButton* m_read;

    WorldsBackend* m_backend;
    enum class Action { Read, Save, Apply, Reload } m_action = Action::Read;
    bool m_ready = false;
    bool m_busy = false;
    bool m_readRequired = false;
    bool m_canReload = false;

    WorldsConfig m_draft;
    QString m_savedText;
    QString m_savedRevision;
    QString m_activeRevision;
};
