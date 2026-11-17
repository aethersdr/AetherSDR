#include "models/MicrohamTrioModel.h"

#include "core/LogManager.h"
#include "core/MicrohamTrioTcpTransport.h"
#include "models/StationAntenna.h"

#include <QStringList>
#include <QVector>

namespace AetherSDR {

MicrohamTrioModel::MicrohamTrioModel(QObject* parent)
    : QObject(parent)
    , m_production(true)
{
}

MicrohamTrioModel::MicrohamTrioModel(const Environment& environment, QObject* parent)
    : QObject(parent)
    , m_environment(environment)
{
}

MicrohamTrioModel::~MicrohamTrioModel()
{
    if (m_session) {
        QObject::disconnect(m_session, nullptr, this, nullptr);
        m_session->stop();
        delete m_session;
        m_session = nullptr;
    }
}

void MicrohamTrioModel::loadSettings(bool connect)
{
    abandonTest();
    dropSession();
    m_settings = MicrohamTrioSettings::load();
    m_released = false;
    m_evicted = false;
    m_failure.clear();
    m_snapshot.reset();
    if (m_settings.isConfigured() && connect) {
        startSession();
    }
    setWord(view().session);
    emit changed();
}

void MicrohamTrioModel::connectNetwork(const QString& host, quint16 port)
{
    abandonTest();
    const QString trimmed = host.trimmed();
    if (trimmed.isEmpty()) {
        removeTrio();
        return;
    }
    dropSession();
    if (trimmed != m_settings.host || port != m_settings.port) {
        m_snapshot.reset();
    }
    m_settings.host = trimmed;
    m_settings.port = port >= 1 ? port : MicrohamTrioSettings::kDefaultPort;
    m_settings.save();
    m_released = false;
    m_evicted = false;
    m_failure.clear();
    startSession();
    setWord(view().session);
    emit changed();
}

void MicrohamTrioModel::disconnect()
{
    abandonTest();
    const bool wasConnected = isConnected();
    dropSession();
    m_released = false;
    m_evicted = false;
    m_failure.clear();
    setWord(view().session);
    if (wasConnected) {
        emit disconnected();
    }
    emit changed();
}

void MicrohamTrioModel::removeTrio()
{
    abandonTest();
    const bool wasConnected = isConnected();
    dropSession();
    MicrohamTrioSettings::remove();
    m_settings = MicrohamTrioSettings{};
    m_released = false;
    m_evicted = false;
    m_failure.clear();
    m_snapshot.reset();
    setWord(view().session);
    if (wasConnected) {
        emit disconnected();
    }
    emit changed();
}

void MicrohamTrioModel::releaseForBrowser()
{
    abandonTest();
    if (!m_session) {
        return;
    }
    const bool wasConnected = isConnected();
    dropSession();
    m_released = true;
    m_evicted = false;
    m_failure.clear();
    setWord(view().session);
    if (wasConnected) {
        emit disconnected();
    }
    emit changed();
}

void MicrohamTrioModel::setAutoReconnect(bool on)
{
    m_autoReconnect = on;
}

void MicrohamTrioModel::testConnection(const QString& host, quint16 port)
{
    const QString trimmed = host.trimmed();
    if (m_testing) {
        return;
    }
    if (trimmed.isEmpty()) {
        emit testFinished(false, tr("Enter the Trio's address first."));
        return;
    }
    const bool sameTarget = trimmed.compare(m_settings.host, Qt::CaseInsensitive) == 0
                            && port == m_settings.port;
    if (sameTarget && isConnected() && m_snapshot) {
        emit testFinished(true, tr("Connected to the Trio at %1:%2 now.\n%3")
                                    .arg(trimmed).arg(port).arg(describeState(*m_snapshot)));
        return;
    }
    m_resumeAfterTest = m_session != nullptr;
    dropSession();
    m_testing = true;
    m_testTarget.host = trimmed;
    m_testTarget.port = port >= 1 ? port : MicrohamTrioSettings::kDefaultPort;
    qCInfo(lcDevices) << "MicrohamTrioModel: testing the Trio at" << trimmed << port;
    startSession();
    emit changed();
}

void MicrohamTrioModel::finishTest(bool reachable, const QString& report)
{
    dropSession();
    m_testing = false;
    qCInfo(lcDevices).noquote() << "MicrohamTrioModel: test" << (reachable ? "reached" : "failed")
                                << "-" << forLog(report);
    if (m_resumeAfterTest && m_settings.isConfigured()) {
        startSession();
    }
    m_resumeAfterTest = false;
    setWord(view().session);
    emit testFinished(reachable, report);
    emit changed();
}

// Anything that replaces the session ends a test in flight, answered.
void MicrohamTrioModel::abandonTest()
{
    if (!m_testing) {
        return;
    }
    m_testing = false;
    m_resumeAfterTest = false;
    emit testFinished(false, tr("Test cancelled."));
}

void MicrohamTrioModel::setRadio(const RadioSettingsScope& scope)
{
    m_scope = scope;
    m_binding = MicrohamTrioBinding::load(scope);
    emit changed();
}

MicrohamTrioModel::BindResult MicrohamTrioModel::bindRadioSlot(const QString& slotKey)
{
    if (!m_settings.isConfigured()) {
        return BindResult::NoTrio;
    }
    if (!MicrohamTrioBinding::scopeNamesOneRadio(m_scope)) {
        return BindResult::NoRadioIdentity;
    }
    if (!m_snapshot) {
        return BindResult::NoState;
    }
    const MicrohamTrio::Button* button = m_snapshot->radio.find(slotKey);
    if (!button) {
        return BindResult::NoSuchPort;
    }
    MicrohamTrioBinding binding;
    binding.stored = MicrohamTrioBinding::Stored::Bound;
    binding.trioHost = m_settings.host;
    binding.slotKey = slotKey;
    binding.slotLabel = button->label;
    if (!binding.save(m_scope)) {
        return BindResult::WriteFailed;
    }
    m_binding = binding;
    emit changed();
    return BindResult::Ok;
}

MicrohamTrioModel::BindResult MicrohamTrioModel::setNotOnTrio()
{
    if (!MicrohamTrioBinding::scopeNamesOneRadio(m_scope)) {
        return BindResult::NoRadioIdentity;
    }
    MicrohamTrioBinding binding;
    binding.stored = MicrohamTrioBinding::Stored::NotOnTrio;
    if (!binding.save(m_scope)) {
        return BindResult::WriteFailed;
    }
    m_binding = binding;
    emit changed();
    return BindResult::Ok;
}

MicrohamTrioModel::BindResult MicrohamTrioModel::clearBinding()
{
    if (!MicrohamTrioBinding::scopeNamesOneRadio(m_scope)) {
        return BindResult::NoRadioIdentity;
    }
    if (!MicrohamTrioBinding::remove(m_scope)) {
        return BindResult::WriteFailed;
    }
    m_binding = MicrohamTrioBinding{};
    emit changed();
    return BindResult::Ok;
}

QString MicrohamTrioModel::description() const
{
    if (!m_session || m_testing || !m_settings.isConfigured()) {
        return {};
    }
    return QStringLiteral("%1:%2").arg(m_settings.host).arg(m_settings.port);
}

MicrohamTrioModel::View MicrohamTrioModel::view() const
{
    View out;
    out.configured = m_settings.isConfigured();
    out.host = m_settings.host;
    out.port = m_settings.port;
    out.snapshot = m_snapshot;
    out.failure = m_failure;
    out.radioKnown = MicrohamTrioBinding::scopeNamesOneRadio(m_scope);
    out.binding = out.radioKnown ? m_binding.stored : MicrohamTrioBinding::Stored::None;
    if (out.binding == MicrohamTrioBinding::Stored::Bound) {
        out.slotKey = m_binding.slotKey;
        out.slotLabel = m_binding.slotLabel;
        const MicrohamTrio::Button* now = m_snapshot ? m_snapshot->radio.find(m_binding.slotKey)
                                                     : nullptr;
        out.slotLabelNow = now ? now->label : QString();
        if (m_settings.isConfigured()
            && m_binding.trioHost.compare(m_settings.host, Qt::CaseInsensitive) != 0) {
            out.bindingIssue = tr("bound against another Trio (%1)").arg(m_binding.trioHost);
        } else if (m_snapshot && !now) {
            out.bindingIssue = tr("the Trio has no radio port %1 any more").arg(m_binding.slotKey);
        } else if (now && now->label != m_binding.slotLabel) {
            out.bindingIssue = tr("radio port %1 now reads \u201c%2\u201d; it was \u201c%3\u201d when "
                                  "this radio was set to it")
                                   .arg(m_binding.slotKey, now->label, m_binding.slotLabel);
        }
    } else if (out.binding == MicrohamTrioBinding::Stored::Unreadable) {
        out.bindingIssue = tr("the stored Trio port of this radio cannot be read; set it again");
    }
    if (!out.configured) {
        out.session = SessionWord::NotConfigured;
        return out;
    }
    if (!m_session) {
        out.session = m_evicted ? SessionWord::Evicted
            : m_released ? SessionWord::Released
            : !m_failure.isEmpty() ? SessionWord::Offline
            : SessionWord::Off;
        return out;
    }
    if (m_testing) {
        out.session = SessionWord::Connecting;
        return out;
    }
    out.stateCurrent = m_session->snapshotCurrent();
    switch (m_session->status()) {
    case MicrohamTrioSession::Status::Held:
        out.session = out.stateCurrent ? SessionWord::Direct : SessionWord::Connecting;
        break;
    case MicrohamTrioSession::Status::Failed:
        out.session = SessionWord::Offline;
        break;
    case MicrohamTrioSession::Status::Evicted:
        out.session = SessionWord::Evicted;
        break;
    case MicrohamTrioSession::Status::Idle:
    case MicrohamTrioSession::Status::Connecting:
        out.session = SessionWord::Connecting;
        break;
    }
    return out;
}

void MicrohamTrioModel::startSession()
{
    if (m_production && !m_ownTransport) {
        m_ownTransport = std::make_unique<MicrohamTrioTcpTransport>();
        m_ownClock = std::make_unique<MicrohamTrioMonotonicClock>();
        m_environment.transport = m_ownTransport.get();
        m_environment.clock = m_ownClock.get();
        m_environment.entropy = MicrohamTrioSession::systemEntropy();
    }
    if (!m_environment.transport || !m_environment.clock) {
        qCWarning(lcDevices) << "MicrohamTrioModel: no transport; no session";
        return;
    }
    m_session = new MicrohamTrioSession(m_environment.transport, m_environment.clock,
                                        m_environment.entropy, this);
    connect(m_session, &MicrohamTrioSession::snapshotChanged,
            this, &MicrohamTrioModel::onSnapshot);
    connect(m_session, &MicrohamTrioSession::sessionStateChanged,
            this, &MicrohamTrioModel::onSessionState);
    connect(m_session, &MicrohamTrioSession::writeResult,
            this, &MicrohamTrioModel::onWriteResult);
    MicrohamTrioSession::Config config;
    const MicrohamTrioSettings& target = m_testing ? m_testTarget : m_settings;
    config.host = target.host;
    config.port = static_cast<quint16>(target.port);
    qCInfo(lcDevices) << "MicrohamTrioModel: taking the session at"
                      << QStringLiteral("%1:%2").arg(config.host).arg(config.port);
    m_session->start(config);
}

void MicrohamTrioModel::dropSession()
{
    if (!m_session) {
        return;
    }
    MicrohamTrioSession* session = m_session;
    m_session = nullptr;
    m_goal.reset();
    QObject::disconnect(session, nullptr, this, nullptr);
    session->stop();
    // Possibly inside one of its own signals.
    session->deleteLater();
}

void MicrohamTrioModel::setWord(SessionWord word)
{
    m_word = word;
}

void MicrohamTrioModel::onSnapshot(const MicrohamTrio::Snapshot& snapshot,
                                   MicrohamTrioSession::SnapshotSource source)
{
    if (m_testing) {
        // Read and released at once; the shown state is not replaced by a test
        // of another address.
        finishTest(true, tr("Reachable: the Trio at %1:%2 answered; session released.\n%3")
                             .arg(m_testTarget.host).arg(m_testTarget.port)
                             .arg(describeState(snapshot)));
        return;
    }
    m_snapshot = snapshot;
    m_failure.clear();
    const SessionWord before = m_word;
    setWord(view().session);
    if (source == MicrohamTrioSession::SnapshotSource::OnConnect) {
        // One line per connect: what the Trio reported, for the log.
        qCInfo(lcDevices).noquote() << "MicrohamTrioModel: state on connect:"
                                    << forLog(describeState(snapshot));
    }
    if (source == MicrohamTrioSession::SnapshotSource::OnConnect
        && before != SessionWord::Direct && m_word == SessionWord::Direct) {
        emit connected();
    }
    emit changed();
}

void MicrohamTrioModel::onSessionState(MicrohamTrioSession::Status status)
{
    if (m_testing) {
        if (status == MicrohamTrioSession::Status::Failed) {
            finishTest(false, tr("Not reachable: %1.")
                                  .arg(m_session ? failureText(m_session->lastFailure())
                                                 : tr("connection failed")));
        } else if (status == MicrohamTrioSession::Status::Evicted) {
            finishTest(false, tr("The Trio closed the session before its state arrived; another "
                                 "client may hold it."));
        }
        return;
    }
    const SessionWord before = m_word;
    switch (status) {
    case MicrohamTrioSession::Status::Evicted:
        // Never retaken: that would evict the other client in turn.
        dropSession();
        m_evicted = true;
        m_failure = tr("another client took the Trio's session (its own web page, or another "
                       "program); click Connect to take it back");
        qCInfo(lcDevices) << "MicrohamTrioModel: session taken by another client; not retaken";
        emit connectionFailed(m_failure);
        break;
    case MicrohamTrioSession::Status::Failed:
        m_failure = m_session ? failureText(m_session->lastFailure()) : tr("connection failed");
        qCInfo(lcDevices) << "MicrohamTrioModel: attempt failed:" << m_failure;
        if (!m_autoReconnect) {
            dropSession();
        }
        emit connectionFailed(m_failure);
        break;
    case MicrohamTrioSession::Status::Idle:
    case MicrohamTrioSession::Status::Connecting:
    case MicrohamTrioSession::Status::Held:
        break;
    }
    setWord(view().session);
    if (before == SessionWord::Direct && m_word != SessionWord::Direct) {
        emit disconnected();
    }
    emit changed();
}

namespace {

QString antennaLabel(const MicrohamTrio::Snapshot& snapshot, const QString& key)
{
    const MicrohamTrio::Button* button = snapshot.antenna.find(key);
    return button != nullptr && !button->label.isEmpty() ? button->label : key;
}

} // namespace

QString MicrohamTrioModel::unavailableReason() const
{
    const View view = this->view();
    if (!view.configured) {
        return tr("No Trio is configured.");
    }
    if (view.binding != MicrohamTrioBinding::Stored::Bound) {
        return tr("Set this radio's Trio port in Radio Setup \u2192 Peripherals first.");
    }
    if (!view.bindingIssue.isEmpty()) {
        return tr("Trio label error: %1.").arg(view.bindingIssue);
    }
    if (view.session == SessionWord::Evicted) {
        return tr("Another client holds the Trio's session.");
    }
    if (view.session != SessionWord::Direct || !view.stateCurrent || !view.snapshot || !m_session) {
        return tr("The Trio is not connected.");
    }
    const MicrohamTrio::Snapshot& snapshot = *view.snapshot;
    if (snapshot.radio.selected != view.slotKey) {
        const MicrohamTrio::Button* focus = snapshot.radio.selectedButton();
        return tr("The Trio's focus is on %1, not on this radio.")
            .arg(focus && !focus->label.isEmpty() ? focus->label : snapshot.radio.selected);
    }
    return {};
}

MicrohamTrioModel::WriteVerdict MicrohamTrioModel::pickVerdict(bool tx, const QString& key) const
{
    WriteVerdict out;
    out.message = unavailableReason();
    if (!out.message.isEmpty()) {
        return out;
    }
    const MicrohamTrio::Snapshot& snapshot = *m_snapshot;
    if (m_goal || m_session->writePending()) {
        out.message = tr("The Trio has not finished the last pick yet.");
        return out;
    }
    if (m_switchBlocked && m_switchBlocked()) {
        out.message = stationAntennaTransmitRefusal();
        return out;
    }
    const MicrohamTrio::Button* button = snapshot.antenna.find(key);
    if (button == nullptr) {
        out.message = tr("The Trio has no antenna %1.").arg(key);
        return out;
    }
    const AntennaGoal goal = goalFor(tx, key, snapshot);
    // The port that ends up carrying TX must not be marked RX only.
    if (!button->enabled && (tx || !goal.split)) {
        out.message = tr("The Trio marks %1 as receive only.").arg(antennaLabel(snapshot, key));
        return out;
    }
    out.allowed = true;
    return out;
}

MicrohamTrioModel::WriteVerdict MicrohamTrioModel::canSelectTxAntenna(const QString& key) const
{
    return pickVerdict(true, key);
}

MicrohamTrioModel::WriteVerdict MicrohamTrioModel::canSelectRxAntenna(const QString& key) const
{
    return pickVerdict(false, key);
}

MicrohamTrioModel::WriteVerdict MicrohamTrioModel::selectTxAntenna(const QString& key)
{
    return startPick(true, key);
}

MicrohamTrioModel::WriteVerdict MicrohamTrioModel::selectRxAntenna(const QString& key)
{
    return startPick(false, key);
}

MicrohamTrioModel::AntennaGoal MicrohamTrioModel::goalFor(bool tx, const QString& key,
                                                          const MicrohamTrio::Snapshot& s)
{
    AntennaGoal goal;
    const QString txNow = s.antennaTx;
    const QString rxNow = s.antennaRx;
    if (tx) {
        // Without split TX moves RX with it; in split a TX pick equal to the
        // RX half ends split there.
        if (!s.split.on || key == rxNow) {
            goal.split = false;
            goal.tx = goal.rx = key;
        } else {
            goal.split = true;
            goal.tx = key;
            goal.rx = rxNow;
        }
    } else {
        // An RX pick other than TX is split; equal to TX it is not.
        if (key == txNow) {
            goal.split = false;
            goal.tx = goal.rx = txNow;
        } else {
            goal.split = true;
            goal.tx = txNow;
            goal.rx = key;
        }
    }
    return goal;
}

std::optional<MicrohamTrioModel::Step> MicrohamTrioModel::nextStep(const AntennaGoal& goal,
                                                                   const MicrohamTrio::Snapshot& s)
{
    using MicrohamTrio::AntennaField;
    if (!goal.split) {
        if (s.split.on) {
            return Step{AntennaField::Split, QLatin1String(MicrohamTrio::kToggleOff)};
        }
        if (s.antenna.selected != goal.tx) {
            return Step{AntennaField::Both, goal.tx};
        }
        return std::nullopt;
    }
    if (!s.split.on) {
        return Step{AntennaField::Split, QLatin1String(MicrohamTrio::kToggleOn)};
    }
    if (s.antennaTx != goal.tx) {
        return Step{AntennaField::TxHalf, goal.tx};
    }
    if (s.antennaRx != goal.rx) {
        return Step{AntennaField::RxHalf, goal.rx};
    }
    return std::nullopt;
}

MicrohamTrioModel::WriteVerdict MicrohamTrioModel::startPick(bool tx, const QString& key)
{
    WriteVerdict out = pickVerdict(tx, key);
    if (!out.allowed) {
        return out;
    }
    const AntennaGoal goal = goalFor(tx, key, *m_snapshot);
    if (!nextStep(goal, *m_snapshot)) {
        return out;   // already in place
    }
    qCInfo(lcDevices) << "MicrohamTrioModel:" << (tx ? "TX" : "RX") << "pick" << key
                      << "-> split" << goal.split << "TX" << goal.tx << "RX" << goal.rx;
    m_goal = goal;
    continuePick();
    out.sent = m_goal.has_value() || m_session->writePending();
    if (!out.sent) {
        out.allowed = false;
        out.message = tr("The pick could not be sent to the Trio.");
    }
    emit changed();
    return out;
}

void MicrohamTrioModel::continuePick()
{
    if (!m_goal) {
        return;
    }
    if (!m_session || !m_snapshot) {
        stopPick(tr("the Trio is not connected"));
        return;
    }
    const std::optional<Step> step = nextStep(*m_goal, *m_snapshot);
    if (!step) {
        m_goal.reset();
        emit changed();
        return;
    }
    // Asked before every command, not only the first.
    if (m_switchBlocked && m_switchBlocked()) {
        stopPickSaying(stationAntennaTransmitRefusal());
        return;
    }
    if (m_goal->writesLeft <= 0) {
        stopPick(tr("the Trio did not reach it in three commands"));
        return;
    }
    --m_goal->writesLeft;
    if (m_session->write(step->field, step->key) != MicrohamTrioSession::WriteRefusal::None) {
        stopPick(tr("the Trio's state does not allow the next command"));
    }
}

void MicrohamTrioModel::stopPick(const QString& why)
{
    stopPickSaying(tr("The Trio antenna pick stopped: %1.").arg(why));
}

void MicrohamTrioModel::stopPickSaying(const QString& sentence)
{
    if (!m_goal) {
        return;
    }
    m_goal.reset();
    qCInfo(lcDevices).noquote() << "MicrohamTrioModel: pick stopped:" << sentence;
    emit notice(sentence);
    emit changed();
}

void MicrohamTrioModel::onWriteResult(MicrohamTrio::AntennaField field, const QString& key,
                                      MicrohamTrioSession::WriteOutcome outcome)
{
    Q_UNUSED(field);
    const QString label = m_snapshot ? antennaLabel(*m_snapshot, key) : key;
    switch (outcome) {
    case MicrohamTrioSession::WriteOutcome::Confirmed:
        continuePick();
        break;
    case MicrohamTrioSession::WriteOutcome::Disagrees:
        stopPick(tr("the Trio did not take %1").arg(label));
        break;
    case MicrohamTrioSession::WriteOutcome::ReplyOverdue:
        stopPick(tr("the Trio did not answer; reading its state again"));
        break;
    }
    emit changed();
}

QString MicrohamTrioModel::failureText(MicrohamTrioSession::Failure failure)
{
    using F = MicrohamTrioSession::Failure;
    switch (failure) {
    case F::None:              return tr("connection failed");
    case F::HostUnusable:      return tr("the address cannot be used");
    case F::ConnectFailed:     return tr("no answer at that address");
    case F::ConnectTimeout:    return tr("no answer in time");
    case F::ClosedEarly:       return tr("the Trio closed the connection before the session opened");
    case F::CookieMissing:     return tr("the Trio's page did not start a session");
    case F::CookieRefused:     return tr("the Trio refused the session");
    case F::UpgradeRedirected:
    case F::UpgradeRefused:
    case F::HandshakeInvalid:  return tr("the device at that address did not answer as a Trio");
    case F::ErrorPage:         return tr("the Trio answered with an error page");
    case F::NoStateDocument:   return tr("the Trio sent no state");
    case F::UnreadableState:   return tr("the Trio's state is not in the form this version reads");
    case F::ProtocolError:     return tr("the Trio sent a frame this version cannot read");
    case F::ConnectionLost:    return tr("the connection was lost");
    case F::SessionSilent:     return tr("the Trio stopped answering");
    }
    return tr("connection failed");
}

namespace {

QString buttonText(const MicrohamTrio::Selector& selector, const QString& key)
{
    const MicrohamTrio::Button* button = selector.find(key);
    if (!button) {
        return key;
    }
    return QStringLiteral("%1 “%2”").arg(button->abbreviation, button->label);
}

QString labelsLine(const QVector<MicrohamTrio::Button>& buttons)
{
    QStringList parts;
    for (const MicrohamTrio::Button& button : buttons) {
        QString part = QStringLiteral("%1 %2").arg(button.abbreviation, button.label);
        if (!button.enabled) {
            part += MicrohamTrioModel::tr(" (RX only)");
        }
        parts.append(part);
    }
    return parts.join(QStringLiteral(" · "));
}

QString onOff(bool on)
{
    return on ? MicrohamTrioModel::tr("on") : MicrohamTrioModel::tr("off");
}

} // namespace

QString MicrohamTrioModel::forLog(const QString& text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        if (c == QLatin1Char('\n')) {
            out += QStringLiteral(" | ");
        } else if (c.category() == QChar::Other_Control) {
            out += QChar(QChar::ReplacementCharacter);
        } else {
            out += c;
        }
    }
    return out;
}

