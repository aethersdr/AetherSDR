#include "NtpSourcesPage.h"

#include "CanonMessageBox.h"
#include "ControlAvailabilityRegistry.h"
#include "ScopedChildWidget.h"
#include "core/ThemeManager.h"
#include "models/RadioModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

enum Column { ColId, ColAddress, ColType, ColStatus, ColStratum, ColReach, ColEnabled, ColPrefer,
              ColDescription, ColumnCount };

bool confirmWithBox(QWidget* parent, const QString& title, const QString& text, const QString& accept)
{
    ScopedChildWidget<CanonMessageBox> box(title, text, parent);
    QPushButton* cancel = box.get()->addButton(QObject::tr("Cancel"), CanonMessageBox::Role::Reject);
    QPushButton* go = box.get()->addButton(accept, CanonMessageBox::Role::Destructive);
    box.get()->setDefaultButton(cancel);
    box.get()->exec();
    return box && box.get()->clickedButton() == go;
}

// The radio's reason, or why there is none: a local timeout is not a refusal.
QString replyReason(const RadioControlReply& reply)
{
    if (reply.kind == RadioControlReply::Kind::Failed && reply.code == -1) {
        return QObject::tr("the radio did not answer. The table shows what it reports");
    }
    if (reply.kind == RadioControlReply::Kind::Cancelled) {
        return QObject::tr("the radio session ended. The table shows what the radio reports");
    }
    const QString why = reply.message.trimmed();
    return why.isEmpty() ? QObject::tr("no reason given") : why;
}

} // namespace

QString NtpSourcesPage::unavailableReason(bool connected, const RadioCapabilities& caps)
{
    if (!connected) { return tr("Connect to a radio to see its NTP time sources."); }
    if (!caps.ntpSourceControl) {
        return tr("This radio does not keep its own NTP time sources. They need SmartSDR 4.3.16 "
                  "or later.");
    }
    return {};
}

QString NtpSourcesPage::definitionIssue(const NtpSourceDefinition& d, bool adding,
                                        const QList<NtpSourceState>& sources, int maxSources,
                                        const QStringList& pendingCreates)
{
    if (adding) {
        if (sources.size() + pendingCreates.size() >= maxSources) {
            return tr("The radio holds at most %1 NTP sources. Remove one first.").arg(maxSources);
        }
        if (d.id.isEmpty()) { return tr("Enter an ID for the source."); }
        if (!NtpSourceRules::idValid(d.id)) {
            return tr("The ID can use letters, digits and hyphens, up to %1 characters, and cannot "
                      "start or end with a hyphen.").arg(NtpSourceRules::kMaxIdChars);
        }
        for (const NtpSourceState& s : sources) {
            if (s.id == d.id) { return tr("A source with the ID %1 already exists.").arg(d.id); }
        }
        if (pendingCreates.contains(d.id)) {
            return tr("A source with the ID %1 is being added.").arg(d.id);
        }
    }
    if (d.address.isEmpty()) { return tr("Enter the server's host name or IP address."); }
    if (!NtpSourceRules::addressValid(d.address)) {
        return tr("The address can use letters, digits and . : - _ %, up to %1 characters.")
            .arg(NtpSourceRules::kMaxAddressChars);
    }
    if (!NtpSourceRules::directiveValid(d.directive)) {
        return tr("Choose server, pool or peer.");
    }
    if (!NtpSourceRules::descriptionValid(d.description)) {
        return tr("The description can use letters, digits, spaces and . _ -, on one line, up to "
                  "%1 characters.").arg(NtpSourceRules::kMaxDescriptionChars);
    }
    return {};
}

QString NtpSourcesPage::statusText(const NtpSourceState& s)
{
    if (!s.enabled) { return tr("Disabled"); }
    // Not color alone: the source the radio is synced to says so in words.
    if (s.inUse()) { return tr("★ In use"); }
    if (s.reach >= 0 && (s.reach & 0xFF) == 0) { return tr("Unreachable"); }
    if (s.state == QLatin1String("candidate")) { return tr("Candidate"); }
    if (s.state == QLatin1String("falseticker")) { return tr("Falseticker (rejected)"); }
    if (s.state == QLatin1String("jittery")) { return tr("Jittery"); }
    if (s.state == QLatin1String("outlier")) { return tr("Outlier"); }
    if (s.state == QLatin1String("non-selectable")) { return tr("Not usable"); }
    return QStringLiteral("—");
}

