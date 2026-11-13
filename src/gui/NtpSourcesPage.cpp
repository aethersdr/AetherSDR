#include "NtpSourcesPage.h"

#include "CanonMessageBox.h"
#include "ControlAvailabilityRegistry.h"
#include "ScopedChildWidget.h"
#include "RadioSetupDialogCommon.h"
#include "GuardedSlider.h"
#include "core/ThemeManager.h"
#include "models/RadioModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QFrame>
#include <QScrollArea>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTabBar>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {


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
        return QObject::tr("the radio did not answer. The dashboard shows what it reports");
    }
    if (reply.kind == RadioControlReply::Kind::Cancelled) {
        return QObject::tr("the radio session ended. The dashboard shows what the radio reports");
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
      m_body(new QWidget(this)), m_tabs(new QTabBar(m_body)),
      m_slots(NtpSourceRules::kMaxSources),
      m_serviceState(new QLabel(m_body)), m_activeSource(new QLabel(m_body)),
      m_health(new QLabel(m_body)), m_gps(new QLabel(m_body)), m_gpsDetails(new QLabel(m_body)),
      m_sourceState(new QLabel(m_body)), m_stratum(new QLabel(m_body)),
      m_reach(new QLabel(m_body)), m_enabled(new QCheckBox(tr("Enabled on radio"), m_body)),
      m_count(new QLabel(m_body)), m_remove(new QPushButton(tr("Remove"), m_body)),
      m_editor(new QGroupBox(m_body)), m_id(new QLineEdit(m_editor)), m_address(new QLineEdit(m_editor)),
      m_directive(new GuardedComboBox(m_editor)), m_description(new QLineEdit(m_editor)),
      m_prefer(new QCheckBox(tr("Prefer this source"), m_editor)), m_editorIssue(new QLabel(m_editor)),
      m_save(new QPushButton(m_editor)), m_cancel(new QPushButton(tr("Cancel"), m_editor)),
      m_message(new QLabel(this)), m_reason(new QLabel(this))
{
    setObjectName(QStringLiteral("ntpSourcesPage"));
    m_confirm = [this](const QString& title, const QString& text, const QString& accept) {
        return confirmWithBox(this, title, text, accept);
    };

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    auto* intro = new QLabel(tr(
        "The radio sets its clock from these NTP servers. The radio keeps the list; "
        "changes here are sent to it. Status follows the radio live."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    m_reason->setObjectName(QStringLiteral("ntpUnavailableReason"));
    m_reason->setWordWrap(true);
    m_reason->setTextFormat(Qt::PlainText);
    m_reason->setAccessibleName(tr("Why NTP time sources are unavailable"));
    layout->addWidget(m_reason);

    auto* bodyLayout = new QVBoxLayout(m_body);
    bodyLayout->setSpacing(8);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(m_body);
    layout->addWidget(scroll, 1);

    auto card = [this](const QString& title, QLabel* value, const char* name) {
        auto* frame = new QFrame(m_body);
        frame->setProperty("ntpCard", true);
        auto* content = new QVBoxLayout(frame);
        content->setContentsMargins(16, 9, 16, 9);
        content->setSpacing(4);
        auto* caption = new QLabel(title, frame);
        setTone(caption, Tone::Hint);
        value->setObjectName(QString::fromLatin1(name));
        value->setAccessibleName(title);
        value->setTextFormat(Qt::PlainText);
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        setTone(value, Tone::Title);
        content->addWidget(caption);
        content->addWidget(value);
        return frame;
    };
    auto* hero = card(tr("NETWORK TIME · LIVE RADIO STATUS"), m_serviceState, "ntpServiceState");
    m_activeSource->setObjectName(QStringLiteral("ntpActiveSource"));
    m_activeSource->setTextFormat(Qt::PlainText);
    m_activeSource->setWordWrap(true);
    m_activeSource->setAccessibleName(tr("Selected NTP reference"));
    setTone(m_activeSource, Tone::Value);
    hero->setObjectName(QStringLiteral("ntpReferenceHero"));
    hero->layout()->addWidget(m_activeSource);
    bodyLayout->addWidget(hero);
    auto* overview = new QHBoxLayout;
    overview->addWidget(card(tr("SOURCE HEALTH"), m_health, "ntpHealth"), 1);
    QFrame* gpsCard = card(tr("ONBOARD REFERENCE"), m_gps, "ntpGpsReference");
    gpsCard->setObjectName(QStringLiteral("ntpGpsCard"));
    m_gpsDetails->setObjectName(QStringLiteral("ntpGpsDetails"));
    m_gpsDetails->setTextFormat(Qt::PlainText);
    m_gpsDetails->setWordWrap(true);
    setTone(m_gpsDetails, Tone::Hint);
    gpsCard->layout()->addWidget(m_gpsDetails);
    overview->addWidget(gpsCard, 1);
    bodyLayout->addLayout(overview);

    auto* heading = new QHBoxLayout;
    auto* servers = new QLabel(tr("Time servers"), m_body);
    setTone(servers, Tone::Heading);
    m_count->setObjectName(QStringLiteral("ntpSourceCount"));
    setTone(m_count, Tone::Hint);
    heading->addWidget(servers);
    heading->addStretch();
    heading->addWidget(m_count);
    bodyLayout->addLayout(heading);
    m_tabs->setObjectName(QStringLiteral("ntpServerTabs"));
    m_tabs->setAccessibleName(tr("NTP server configuration slots 1 through 10"));
    m_tabs->setExpanding(true);
    m_tabs->setUsesScrollButtons(true);
    for (int i = 0; i < NtpSourceRules::kMaxSources; ++i) {
        m_tabs->addTab(QString::number(i + 1));
    }
    bodyLayout->addWidget(m_tabs);

    auto* sourceStats = new QHBoxLayout;
    sourceStats->addWidget(card(tr("SELECTION STATE"), m_sourceState, "ntpSelectionState"), 2);
    sourceStats->addWidget(card(tr("SOURCE STRATUM"), m_stratum, "ntpSourceStratum"), 1);
    sourceStats->addWidget(card(tr("REACH · OCTAL"), m_reach, "ntpReachRegister"), 1);
    bodyLayout->addLayout(sourceStats);
    auto* polls = new QFrame(m_body);
    polls->setProperty("ntpCard", true);
    auto* pollLayout = new QVBoxLayout(polls);
    pollLayout->setContentsMargins(10, 8, 10, 8);
    pollLayout->setSpacing(4);
    auto* pollCaption = new QLabel(tr("LAST EIGHT POLLS · OLDEST → NEWEST"), polls);
    setTone(pollCaption, Tone::Hint);
    pollLayout->addWidget(pollCaption);
    auto* pollStrip = new QHBoxLayout;
    for (int i = 0; i < 8; ++i) {
        m_polls[i] = new QLabel(polls);
        m_polls[i]->setObjectName(QStringLiteral("ntpPoll%1").arg(i + 1));
        m_polls[i]->setAlignment(Qt::AlignCenter);
        m_polls[i]->setProperty("ntpPoll", true);
        m_polls[i]->setMinimumHeight(32);
        pollStrip->addWidget(m_polls[i], 1);
    }
    pollLayout->addLayout(pollStrip);
    bodyLayout->addWidget(polls);
    m_enabled->setObjectName(QStringLiteral("ntpSourceEnabled"));
    m_enabled->setAccessibleName(tr("Enable the selected NTP source on the radio"));
    m_remove->setObjectName(QStringLiteral("ntpRemoveSource"));
    m_remove->setAccessibleName(tr("Remove the selected NTP source"));
    setButtonRole(m_remove, ButtonRole::Danger);
    auto* sourceActions = new QHBoxLayout;
    sourceActions->addWidget(m_enabled);
    sourceActions->addStretch();
    sourceActions->addWidget(m_remove);
    bodyLayout->addLayout(sourceActions);
    for (QPushButton* button : {m_remove, m_save, m_cancel}) { button->setAutoDefault(false); }

    m_editor->setObjectName(QStringLiteral("ntpSourceEditor"));
    auto* form = new QFormLayout(m_editor);
    form->setVerticalSpacing(4);
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
    bodyLayout->addStretch();
    ThemeManager::instance().applyStyleSheet(this, QStringLiteral(R"(
        QFrame[ntpCard="true"] {
            background: {{color.canon.nested}};
            border: 1px solid {{color.canon.line}}; border-radius: 12px;
        }
        QFrame#ntpReferenceHero {
            background: qlineargradient(x1:0,y1:0,x2:1,y2:1,
                stop:0 {{color.canon.control}}, stop:1 {{color.canon.raised}});
            border: 1px solid {{color.canon.lineHi}};
        }
        QLabel#ntpServiceState { font-size: 28px; font-weight: 600; }
        QLabel#ntpActiveSource { font-size: 15px; }
        QLabel#ntpHealth, QLabel#ntpGpsReference,
        QLabel#ntpSelectionState, QLabel#ntpSourceStratum,
        QLabel#ntpReachRegister { font-size: 18px; font-weight: 600; }
        QLabel[ntpPoll="true"] {
            background: {{color.canon.control}};
            border: 1px solid {{color.canon.lineHi}}; border-radius: 6px;
        }
        QFrame#ntpGpsCard[ntpGold="true"] {
            background: qlineargradient(x1:0,y1:0,x2:1,y2:1,
                stop:0 {{color.canon.control}}, stop:1 {{color.canon.raised}});
            border: 2px solid {{color.canon.sparkGold}};
        }
    )"));

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

    connect(m_tabs, &QTabBar::currentChanged, this, &NtpSourcesPage::selectTab);
    connect(m_remove, &QPushButton::clicked, this, &NtpSourcesPage::removeSelected);
    connect(m_enabled, &QCheckBox::clicked, this, [this](bool enabled) {
        const QString id = selectedId();
        rebuild();
        requestEnabled(id, enabled);
    });
    for (QLineEdit* edit : {m_id, m_address, m_description}) {
        connect(edit, &QLineEdit::textChanged, this, &NtpSourcesPage::validateEditor);
    }
    connect(m_directive, &QComboBox::currentIndexChanged, this, &NtpSourcesPage::validateEditor);
    connect(m_prefer, &QCheckBox::toggled, this, &NtpSourcesPage::validateEditor);
    connect(m_save, &QPushButton::clicked, this, &NtpSourcesPage::saveEditor);
    m_cancel->setText(tr("Reset"));
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        m_drafts.remove(m_tabs->currentIndex());
        openEditor(source(selectedId()));
        updateDashboard();
    });
    connect(&m_model, &RadioModel::ntpSourcesChanged, this, &NtpSourcesPage::rebuild);
    connect(&m_model, &RadioModel::capabilitiesChanged, this, [this] {
        // A different radio, or none: an open editor and an old message no longer apply.
        if (!unavailableReason(m_model.isConnected(), m_model.backendCapabilities()).isEmpty()) {
            closeEditor();
            m_drafts.clear();
            setMessage({}, {});
        }
        rebuild();
    });
    connect(&m_model, &RadioModel::gpsStatusChanged, this, &NtpSourcesPage::updateDashboard);
    connect(&m_model, &RadioModel::oscillatorChanged, this, &NtpSourcesPage::updateDashboard);
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
    return m_slots.value(m_tabs->currentIndex());
}