QString MicrohamTrioModel::describeBinding(const View& view)
{
    if (!view.radioKnown) {
        return tr("This radio: no radio connected");
    }
    QString text;
    switch (view.binding) {
    case MicrohamTrioBinding::Stored::None:
        text = tr("This radio: not set");
        break;
    case MicrohamTrioBinding::Stored::NotOnTrio:
        text = tr("This radio: not on the Trio");
        break;
    case MicrohamTrioBinding::Stored::Unreadable:
        text = tr("This radio: unreadable setting");
        break;
    case MicrohamTrioBinding::Stored::Bound:
        text = tr("This radio: Trio %1 \u201c%2\u201d").arg(view.slotKey, view.slotLabel);
        if (view.snapshot && view.bindingIssue.isEmpty()) {
            const bool focus = view.snapshot->radio.selected == view.slotKey;
            text += focus ? tr(" (has focus)") : tr(" (focus is elsewhere)");
        }
        break;
    }
    if (!view.bindingIssue.isEmpty()) {
        text += QStringLiteral(" \u2014 ") + tr("LABEL ERROR: %1").arg(view.bindingIssue);
    }
    return text;
}

QString MicrohamTrioModel::describeState(const MicrohamTrio::Snapshot& snapshot)
{
    QStringList lines;
    lines.append(tr("Radio with focus: %1").arg(buttonText(snapshot.radio, snapshot.radio.selected)));
    if (snapshot.split.on) {
        lines.append(tr("Antenna: TX %1, RX %2 (split)")
                         .arg(buttonText(snapshot.antenna, snapshot.antennaTx),
                              buttonText(snapshot.antenna, snapshot.antennaRx)));
    } else {
        lines.append(tr("Antenna: %1").arg(buttonText(snapshot.antenna, snapshot.antennaTx)));
    }
    lines.append(tr("RX antenna: %1, RX EXT %2, PA loop %3")
                     .arg(buttonText(snapshot.rxAntenna, snapshot.rxAntenna.selected),
                          onOff(snapshot.rxExt.on), onOff(snapshot.pa.on)));
    lines.append(tr("Radios: %1").arg(labelsLine(snapshot.radio.buttons)));
    lines.append(tr("Antennas: %1").arg(labelsLine(snapshot.antenna.buttons)));
    lines.append(tr("RX antennas: %1").arg(labelsLine(snapshot.rxAntenna.buttons)));
    return lines.join(QLatin1Char('\n'));
}

} // namespace AetherSDR