QString NtpSourcesPage::reachText(const NtpSourceState& s)
{
    if (!s.enabled || s.reach < 0) { return QStringLiteral("—"); }
    const int reg = s.reach & 0xFF;
    // The count of the last eight polls that got an answer.
    const int answered = qPopulationCount(static_cast<quint32>(reg));
    return reg == 0 ? tr("%1/8 (unreachable)").arg(answered) : tr("%1/8").arg(answered);
}

NtpSourcesPage::NtpSourcesPage(RadioModel& model, QWidget* parent)
    : QWidget(parent), m_model(model),
      m_availability(new ControlAvailabilityRegistry(model, this)),
      m_body(new QWidget(this)), m_table(new QTableWidget(0, ColumnCount, m_body)),
      m_count(new QLabel(m_body)), m_add(new QPushButton(tr("Add…"), m_body)),
      m_edit(new QPushButton(tr("Edit…"), m_body)), m_remove(new QPushButton(tr("Remove"), m_body)),
      m_editor(new QGroupBox(m_body)), m_id(new QLineEdit(m_editor)), m_address(new QLineEdit(m_editor)),
      m_directive(new QComboBox(m_editor)), m_description(new QLineEdit(m_editor)),
      m_prefer(new QCheckBox(tr("Prefer this source"), m_editor)), m_editorIssue(new QLabel(m_editor)),
      m_save(new QPushButton(m_editor)), m_cancel(new QPushButton(tr("Cancel"), m_editor)),
      m_message(new QLabel(this)), m_reason(new QLabel(this))
{
    setObjectName(QStringLiteral("ntpSourcesPage"));
    m_confirm = [this](const QString& title, const QString& text, const QString& accept) {
        return confirmWithBox(this, title, text, accept);
    };

    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr(
        "The radio sets its clock from these NTP servers. The radio keeps the list; "
        "changes here are sent to it and the table shows what it reports."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    m_reason->setObjectName(QStringLiteral("ntpUnavailableReason"));
    m_reason->setWordWrap(true);
    m_reason->setTextFormat(Qt::PlainText);
    m_reason->setAccessibleName(tr("Why NTP time sources are unavailable"));
    layout->addWidget(m_reason);

    auto* bodyLayout = new QVBoxLayout(m_body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    m_table->setObjectName(QStringLiteral("ntpSourcesTable"));
    m_table->setAccessibleName(tr("NTP time sources"));
    m_table->setHorizontalHeaderLabels({tr("ID"), tr("Address"), tr("Type"), tr("Status"), tr("Stratum"),
                                        tr("Reach"), tr("Enabled"), tr("Prefer"), tr("Description")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    // The short columns take what their text needs, so Enabled stays on screen
    // at the dialog's size; Description takes what is left.
    QHeaderView* header = m_table->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setSectionResizeMode(ColDescription, QHeaderView::Stretch);
    m_table->setTabKeyNavigation(false);
    bodyLayout->addWidget(m_table, 1);

    auto* buttons = new QHBoxLayout;
    m_count->setObjectName(QStringLiteral("ntpSourceCount"));
    m_add->setObjectName(QStringLiteral("ntpAddSource"));
    m_add->setAccessibleName(tr("Add an NTP source"));
    m_edit->setObjectName(QStringLiteral("ntpEditSource"));
    m_edit->setAccessibleName(tr("Edit the selected NTP source"));
    m_remove->setObjectName(QStringLiteral("ntpRemoveSource"));
    m_remove->setAccessibleName(tr("Remove the selected NTP source"));
    for (QPushButton* b : {m_add, m_edit, m_remove, m_save, m_cancel}) { b->setAutoDefault(false); }
    buttons->addWidget(m_count);
    buttons->addStretch();
    buttons->addWidget(m_add);
    buttons->addWidget(m_edit);
    buttons->addWidget(m_remove);
    bodyLayout->addLayout(buttons);

    m_editor->setObjectName(QStringLiteral("ntpSourceEditor"));
    auto* form = new QFormLayout(m_editor);
    m_id->setObjectName(QStringLiteral("ntpSourceId"));
    m_id->setAccessibleName(tr("Source ID"));
    m_id->setMaxLength(NtpSourceRules::kMaxIdChars);
    m_address->setObjectName(QStringLiteral("ntpSourceAddress"));
    m_address->setAccessibleName(tr("Server host name or IP address"));
    m_address->setMaxLength(NtpSourceRules::kMaxAddressChars);
    m_directive->setObjectName(QStringLiteral("ntpSourceType"));
    m_directive->setAccessibleName(tr("Source type"));
    m_directive->addItem(tr("Server"), QStringLiteral("server"));
    m_directive->addItem(tr("Pool"), QStringLiteral("pool"));
    m_directive->addItem(tr("Peer"), QStringLiteral("peer"));
    m_description->setObjectName(QStringLiteral("ntpSourceDescription"));
    m_description->setAccessibleName(tr("Source description"));
    m_description->setMaxLength(NtpSourceRules::kMaxDescriptionChars);
    m_prefer->setObjectName(QStringLiteral("ntpSourcePrefer"));
    m_editorIssue->setObjectName(QStringLiteral("ntpSourceIssue"));
    m_editorIssue->setWordWrap(true);
    m_editorIssue->setTextFormat(Qt::PlainText);
    m_editorIssue->setAccessibleName(tr("Why the source cannot be saved"));
    m_save->setObjectName(QStringLiteral("ntpSourceSave"));
    m_cancel->setObjectName(QStringLiteral("ntpSourceCancel"));
    form->addRow(tr("ID:"), m_id);
    form->addRow(tr("Address:"), m_address);
    form->addRow(tr("Type:"), m_directive);
    form->addRow(tr("Description:"), m_description);
    form->addRow(m_prefer);
    form->addRow(m_editorIssue);
    auto* editorButtons = new QHBoxLayout;
    editorButtons->addStretch();
    editorButtons->addWidget(m_cancel);
    editorButtons->addWidget(m_save);
    form->addRow(editorButtons);
    m_editor->setVisible(false);
    bodyLayout->addWidget(m_editor);
    layout->addWidget(m_body, 1);

    m_message->setObjectName(QStringLiteral("ntpSourcesMessage"));
    m_message->setWordWrap(true);
    m_message->setTextFormat(Qt::PlainText);
    m_message->setAccessibleName(tr("NTP source status"));
    m_message->setVisible(false);
    layout->addWidget(m_message);

    // True whether or not a radio is connected; m_reason names the specific one.
    m_availability->registerWidget(m_body,
        tr("NTP time sources need a connection to a radio running SmartSDR 4.3.16 or later."),
        [](bool connected, const RadioCapabilities& caps) {
            return unavailableReason(connected, caps).isEmpty();
        }, {}, false);

    connect(m_add, &QPushButton::clicked, this, [this] { openEditor(nullptr); });
    connect(m_edit, &QPushButton::clicked, this, [this] { openEditor(source(selectedId())); });
    connect(m_remove, &QPushButton::clicked, this, &NtpSourcesPage::removeSelected);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        if (!m_rebuilding) { m_pendingSelection = selectedId(); }
        updateButtons();
    });
    connect(m_table, &QTableWidget::itemDoubleClicked, this, [this] {
        if (m_edit->isEnabled()) { openEditor(source(selectedId())); }
    });
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (m_rebuilding || item->column() != ColEnabled) { return; }
        const QString id = item->data(Qt::UserRole).toString();
        const bool wanted = item->checkState() == Qt::Checked;
        // The box shows the radio's state until the radio reports the change.
        rebuild();
        requestEnabled(id, wanted);
    });
    for (QLineEdit* edit : {m_id, m_address, m_description}) {
        connect(edit, &QLineEdit::textChanged, this, &NtpSourcesPage::validateEditor);
    }
    connect(m_directive, &QComboBox::currentIndexChanged, this, &NtpSourcesPage::validateEditor);
    connect(m_prefer, &QCheckBox::toggled, this, &NtpSourcesPage::validateEditor);
    connect(m_save, &QPushButton::clicked, this, &NtpSourcesPage::saveEditor);
    connect(m_cancel, &QPushButton::clicked, this, &NtpSourcesPage::closeEditor);
    connect(&m_model, &RadioModel::ntpSourcesChanged, this, &NtpSourcesPage::rebuild);
    connect(&m_model, &RadioModel::capabilitiesChanged, this, [this] {
        // A different radio, or none: an open editor and an old message no longer apply.
        if (!unavailableReason(m_model.isConnected(), m_model.backendCapabilities()).isEmpty()) {
            closeEditor();
            setMessage({}, {});
        }
        rebuild();
    });
    rebuild();
}

