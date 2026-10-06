// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QFileInfo>
#include "localworldsbackend.h"
#include "member_window.h"
#include "worldsdialog.h"

#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

MemberWindow::MemberWindow(const QString& databasePath, QWidget* parent) : QWidget(parent) {
    setWindowTitle("shadNet Toolbox");
    resize(760, 660);
    auto* layout = new QVBoxLayout(this);
    auto* note = new QLabel("Stop shadnet before changing accounts. Restart it when you're done.");
    note->setWordWrap(true);
    layout->addWidget(note);

    auto* worldsButton = new QPushButton("Worlds configuration...");
    worldsButton->setObjectName("openWorlds");
    layout->addWidget(worldsButton);
    connect(worldsButton, &QPushButton::clicked, this, [this] {
        QString suggested = QSettings().value("worldsPath").toString();
        if (suggested.isEmpty() && m_store.IsOpen())
            suggested = QFileInfo(m_store.Path()).dir().absoluteFilePath("../worlds.cfg");
        if (suggested.isEmpty())
            suggested = QDir(QCoreApplication::applicationDirPath()).filePath("worlds.cfg");
        QFileDialog picker(this, "Open local worlds.cfg", suggested);
        picker.setFileMode(QFileDialog::AnyFile);
        picker.setAcceptMode(QFileDialog::AcceptOpen);
        picker.setNameFilter("Worlds configuration (worlds.cfg);;Configuration files (*.cfg)");
        if (picker.exec() == QDialog::Accepted && !picker.selectedFiles().isEmpty())
            OpenWorlds(picker.selectedFiles().first());
    });

    auto* databaseRow = new QHBoxLayout;
    databaseRow->addWidget(new QLabel("Database:"));
    m_path = new QLineEdit;
    m_path->setReadOnly(true);
    m_path->setObjectName("databasePath");
    m_path->setPlaceholderText("Choose the server's db/shadnet.db file");
    databaseRow->addWidget(m_path, 1);
    auto* browse = new QPushButton("Browse...");
    databaseRow->addWidget(browse);
    layout->addLayout(databaseRow);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, "Open shadnet database", m_path->text(),
            "SQLite databases (*.db *.sqlite *.sqlite3);;All files (*)");
        if (!path.isEmpty())
            OpenDatabase(path);
    });

    auto* createGroup = new QGroupBox("Add member");
    auto* form = new QFormLayout(createGroup);
    m_username = new QLineEdit;
    m_username->setObjectName("username");
    m_username->setPlaceholderText("Username (3–16 characters)");
    form->addRow("Username:", m_username);
    m_email = new QLineEdit;
    m_email->setReadOnly(true);
    m_email->setObjectName("email");
    form->addRow("Email:", m_email);
    m_add = new QPushButton("Create member");
    m_add->setObjectName("createMember");
    m_add->setEnabled(false);
    form->addRow(m_add);
    layout->addWidget(createGroup);
    connect(m_username, &QLineEdit::textChanged, this, [this](const QString& text) {
        const QString name = text.trimmed();
        m_email->setText(name.isEmpty() ? QString() : name + "@shadps4.local");
        m_add->setEnabled(m_store.IsOpen() && !name.isEmpty());
    });
    connect(m_username, &QLineEdit::returnPressed, m_add, &QPushButton::click);
    connect(m_add, &QPushButton::clicked, this, &MemberWindow::AddMember);

    auto* credentialsGroup = new QGroupBox("Last created member");
    auto* credentialsLayout = new QVBoxLayout(credentialsGroup);
    m_credentials = new QPlainTextEdit;
    m_credentials->setReadOnly(true);
    m_credentials->setObjectName("credentials");
    m_credentials->setPlaceholderText("The generated password will appear here after creation.");
    m_credentials->setMaximumHeight(105);
    credentialsLayout->addWidget(m_credentials);
    m_copy = new QPushButton("Copy login details");
    m_copy->setObjectName("copyCredentials");
    m_copy->setEnabled(false);
    credentialsLayout->addWidget(m_copy);
    connect(m_copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_credentials->toPlainText());
        m_status->setText("Login details copied.");
    });
    layout->addWidget(credentialsGroup);

    auto* listRow = new QHBoxLayout;
    m_search = new QLineEdit;
    m_search->setObjectName("memberSearch");
    m_search->setPlaceholderText("Find a member");
    m_search->setClearButtonEnabled(true);
    listRow->addWidget(m_search, 1);
    m_refresh = new QPushButton("Refresh");
    m_refresh->setEnabled(false);
    listRow->addWidget(m_refresh);
    layout->addLayout(listRow);
    m_members = new QTableWidget(0, 2);
    m_members->setObjectName("members");
    m_members->setHorizontalHeaderLabels({"Username", "Email"});
    m_members->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_members->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_members->verticalHeader()->hide();
    m_members->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_members->setSelectionMode(QAbstractItemView::SingleSelection);
    m_members->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_members, 1);
    connect(m_search, &QLineEdit::textChanged, this, &MemberWindow::Filter);
    connect(m_refresh, &QPushButton::clicked, this, &MemberWindow::Refresh);

    auto* footer = new QHBoxLayout;
    m_status = new QLabel("Choose a database to begin.");
    m_status->setWordWrap(true);
    footer->addWidget(m_status, 1);
    m_remove = new QPushButton("Remove selected member");
    m_remove->setObjectName("removeMember");
    m_remove->setEnabled(false);
    footer->addWidget(m_remove);
    layout->addLayout(footer);
    connect(m_members, &QTableWidget::itemSelectionChanged, this,
            [this] { m_remove->setEnabled(!m_members->selectedItems().isEmpty()); });
    connect(m_remove, &QPushButton::clicked, this, &MemberWindow::RemoveMember);

    if (!databasePath.isEmpty())
        OpenDatabase(databasePath);
}

