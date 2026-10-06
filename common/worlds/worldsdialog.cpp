// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "worldsdialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <limits>

namespace {

QTableWidget* configTable(const QStringList& headings, QWidget* parent) {
    auto* table = new QTableWidget(0, headings.size(), parent);
    table->setHorizontalHeaderLabels(headings);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(34);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    return table;
}

void addRow(QTableWidget* table, const QStringList& cells) {
    const int row = table->rowCount();
    table->insertRow(row);
    for (int column = 0; column < cells.size(); ++column)
        table->setItem(row, column, new QTableWidgetItem(cells[column]));
}

// QSpinBox stops at INT_MAX. Zero-decimal doubles represent all uint32 values exactly.
QDoubleSpinBox* numberField(const QString& name, quint32 minimum, quint32 maximum, quint32 value,
                            QWidget* parent) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setObjectName(name);
    spin->setDecimals(0);
    spin->setRange(minimum, maximum);
    spin->setValue(value);
    spin->setGroupSeparatorShown(false);
    return spin;
}

class EntryDialog : public QDialog {
public:
    EntryDialog(const WorldsConfig& draft, bool titleMapping, int row, QWidget* parent)
        : QDialog(parent), m_draft(draft), m_titleMapping(titleMapping), m_row(row) {
        setObjectName(QStringLiteral("worldsEntryDialog"));
        setWindowTitle(titleMapping ? (row < 0 ? tr("Add title group") : tr("Edit title group"))
                                    : (row < 0 ? tr("Add world") : tr("Edit world")));
        setMinimumWidth(440);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(22, 22, 22, 22);
        layout->setSpacing(14);
        auto* intro = new QLabel(titleMapping ? tr("Assign a title to a shared matchmaking pool.")
                                              : tr("Choose a pool and the world settings. The "
                                                   "configuration is filled in automatically."),
                                 this);
        intro->setWordWrap(true);
        layout->addWidget(intro);
        auto* form = new QFormLayout;
        m_group = new QComboBox(this);
        m_group->setObjectName(QStringLiteral("matchingGroup"));
        m_group->setEditable(true);
        m_group->setInsertPolicy(QComboBox::NoInsert);
        QSet<QString> knownGroups;
        for (const auto& world : draft.worlds)
            knownGroups.insert(world.group);
        for (const auto& mapping : draft.groups)
            knownGroups.insert(mapping.group);
        QStringList names = knownGroups.values();
        names.sort(Qt::CaseInsensitive);
        m_group->addItems(names);
        m_group->lineEdit()->setPlaceholderText(tr("Choose a pool or enter a new name"));
        if (titleMapping) {
            m_title = new QLineEdit(this);
            m_title->setObjectName(QStringLiteral("titleId"));
            m_title->setPlaceholderText(QStringLiteral("CUSA00207"));
            if (row >= 0) {
                m_title->setText(draft.groups[row].titleId);
                m_group->setCurrentText(draft.groups[row].group);
            }
            form->addRow(tr("Title ID"), m_title);
            form->addRow(tr("Matching group"), m_group);
            connect(m_title, &QLineEdit::textChanged, this, [this]() { validateFields(); });
        } else {
            const WorldDefinition world =
                row >= 0 ? draft.worlds[row] : WorldDefinition{m_group->currentText(), 1, 1, 0, 0};
            m_group->setCurrentText(world.group);
            const auto max = std::numeric_limits<quint32>::max();
            m_worldId = numberField(QStringLiteral("worldId"), 1, max, world.worldId, this);
            m_serverId = numberField(QStringLiteral("serverId"), 0, 65535, world.serverId, this);
            m_lobbies = numberField(QStringLiteral("lobbies"), 0, max, world.lobbies, this);
            m_members =
                numberField(QStringLiteral("maxMembers"), 0, max, world.maxLobbyMembers, this);
            form->addRow(tr("Matching group"), m_group);
            form->addRow(tr("World ID"), m_worldId);
            form->addRow(tr("Server ID"), m_serverId);
            form->addRow(tr("Lobbies"), m_lobbies);
            form->addRow(tr("Max lobby members"), m_members);
            if (row < 0)
                suggestWorldId();
            for (auto* spin : {m_worldId, m_serverId, m_lobbies, m_members}) {
                connect(spin, &QDoubleSpinBox::valueChanged, this, [this]() { validateFields(); });
                connect(spin, &QDoubleSpinBox::textChanged, this, [this]() { validateFields(); });
            }
        }
        layout->addLayout(form);
        m_error = new QLabel(this);
        m_error->setObjectName(QStringLiteral("entryError"));
        m_error->setTextFormat(Qt::PlainText);
        m_error->setStyleSheet(QStringLiteral("color: #a32626;"));
        m_error->setWordWrap(true);
        layout->addWidget(m_error);
        auto* buttons =
            new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        m_submit = buttons->button(QDialogButtonBox::Save);
        m_submit->setObjectName(QStringLiteral("saveEntry"));
        m_submit->setText(row < 0 ? tr("Add") : tr("Apply changes"));
        connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
            if (validateFields())
                accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
        connect(m_group, &QComboBox::currentTextChanged, this, [this]() {
            if (!m_titleMapping && m_row < 0)
                suggestWorldId();
            validateFields();
        });
        validateFields();
        if (m_title)
            m_title->setFocus();
    }