int NtpSourcesPage::maxSources() const
{
    const std::optional<RadioCapabilities::NtpSourceControl> control =
        m_model.backendCapabilities().ntpSourceControl;
    return control ? control->maxSources : NtpSourceRules::kMaxSources;
}

const NtpSourceState* NtpSourcesPage::source(const QString& id) const
{
    for (const NtpSourceState& s : m_model.ntpSources()) {
        if (s.id == id) { return &s; }
    }
    return nullptr;
}

int NtpSourcesPage::enabledCount() const
{
    using Action = NtpSourceRequest::Action;
    int count = 0;
    for (const NtpSourceState& s : m_model.ntpSources()) {
        const auto request = m_pending.constFind(s.id);
        if (request != m_pending.cend()
            && (*request == Action::Disable || *request == Action::Remove)) {
            continue;
        }
        // A requested enable or create can still fail; count its status only.
        count += m_model.ntpSourceEnabledShown(s) ? 1 : 0;
    }
    return count;
}

void NtpSourcesPage::track(const QString& id, NtpSourceRequest::Action action)
{
    m_pending.insert(id, action);
    rebuild();
}

void NtpSourcesPage::untrack(const QString& id, NtpSourceRequest::Action action)
{
    const auto it = m_pending.find(id);
    if (it == m_pending.end() || *it != action) { return; }
    m_pending.erase(it);
    rebuild();
}