void NtpSourcesPage::selectTab()
{
    if (m_rebuilding) { return; }
    if (m_editorSessionCurrent && m_editorSessionCurrent()) {
        if (!m_editor->isHidden()) { m_drafts.insert(m_editorTab, editorDefinition()); }
    } else {
        m_drafts.clear();
    }
    openEditor(source(selectedId()));
    const auto draft = m_drafts.constFind(m_tabs->currentIndex());
    if (draft != m_drafts.cend() && (m_adding || draft->id == selectedId())) {
        const QSignalBlocker id(m_id), address(m_address), description(m_description),
            directive(m_directive), prefer(m_prefer);
        m_id->setText(draft->id);
        m_address->setText(draft->address);
        m_directive->setCurrentIndex(m_directive->findData(draft->directive));
        m_description->setText(draft->description);
        m_prefer->setChecked(draft->prefer);
    }
    validateEditor();
    updateDashboard();
}

void NtpSourcesPage::rebuild()
{
    const QString reason = unavailableReason(m_model.isConnected(), m_model.backendCapabilities());
    m_reason->setText(reason);
    m_reason->setVisible(!reason.isEmpty());
    m_rebuilding = true;
    // Preserve a slot's identity while live sources arrive, disappear or reorder.
    for (QString& id : m_slots) {
        if (!id.isEmpty() && !source(id) && !pending(id)) { id.clear(); }
    }
    for (const NtpSourceState& state : m_model.ntpSources()) {
        if (m_slots.contains(state.id)) { continue; }
        const int empty = m_slots.indexOf(QString());
        if (empty < 0) { break; }
        m_slots[empty] = state.id;
    }
    for (int i = 0; i < m_slots.size(); ++i) {
        const NtpSourceState* state = source(m_slots.at(i));
        m_tabs->setTabText(i, state && state->enabled && state->inUse()
            ? tr("%1 ★").arg(i + 1) : QString::number(i + 1));
        m_tabs->setTabToolTip(i, state ? tr("Server %1: %2 — %3").arg(i + 1).arg(state->address, statusText(*state))
                                              : tr("Server %1: empty configuration slot").arg(i + 1));
    }
    m_rebuilding = false;
    m_count->setText(tr("%1 of %2 sources").arg(m_model.ntpSources().size()).arg(maxSources()));
    const QString id = selectedId();
    if (m_editor->isHidden() || (!m_adding && m_editorId != id)
        || (m_adding && m_id->text().isEmpty() && !id.isEmpty())) {
        openEditor(source(id));
    }
    validateEditor();
    updateDashboard();
    updateButtons();
}