    WorldDefinition world() const {
        return {m_group->currentText().trimmed(), static_cast<quint32>(m_worldId->value()),
                static_cast<quint16>(m_serverId->value()), static_cast<quint32>(m_lobbies->value()),
                static_cast<quint32>(m_members->value())};
    }

    WorldGroupMapping mapping() const {
        return {m_title->text().trimmed().toUpper(), m_group->currentText().trimmed()};
    }

private:
    void suggestWorldId() {
        QSet<quint32> used;
        for (const auto& world : m_draft.worlds) {
            if (world.group == m_group->currentText().trimmed())
                used.insert(world.worldId);
        }
        quint32 next = 1;
        while (used.contains(next) && next < std::numeric_limits<quint32>::max())
            ++next;
        m_worldId->setValue(next);
    }

    bool validateFields() {
        if (!m_submit)
            return false;
        QString error;
        const QString group = m_group->currentText().trimmed();
        // Prevent config separators or line breaks from being emitted as names.
        static const QRegularExpression name(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*$"));
        static const QRegularExpression title(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_-]*$"));
        if (!name.match(group).hasMatch())
            error = tr("Enter a group name using letters, numbers, dots, underscores or hyphens.");
        else if (m_titleMapping) {
            const QString id = mapping().titleId;
            if (!title.match(id).hasMatch())
                error =
                    tr("Enter a title ID such as CUSA00207, without spaces or config separators.");
            else {
                for (int i = 0; i < m_draft.groups.size(); ++i) {
                    if (i != m_row &&
                        m_draft.groups[i].titleId.compare(id, Qt::CaseInsensitive) == 0) {
                        error =
                            tr("This title already has a matching group. Edit its existing row.");
                        break;
                    }
                }
            }
        } else {
            for (auto* spin : {m_worldId, m_serverId, m_lobbies, m_members}) {
                if (!spin->hasAcceptableInput())
                    error = tr("Enter a whole number within each field's allowed range.");
            }
            for (int i = 0; error.isEmpty() && i < m_draft.worlds.size(); ++i) {
                if (i != m_row && m_draft.worlds[i].group == group &&
                    m_draft.worlds[i].worldId == world().worldId)
                    error = tr("This pool already has that world ID. Choose another ID.");
            }
        }
        m_error->setText(error);
        m_error->setVisible(!error.isEmpty());
        m_submit->setEnabled(error.isEmpty());
        return error.isEmpty();
    }

    const WorldsConfig& m_draft;
    bool m_titleMapping;
    int m_row;
    QComboBox* m_group;
    QLineEdit* m_title = nullptr;
    QDoubleSpinBox* m_worldId = nullptr;
    QDoubleSpinBox* m_serverId = nullptr;
    QDoubleSpinBox* m_lobbies = nullptr;
    QDoubleSpinBox* m_members = nullptr;
    QLabel* m_error = nullptr;
    QPushButton* m_submit = nullptr;
};

} // namespace