QString NtpSourcesPage::selectedId() const
{
    const QList<QTableWidgetItem*> items = m_table->selectedItems();
    return items.isEmpty() ? QString() : items.first()->data(Qt::UserRole).toString();
}

void NtpSourcesPage::rebuild()
{
    const QString reason = unavailableReason(m_model.isConnected(), m_model.backendCapabilities());
    m_reason->setText(reason);
    m_reason->setVisible(!reason.isEmpty());

    m_rebuilding = true;
    const QString keep = m_pendingSelection;
    const QList<NtpSourceState>& sources = m_model.ntpSources();
    m_table->setRowCount(int(sources.size()));
    int keepRow = -1;
    for (int row = 0; row < sources.size(); ++row) {
        const NtpSourceState& s = sources.at(row);
        const bool replacing = m_model.ntpSourceReplaceActive(s.id);
        const bool enabled = m_model.ntpSourceEnabledShown(s);
        const bool locked = replacing || pending(s.id);
        const QStringList texts{s.id, s.address, s.directive,
                                replacing ? tr("Updating…") : statusText(s),
                                s.enabled && s.stratum >= 0 ? QString::number(s.stratum) : QStringLiteral("—"),
                                reachText(s), QString(), s.prefer ? tr("Yes") : tr("No"), s.description};
        for (int column = 0; column < ColumnCount; ++column) {
            QTableWidgetItem* item = m_table->item(row, column);
            if (!item) { item = new QTableWidgetItem; m_table->setItem(row, column, item); }
            item->setData(Qt::UserRole, s.id);
            item->setText(texts.at(column));
            if (column == ColEnabled) {
                item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled
                               | (locked ? Qt::NoItemFlags : Qt::ItemIsUserCheckable));
                item->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
                item->setData(Qt::AccessibleTextRole, enabled ? tr("Enabled") : tr("Disabled"));
            } else {
                item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
            }
        }
        if (s.id == keep) { keepRow = row; }
    }
    if (keepRow >= 0) {
        if (selectedId() != keep) { m_table->selectRow(keepRow); }
    } else {
        m_table->clearSelection();
    }
    m_rebuilding = false;
    m_count->setText(tr("%1 of %2 sources").arg(sources.size()).arg(maxSources()));
    // Another client can change the list while the form is open.
    if (!m_editor->isHidden()) { validateEditor(); }
    updateButtons();
}