void NtpSourcesPage::updateDashboard()
{
    const bool usable = unavailableReason(m_model.isConnected(), m_model.backendCapabilities()).isEmpty();
    int enabled = 0;
    int reachable = 0;
    const NtpSourceState* active = nullptr;
    for (const NtpSourceState& state : m_model.ntpSources()) {
        if (!state.enabled) { continue; }
        ++enabled;
        if (state.reach > 0) { ++reachable; }
        if (state.inUse()) { active = &state; }
    }
    m_serviceState->setText(!usable ? tr("Status unavailable") : active ? tr("Reference selected")
        : enabled ? tr("Awaiting a reference") : tr("No enabled sources"));
    setTone(m_serviceState, active && usable ? Tone::Ok : Tone::Warn);
    m_activeSource->setText(active && usable ? tr("%1 · %2").arg(active->address, active->id)
        : tr("No network reference reported by the radio"));
    m_health->setText(tr("%1 enabled · %2 reachable").arg(enabled).arg(reachable));
    m_health->setAccessibleDescription(tr("Reachable means at least one reply in the last eight polls; "
        "these counts describe configured sources, not individual servers resolved from a pool."));
    const bool gpsdo = m_model.isConnected() && m_model.gpsdoPresent();
    const bool gpsLocked = gpsdo && m_model.gpsStatus() == QLatin1String("Locked");
    m_gps->setText(gpsLocked ? tr("GPSDO locked") : gpsdo ? tr("GPSDO · %1").arg(m_model.gpsStatus())
                                                           : tr("No GPSDO reported"));
    m_gpsDetails->setText(gpsdo
        ? tr("Service stratum not reported · %1 satellites tracked").arg(m_model.gpsTracked())
        : tr("Service stratum not reported"));
    // GPS lock is an oscillator status, not the local chrony tracking stratum.
    m_gps->setToolTip(tr("Local NTP service stratum is not reported by this radio."));
    m_gps->setAccessibleDescription(tr("The radio does not report its local NTP service stratum. "
        "A GPSDO lock alone does not confirm stratum 1."));
    QWidget* gpsCard = findChild<QWidget*>(QStringLiteral("ntpGpsCard"));
    if (gpsCard->property("ntpGold").toBool() != gpsLocked) {
        gpsCard->setProperty("ntpGold", gpsLocked);
        gpsCard->style()->unpolish(gpsCard);
        gpsCard->style()->polish(gpsCard);
    }
    ThemeManager::instance().setWidgetForegroundToken(m_gps, gpsLocked
        ? QStringLiteral("color.canon.sparkGold") : QStringLiteral("color.canon.inkSoft"));
    const NtpSourceState* state = source(selectedId());
    const bool measured = usable && state && state->enabled;
    m_sourceState->setText(!state ? tr("Empty slot") : m_model.ntpSourceReplaceActive(state->id)
        ? tr("Updating…") : statusText(*state));
    setTone(m_sourceState, measured && state->inUse() ? Tone::Ok : Tone::Value);
    m_stratum->setText(measured && state->stratum >= 0 ? QString::number(state->stratum) : tr("Not reported"));
    m_stratum->setAccessibleDescription(tr("Upstream source stratum; this is not the radio's local service stratum."));
    const bool reachKnown = measured && state->reach >= 0;
    m_reach->setText(reachKnown ? QStringLiteral("%1 · %2").arg(state->reach & 255, 3, 8, QLatin1Char('0'))
        .arg(reachText(*state)) : tr("Not reported"));
    for (int i = 0; i < 8; ++i) {
        const bool answered = reachKnown && (state->reach & (1 << (7 - i)));
        m_polls[i]->setText(!reachKnown ? tr("—") : answered ? tr("✓") : tr("×"));
        m_polls[i]->setAccessibleName(tr("Poll %1, %2").arg(i + 1).arg(!reachKnown ? tr("not reported")
            : answered ? tr("reply received") : tr("no reply")));
        ThemeManager::instance().setWidgetForegroundToken(m_polls[i], !reachKnown
            ? QStringLiteral("color.canon.muted") : answered ? QStringLiteral("color.accent.success")
                                                           : QStringLiteral("color.accent.warning"));
    }
    const QSignalBlocker blocker(m_enabled);
    m_enabled->setChecked(state && m_model.ntpSourceEnabledShown(*state));
}