WorldsDialog::WorldsDialog(WorldsBackend* backend, QWidget* parent)
    : QDialog(parent), m_backend(backend) {
    setObjectName(QStringLiteral("worldsDialog"));
    setWindowTitle(tr("Worlds configuration[*]"));
    resize(860, 600);
    setMinimumSize(760, 420);
    setStyleSheet(QStringLiteral(
        "QDialog { background: #f4f6fa; color: #172338; }"
        "QLabel { color: #172338; background: transparent; }"
        "QLabel#openFile { background: white; border: 1px solid #dde3eb; "
        "border-radius: 7px; padding: 12px; }"
        "QLabel#error { color: #a32626; background: #fff0f0; border-radius: 6px; padding: 10px; }"
        "QPushButton { color: #26354c; background: white; border: 1px solid #cdd6e2; "
        "border-radius: 6px; padding: 9px 14px; }"
        "QPushButton:hover { background: #edf2fa; border-color: #91a4bf; }"
        "QPushButton:disabled { background: #f0f2f5; color: #94a0af; border-color: #e0e5ed; }"
        "QPushButton#apply, QPushButton#add { background: #255ce4; color: white; border-color: "
        "#255ce4; font-weight: 600; }"
        "QPushButton#apply:hover, QPushButton#add:hover { background: #194bc4; }"
        "QPushButton#apply:disabled { background: #b7c8ed; border-color: #b7c8ed; color: #f5f7fc; }"
        "QTabWidget::pane { border: 1px solid #d5dde8; background: white; }"
        "QTabBar::tab { color: #54637a; background: #e8edf5; padding: 10px 18px; }"
        "QTabBar::tab:selected { color: #255ce4; background: white; }"
        "QTableWidget { color: #172338; background: white; alternate-background-color: #f8fafc; "
        "border: 0; selection-background-color: #dbe8ff; selection-color: #172338; }"
        "QHeaderView::section { background: #f0f4f9; color: #54637a; border: 0; padding: 10px 8px; "
        "font-weight: 500; }"
        "QLineEdit, QComboBox, QDoubleSpinBox { background: white; color: #172338; border: 1px "
        "solid #cdd6e2; border-radius: 4px; padding: 7px; }"
        "QComboBox QAbstractItemView { background: white; color: #172338; "
        "selection-background-color: #dbe8ff; }"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(12);
    auto* openFile = new QLabel(tr("Currently open file: %1").arg(backend->location()), this);
    openFile->setObjectName(QStringLiteral("openFile"));
    openFile->setWordWrap(true);
    openFile->setTextFormat(Qt::PlainText);
    openFile->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(openFile);
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("configTabs"));
    m_worlds = configTable(
        {tr("Matching group"), tr("World ID"), tr("Server ID"), tr("Lobbies"), tr("Max members")},
        this);
    m_worlds->setObjectName(QStringLiteral("worldsTable"));
    m_groups = configTable({tr("Title ID"), tr("Matching group")}, this);
    m_groups->setObjectName(QStringLiteral("groupsTable"));
    m_tabs->addTab(m_worlds, tr("Worlds"));
    m_tabs->addTab(m_groups, tr("Title groups"));
    layout->addWidget(m_tabs, 1);
    auto* tableActions = new QHBoxLayout;
    tableActions->addStretch();
    m_remove = new QPushButton(tr("Remove"), this);
    m_remove->setObjectName(QStringLiteral("remove"));
    m_edit = new QPushButton(tr("Edit..."), this);
    m_edit->setObjectName(QStringLiteral("edit"));
    m_add = new QPushButton(this);
    m_add->setObjectName(QStringLiteral("add"));
    tableActions->addWidget(m_remove);
    tableActions->addWidget(m_edit);
    tableActions->addWidget(m_add);
    layout->addLayout(tableActions);
    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("error"));
    m_error->setWordWrap(true);
    m_error->setTextFormat(Qt::PlainText);
    m_error->hide();
    layout->addWidget(m_error);
    auto* actions = new QHBoxLayout;
    m_revert = new QPushButton(tr("Discard edits"), this);
    m_revert->setObjectName(QStringLiteral("discard"));
    m_reload = new QPushButton(tr("Reload saved worlds"), this);
    m_reload->setObjectName(QStringLiteral("reload"));
    m_reload->setToolTip(
        tr("Apply the saved configuration. Unsaved table edits remain in your draft."));
    m_save = new QPushButton(tr("Save only"), this);
    m_save->setObjectName(QStringLiteral("save"));
    m_apply = new QPushButton(tr("Save && reload worlds"), this);
    m_apply->setObjectName(QStringLiteral("apply"));
    m_read = new QPushButton(tr("Refresh open file"), this);
    m_read->setObjectName(QStringLiteral("readLatest"));
    actions->addWidget(m_revert);
    actions->addWidget(m_read);
    actions->addStretch();
    actions->addWidget(m_reload);
    actions->addWidget(m_save);
    actions->addWidget(m_apply);
    layout->addLayout(actions);
    for (auto* button : {m_add, m_edit, m_remove, m_revert, m_reload, m_save, m_apply, m_read})
        button->setAutoDefault(false);
    connect(m_tabs, &QTabWidget::currentChanged, this, &WorldsDialog::updateSelection);
    for (auto* table : {m_worlds, m_groups}) {
        connect(table, &QTableWidget::itemSelectionChanged, this, &WorldsDialog::updateSelection);
        connect(table, &QTableWidget::cellDoubleClicked, this, [this]() { editEntry(false); });
    }
    connect(m_add, &QPushButton::clicked, this, [this]() { editEntry(true); });
    connect(m_edit, &QPushButton::clicked, this, [this]() { editEntry(false); });
    connect(m_remove, &QPushButton::clicked, this, &WorldsDialog::removeEntry);
    connect(m_revert, &QPushButton::clicked, this, &WorldsDialog::revertDraft);
    connect(m_reload, &QPushButton::clicked, this, &WorldsDialog::reloadSaved);
    connect(m_save, &QPushButton::clicked, this, [this]() { save(false); });
    connect(m_apply, &QPushButton::clicked, this, [this]() { save(true); });
    connect(m_read, &QPushButton::clicked, this, &WorldsDialog::readLatest);
    connect(backend, &WorldsBackend::received, this, &WorldsDialog::receive);
    connect(backend, &WorldsBackend::failed, this,
            [this](const QString& message, bool readRequired) {
                m_busy = false;
                m_readRequired |= readRequired;
                updateState();
                setError(message);
            });
    beginRequest();
    backend->load();
}