void NtpSourcesPage::updateButtons()
{
    const bool usable = unavailableReason(m_model.isConnected(), m_model.backendCapabilities()).isEmpty();
    const QString id = selectedId();
    const bool selected = usable && !id.isEmpty() && !m_model.ntpSourceReplaceActive(id) && !pending(id);
    QStringList creates;
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        if (it.value() == NtpSourceRequest::Action::Create) { creates.append(it.key()); }
    }
    const bool full = m_model.ntpSources().size() + creates.size() >= maxSources();
    m_add->setEnabled(usable && !full && m_editor->isHidden());
    const QString addWhy = full ? tr("The radio holds at most %1 NTP sources. Remove one first.").arg(maxSources())
                                : QString();
    m_add->setToolTip(addWhy);
    m_add->setAccessibleDescription(addWhy);
    m_edit->setEnabled(selected && m_editor->isHidden());
    m_remove->setEnabled(selected && m_editor->isHidden());
}

void NtpSourcesPage::setMessage(const QString& text, const QString& token)
{
    m_message->setText(text);
    m_message->setVisible(!text.isEmpty());
    m_message->setAccessibleDescription(text);
    ThemeManager::instance().setWidgetForegroundToken(m_message, token);
}

void NtpSourcesPage::openEditor(const NtpSourceState* existing)
{
    m_adding = existing == nullptr;
    const RadioControlSession session = m_model.controlSession();
    const QPointer<RadioModel> model(&m_model);
    m_editorSessionCurrent = [model, session] { return model && model->controlSessionCurrent(session); };
    const QSignalBlocker blockId(m_id), blockAddress(m_address), blockDescription(m_description),
        blockDirective(m_directive), blockPrefer(m_prefer);
    m_id->setText(existing ? existing->id : QString());
    m_id->setReadOnly(!m_adding);
    m_id->setAccessibleDescription(m_adding ? QString() : tr("The ID cannot be changed."));
    m_address->setText(existing ? existing->address : QString());
    const int type = m_directive->findData(existing ? existing->directive : QStringLiteral("server"));
    m_directive->setCurrentIndex(type >= 0 ? type : 0);
    m_description->setText(existing ? existing->description : QString());
    m_prefer->setChecked(existing && existing->prefer);
    m_editor->setTitle(m_adding ? tr("New NTP source") : tr("Edit %1").arg(existing->id));
    m_save->setText(m_adding ? tr("Add Source") : tr("Save Changes"));
    m_save->setAccessibleName(m_adding ? tr("Add the NTP source") : tr("Save the NTP source changes"));
    m_editor->setVisible(true);
    validateEditor();
    updateButtons();
    (m_adding ? m_id : m_address)->setFocus();
}

void NtpSourcesPage::closeEditor()
{
    m_editor->setVisible(false);
    m_editorSessionCurrent = {};
    updateButtons();
}

NtpSourceDefinition NtpSourcesPage::editorDefinition() const
{
    return {m_id->text().trimmed(), m_address->text().trimmed(), m_directive->currentData().toString(),
            m_description->text(), m_prefer->isChecked()};
}