bool MemberWindow::OpenDatabase(const QString& path) {
    QString error;
    if (!m_store.Open(path, error)) {
        QMessageBox::warning(this, "Cannot open database", error);
        return false;
    }
    m_path->setText(m_store.Path());
    m_credentials->clear();
    m_copy->setEnabled(false);
    m_add->setEnabled(!m_username->text().trimmed().isEmpty());
    m_refresh->setEnabled(true);
    QSettings().setValue("databasePath", m_store.Path());
    Refresh();
    m_username->setFocus();
    return true;
}

void MemberWindow::Refresh() {
    QString error;
    const auto members = m_store.List(error);
    m_members->setRowCount(0);
    m_remove->setEnabled(false);
    if (!error.isEmpty()) {
        m_status->setText("Could not read members: " + error);
        return;
    }
    for (const auto& member : members) {
        const int row = m_members->rowCount();
        m_members->insertRow(row);
        auto* name = new QTableWidgetItem(member.username);
        name->setData(Qt::UserRole, static_cast<qlonglong>(member.id));
        m_members->setItem(row, 0, name);
        m_members->setItem(row, 1, new QTableWidgetItem(member.email));
    }
    Filter();
    m_status->setText(QString("%1 member(s).").arg(members.size()));
}

void MemberWindow::Filter() {
    m_members->clearSelection();
    const QString search = m_search->text().trimmed();
    for (int row = 0; row < m_members->rowCount(); ++row)
        m_members->setRowHidden(
            row, !m_members->item(row, 0)->text().contains(search, Qt::CaseInsensitive));
}

void MemberWindow::AddMember() {
    QString error;
    const auto credentials = m_store.Add(m_username->text(), error);
    if (!credentials) {
        QMessageBox::warning(this, "Cannot create member", error);
        return;
    }
    QString details = QString("Username: %1\nPassword: %2\nEmail: %3")
                          .arg(credentials->username, credentials->password, credentials->email);
    if (m_store.NeedsToken())
        details += "\nToken: " + credentials->token;
    m_credentials->setPlainText(details);
    m_copy->setEnabled(true);
    m_search->clear();
    Refresh();
    m_status->setText(credentials->username + " created. Copy the login details before closing.");
    m_username->selectAll();
    m_username->setFocus();
}

void MemberWindow::RemoveMember() {
    const auto selected = m_members->selectedItems();
    if (selected.isEmpty())
        return;
    const auto* name = m_members->item(selected.first()->row(), 0);
    const QString username = name->text();
    const int64_t id = name->data(Qt::UserRole).toLongLong();
    if (QMessageBox::question(
            this, "Remove member",
            QString("Remove %1 and their scores, trophies, friendships, and title storage?")
                .arg(username),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    QString error, warning;
    if (!m_store.Remove(id, error, warning)) {
        QMessageBox::warning(this, "Cannot remove member", error);
        return;
    }
    m_credentials->clear();
    m_copy->setEnabled(false);
    Refresh();
    m_status->setText(warning.isEmpty() ? username + " removed." : warning);
}

void MemberWindow::OpenWorlds(const QString& path) {
    QSettings().setValue("worldsPath", QFileInfo(path).absoluteFilePath());
    LocalWorldsBackend backend(path);
    WorldsDialog(&backend, this).exec();
}
