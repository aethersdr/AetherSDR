#include "models/MicrohamTrioModel.h"

#include "core/LogManager.h"
#include "core/MicrohamTrioTcpTransport.h"

#include <QStringList>

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
    if (!m_session || !m_settings.isConfigured()) {
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
    case MicrohamTrioSession::Status::Released:
        out.session = SessionWord::Released;
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
    MicrohamTrioSession::Config config;
    config.host = m_settings.host;
    config.port = static_cast<quint16>(m_settings.port);
    qCInfo(lcDevices) << "MicrohamTrioModel: taking the session at" << description();
    m_session->start(config);
}

void MicrohamTrioModel::dropSession()
{
    if (!m_session) {
        return;
    }
    MicrohamTrioSession* session = m_session;
    m_session = nullptr;
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
    m_snapshot = snapshot;
    m_failure.clear();
    const SessionWord before = m_word;
    setWord(view().session);
    if (source == MicrohamTrioSession::SnapshotSource::OnConnect
        && before != SessionWord::Direct && m_word == SessionWord::Direct) {
        emit connected();
    }
    emit changed();
}

void MicrohamTrioModel::onSessionState(MicrohamTrioSession::Status status)
{
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
    case MicrohamTrioSession::Status::Released:
        break;
    }
    setWord(view().session);
    if (before == SessionWord::Direct && m_word != SessionWord::Direct) {
        emit disconnected();
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