void NtpSourcesPage::validateEditor()
{
    const NtpSourceDefinition d = editorDefinition();
    QStringList creates;
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        if (it.value() == NtpSourceRequest::Action::Create) { creates.append(it.key()); }
    }
    QString issue = definitionIssue(d, m_adding, m_model.ntpSources(), maxSources(), creates);
    if (issue.isEmpty() && !m_adding) {
        const NtpSourceState* current = source(d.id);
        if (!current) {
            issue = tr("The radio no longer lists %1.").arg(d.id);
        } else if (m_model.ntpSourceReplaceActive(d.id) || pending(d.id)) {
            issue = tr("%1 is being updated. Wait for the radio to answer.").arg(d.id);
        } else if (current->address == d.address && current->directive == d.directive
                   && current->description == d.description && current->prefer == d.prefer) {
            // Saving would remove and re-create the source for nothing.
            issue = tr("Nothing has changed.");
        }
    }
    m_editorIssue->setText(issue);
    m_editorIssue->setVisible(!issue.isEmpty());
    ThemeManager::instance().setWidgetForegroundToken(m_editorIssue, QStringLiteral("color.accent.warning"));
    m_save->setEnabled(issue.isEmpty());
    m_save->setToolTip(issue);
    m_save->setAccessibleDescription(issue);
}

void NtpSourcesPage::saveEditor()
{
    if (!m_editorSessionCurrent || !m_editorSessionCurrent()) {
        setMessage(tr("The radio session changed while the form was open. Nothing was sent."),
                   QStringLiteral("color.accent.warning"));
        closeEditor();
        return;
    }
    validateEditor();
    if (!m_save->isEnabled()) { return; }   // the reason is shown under the form
    const NtpSourceDefinition d = editorDefinition();
    const RadioControlSession session = m_model.controlSession();
    const QPointer<NtpSourcesPage> self(this);
    using Action = NtpSourceRequest::Action;
    if (m_adding) {
        m_pendingSelection = d.id;
        setMessage(tr("Adding %1…").arg(d.id), {});
        closeEditor();
        track(d.id, Action::Create);
        m_model.requestNtpSource({Action::Create, d}, this,
            [self, id = d.id](const RadioControlReply& reply) {
                if (!self) { return; }
                self->untrack(id, Action::Create);
                if (reply.kind == RadioControlReply::Kind::Accepted) {
                    self->setMessage(tr("The radio added %1.").arg(id), QStringLiteral("color.accent.success"));
                } else {
                    self->setMessage(tr("The radio did not add %1: %2").arg(id, replyReason(reply)),
                                     QStringLiteral("color.accent.danger"));
                }
            }, session);
        return;
    }
    setMessage(tr("Updating %1…").arg(d.id), {});
    closeEditor();
    const bool started = m_model.replaceNtpSource(d, this,
        [self, id = d.id](const RadioModel::NtpSourceReplaceResult& r) {
            if (!self) { return; }
            using Outcome = RadioModel::NtpSourceReplaceResult::Outcome;
            switch (r.outcome) {
            case Outcome::Replaced:
                self->setMessage(r.message.isEmpty() ? tr("The radio updated %1.").arg(id)
                                                     : tr("The radio updated %1, but %2").arg(id, r.message),
                                 r.message.isEmpty() ? QStringLiteral("color.accent.success")
                                                     : QStringLiteral("color.accent.warning"));
                break;
            case Outcome::Unchanged:
                self->setMessage(tr("%1 was not changed: %2").arg(id, r.message), QStringLiteral("color.accent.danger"));
                break;
            case Outcome::Restored:
                self->setMessage(tr("%1 kept its old settings: %2").arg(id, r.message),
                                 QStringLiteral("color.accent.danger"));
                break;
            case Outcome::Lost:
                self->setMessage(tr("%1 was removed but could not be re-created: %2").arg(id, r.message),
                                 QStringLiteral("color.accent.danger"));
                break;
            case Outcome::Cancelled:
                self->setMessage(tr("The radio session ended while %1 was being updated. "
                                    "The table shows what the radio reports.").arg(id),
                                 QStringLiteral("color.accent.warning"));
                break;
            case Outcome::NoAnswer:
                self->setMessage(tr("While %1 was being updated, %2. The table shows what the radio "
                                    "reports.").arg(id, r.message),
                                 QStringLiteral("color.accent.warning"));
                break;
            }
        }, session);
    if (!started) {
        setMessage(tr("%1 could not be updated now. Try again in a moment.").arg(d.id),
                   QStringLiteral("color.accent.danger"));
    }
}