QString WorldsDialog::draftText() const {
    return serializeWorldsConfig(m_draft);
}

QTableWidget* WorldsDialog::currentTable() const {
    return m_tabs->currentIndex() == 0 ? m_worlds : m_groups;
}

void WorldsDialog::refreshTables() {
    const int worldRow = m_worlds->currentRow();
    const int groupRow = m_groups->currentRow();
    m_worlds->setRowCount(0);
    m_groups->setRowCount(0);
    for (const auto& world : m_draft.worlds) {
        addRow(m_worlds,
               {world.group, QString::number(world.worldId), QString::number(world.serverId),
                QString::number(world.lobbies), QString::number(world.maxLobbyMembers)});
    }
    for (const auto& mapping : m_draft.groups) {
        addRow(m_groups, {mapping.titleId, mapping.group});
    }
    m_worlds->selectRow(qMin(qMax(0, worldRow), m_worlds->rowCount() - 1));
    m_groups->selectRow(qMin(qMax(0, groupRow), m_groups->rowCount() - 1));
    if (!m_readRequired)
        setError({});
    updateSelection();
    updateState();
}

void WorldsDialog::updateSelection() {
    m_add->setText(m_tabs->currentIndex() == 0 ? tr("Add world...") : tr("Add title group..."));
    const bool editable = m_ready && !m_busy;
    m_tabs->setEnabled(editable);
    m_add->setEnabled(editable);
    const bool selected = editable && currentTable()->selectionModel()->hasSelection();
    m_edit->setEnabled(selected);
    m_remove->setEnabled(selected);
}

void WorldsDialog::editEntry(bool adding) {
    if (!m_ready || m_busy)
        return;
    const bool mapping = m_tabs->currentIndex() == 1;
    if (!adding && !currentTable()->selectionModel()->hasSelection())
        return;
    const int row = adding ? -1 : currentTable()->currentRow();
    EntryDialog entry(m_draft, mapping, row, this);
    if (entry.exec() != QDialog::Accepted)
        return;
    if (mapping) {
        if (adding)
            m_draft.groups.append(entry.mapping());
        else
            m_draft.groups[row] = entry.mapping();
    } else {
        if (adding)
            m_draft.worlds.append(entry.world());
        else
            m_draft.worlds[row] = entry.world();
    }
    refreshTables();
    currentTable()->selectRow(adding ? currentTable()->rowCount() - 1 : row);
}