void NtpSourcesPage::updateButtons()
{
    const bool usable = unavailableReason(m_model.isConnected(), m_model.backendCapabilities()).isEmpty();
    const QString id = selectedId();
    const bool selected = usable && source(id) && !m_model.ntpSourceReplaceActive(id) && !pending(id);
    m_enabled->setEnabled(selected);
    m_remove->setEnabled(selected);
    const QString reason = !usable ? unavailableReason(m_model.isConnected(), m_model.backendCapabilities())
        : id.isEmpty() ? tr("Add a source in this slot first.") : !selected ? tr("Waiting for the radio to answer.") : QString();
    for (QWidget* control : {static_cast<QWidget*>(m_enabled), static_cast<QWidget*>(m_remove)}) {
        control->setToolTip(reason);
        control->setAccessibleDescription(reason);
    }
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
    m_editorId = existing ? existing->id : QString();
    m_editorTab = m_tabs->currentIndex();
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

}

void NtpSourcesPage::closeEditor()
{
    m_editor->setVisible(false);
    m_drafts.remove(m_editorTab);
    m_editorSessionCurrent = {};
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
    if (issue.isEmpty() && (!m_editorSessionCurrent || !m_editorSessionCurrent())) {
        issue = tr("The radio session changed. Select a server tab to reload its settings.");
        m_editorIssue->setText(issue);
        m_editorIssue->setVisible(true);
    }
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
        const int slot = m_tabs->currentIndex();
        if (slot >= 0 && m_slots.at(slot).isEmpty()) { m_slots[slot] = d.id; }
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
                                    "The dashboard shows what the radio reports.").arg(id),
                                 QStringLiteral("color.accent.warning"));
                break;
            case Outcome::NoAnswer:
                self->setMessage(tr("While %1 was being updated, %2. The dashboard shows what the radio "
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