void NtpSourcesPage::removeSelected()
{
    const QString id = selectedId();
    const NtpSourceState* s = source(id);
    if (!s || pending(id) || m_model.ntpSourceReplaceActive(id)) { return; }
    const bool lastEnabled = m_model.ntpSourceEnabledShown(*s) && enabledCount() == 1;
    const QString text = lastEnabled
        ? tr("%1 is the radio's only enabled NTP source. Without it the radio cannot set its clock "
             "from the network. Remove it?").arg(id)
        : tr("Remove the NTP source %1 from the radio?").arg(id);
    // The question is asked of this session, about this source.
    const RadioControlSession session = m_model.controlSession();
    const QPointer<NtpSourcesPage> self(this);
    if (!m_confirm(tr("Remove NTP Source"), text, tr("Remove"))) { return; }
    if (!self) { return; }
    if (!m_model.controlSessionCurrent(session) || !source(id) || pending(id)
        || m_model.ntpSourceReplaceActive(id)) {
        setMessage(tr("%1 changed while you were asked. Nothing was sent.").arg(id),
                   QStringLiteral("color.accent.warning"));
        return;
    }
    using Action = NtpSourceRequest::Action;
    setMessage(tr("Removing %1…").arg(id), {});
    track(id, Action::Remove);
    m_model.requestNtpSource({Action::Remove, {id, {}, {}, {}, false}}, this,
        [self, id](const RadioControlReply& reply) {
            if (!self) { return; }
            self->untrack(id, Action::Remove);
            if (reply.kind == RadioControlReply::Kind::Accepted) {
                self->setMessage(tr("The radio removed %1.").arg(id), QStringLiteral("color.accent.success"));
            } else {
                self->setMessage(tr("The radio did not remove %1: %2").arg(id, replyReason(reply)),
                                 QStringLiteral("color.accent.danger"));
            }
        }, session);
}

void NtpSourcesPage::requestEnabled(const QString& id, bool enabled)
{
    const NtpSourceState* s = source(id);
    if (!s || s->enabled == enabled || m_model.ntpSourceReplaceActive(id) || pending(id)) { return; }
    const RadioControlSession session = m_model.controlSession();
    const QPointer<NtpSourcesPage> self(this);
    if (!enabled && enabledCount() == 1) {
        if (!m_confirm(tr("Disable NTP Source"),
                       tr("%1 is the radio's only enabled NTP source. Without it the radio cannot set "
                          "its clock from the network. Disable it?").arg(id), tr("Disable"))) {
            return;
        }
        if (!self) { return; }
        const NtpSourceState* now = source(id);
        if (!m_model.controlSessionCurrent(session) || !now || now->enabled == enabled || pending(id)
            || m_model.ntpSourceReplaceActive(id)) {
            setMessage(tr("%1 changed while you were asked. Nothing was sent.").arg(id),
                       QStringLiteral("color.accent.warning"));
            return;
        }
    }
    using Action = NtpSourceRequest::Action;
    const Action action = enabled ? Action::Enable : Action::Disable;
    track(id, action);
    m_model.requestNtpSource({action, {id, {}, {}, {}, false}}, this,
        [self, id, action](const RadioControlReply& reply) {
            if (!self) { return; }
            self->untrack(id, action);
            if (reply.kind == RadioControlReply::Kind::Accepted) {
                self->setMessage({}, {});
                return;
            }
            self->setMessage((action == Action::Enable ? tr("The radio did not enable %1: %2")
                                                       : tr("The radio did not disable %1: %2"))
                                 .arg(id, replyReason(reply)),
                             QStringLiteral("color.accent.danger"));
        }, session);
}

} // namespace AetherSDR