void WorldsDialog::removeEntry() {
    if (!m_ready || m_busy)
        return;
    if (!currentTable()->selectionModel()->hasSelection())
        return;
    const int row = currentTable()->currentRow();
    if (m_tabs->currentIndex() == 0)
        m_draft.worlds.removeAt(row);
    else
        m_draft.groups.removeAt(row);
    refreshTables();
}

void WorldsDialog::updateState() {
    const bool dirty = m_ready && draftText() != m_savedText;
    const bool pending = m_canReload && m_savedRevision != m_activeRevision;
    const bool canWrite = m_ready && !m_busy && !m_readRequired;
    m_save->setEnabled(canWrite && dirty);
    m_apply->setEnabled(canWrite && m_canReload && (dirty || pending));
    m_reload->setEnabled(canWrite && m_canReload);
    m_apply->setVisible(m_backend->kind() != WorldsBackend::Kind::Local || m_canReload);
    m_reload->setVisible(m_backend->kind() != WorldsBackend::Kind::Local || m_canReload);
    m_revert->setEnabled(dirty && !m_busy);
    m_read->setEnabled(!m_busy);
    updateSelection();
    setWindowModified(dirty);
    setCursor(m_busy ? Qt::WaitCursor : Qt::ArrowCursor);
}

void WorldsDialog::setError(const QString& message) {
    m_error->setText(message);
    m_error->setVisible(!message.isEmpty());
}

void WorldsDialog::beginRequest() {
    m_busy = true;
    updateState();
    setError({});
}

void WorldsDialog::readLatest() {
    if (m_busy)
        return;
    if (m_ready && draftText() != m_savedText &&
        QMessageBox::question(this, tr("Refresh open file?"),
                              tr("Replace your unsaved draft with the latest saved configuration?"),
                              QMessageBox::Discard | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Discard)
        return;
    m_action = Action::Read;
    beginRequest();
    m_backend->load();
}

void WorldsDialog::receive(const WorldsSnapshot& snapshot) {
    m_busy = false;
    const auto parsed = parseWorldsConfig(snapshot.content);
    if (!parsed.isValid()) {
        m_ready = false;
        m_readRequired = true;
        updateState();
        setError(tr("Cannot edit this configuration. Line %1: %2. Correct the file in a text "
                    "editor and refresh it.")
                     .arg(parsed.errors.first().line)
                     .arg(parsed.errors.first().message));
        return;
    }
    if (m_action != Action::Reload || !m_ready)
        m_draft = parsed;
    m_savedText = serializeWorldsConfig(parsed);
    m_savedRevision = snapshot.revision;
    m_activeRevision = snapshot.activeRevision;
    m_canReload = snapshot.canReload;
    m_ready = true;
    m_readRequired = false;
    refreshTables();
}

void WorldsDialog::save(bool reload) {
    if (!m_ready || m_busy || m_readRequired || (reload && !m_canReload))
        return;
    const QString generated = draftText();
    const auto parsed = parseWorldsConfig(generated);
    if (!parsed.isValid()) {
        setError(parsed.errors.first().message);
        return;
    }
    m_action = reload ? Action::Apply : Action::Save;
    beginRequest();
    m_backend->save(generated, m_savedRevision, reload);
}

void WorldsDialog::reloadSaved() {
    if (!m_ready || m_busy || m_readRequired || !m_canReload)
        return;
    m_action = Action::Reload;
    beginRequest();
    m_backend->reload(m_savedRevision);
}

void WorldsDialog::revertDraft() {
    if (QMessageBox::question(this, tr("Discard edits?"),
                              tr("Restore the last saved configuration?"),
                              QMessageBox::Discard | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Discard)
        return;
    m_draft = parseWorldsConfig(m_savedText);
    refreshTables();
}

void WorldsDialog::reject() {
    if (m_busy) {
        setError(tr("Wait for the current request to finish before closing."));
        return;
    }
    if (m_ready && draftText() != m_savedText &&
        QMessageBox::question(this, tr("Unsaved edits"), tr("Discard your edits and close?"),
                              QMessageBox::Discard | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Discard)
        return;
    QDialog::reject();
}
