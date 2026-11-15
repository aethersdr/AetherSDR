#include "SmartCatSession.h"
#include "LogManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

namespace AetherSDR {

SmartCatSession::SmartCatSession(QTcpSocket* socket, RadioModel* model,
                                 int vfoA, int vfoB, bool flexExtensions,
                                 QObject* parent)
    : QObject(parent)
    , m_protocol(model, vfoA, vfoB, flexExtensions)
    , m_socket(socket)
    , m_model(model)
{
    connect(socket, &QTcpSocket::readyRead,    this, &SmartCatSession::onReadyRead);
    connect(socket, &QTcpSocket::disconnected, this, &SmartCatSession::onDisconnected);
    socket->setParent(this);
}

SmartCatSession::~SmartCatSession()
{
    setAI(false);
    m_protocol.releasePtt();
}

void SmartCatSession::send(const QString& data)
{
    if (m_socket && m_socket->isOpen())
        m_socket->write(data.toUtf8());
}

void SmartCatSession::onReadyRead()
{
    m_buffer.append(m_socket->readAll());

    int pos;
    while ((pos = m_buffer.indexOf(';')) >= 0) {
        QString cmd = QString::fromUtf8(m_buffer.left(pos)).trimmed();
        m_buffer.remove(0, pos + 1);
        if (cmd.isEmpty()) continue;

        // AI enable/disable — handled at session level to wire/unwire signals
        const QString upper = cmd.toUpper();
        if (upper.left(2) == "AI" && upper.size() > 2) {
            setAI(upper.mid(2) == "1");
            m_protocol.setAiEnabled(m_aiEnabled);
            continue;
        }
        if (upper.left(4) == "ZZAI" && upper.size() > 4) {
            bool ok;
            int val = upper.mid(4).toInt(&ok);
            setAI(ok && val > 0);
            m_protocol.setAiEnabled(m_aiEnabled);
            continue;
        }

        QString resp = m_protocol.processCommand(cmd);
        if (!resp.isEmpty()) {
            qCDebug(lcCat) << "SmartCAT" << cmd << "->" << resp.left(40);
            send(resp);
        } else {
            qCDebug(lcCat) << "SmartCAT" << cmd << "(set)";
        }
    }
}

void SmartCatSession::onDisconnected()
{
    m_protocol.releasePtt();
    emit sessionEnded(this);
}

// ── AI mode — async frequency / mode push ───────────────────────────────────

void SmartCatSession::setAI(bool on)
{
    if (m_aiEnabled == on) return;
    m_aiEnabled = on;

    SliceModel* a = vfoA();
    if (!a) return;

    if (on) {
        connect(a, &SliceModel::frequencyChanged,
                this, &SmartCatSession::onVfoAFrequencyChanged);
        connect(a, &SliceModel::modeChanged,
                this, &SmartCatSession::onVfoAModeChanged);
    } else {
        disconnect(a, &SliceModel::frequencyChanged,
                   this, &SmartCatSession::onVfoAFrequencyChanged);
        disconnect(a, &SliceModel::modeChanged,
                   this, &SmartCatSession::onVfoAModeChanged);
    }
}

void SmartCatSession::onVfoAFrequencyChanged(double mhz)
{
    send("FA" + SmartCatProtocol::freqField(mhz) + ";");
}

void SmartCatSession::onVfoAModeChanged(const QString& mode)
{
    send("MD" + SmartCatProtocol::modeToKenwood(mode) + ";");
}

SliceModel* SmartCatSession::vfoA() const
{
    if (!m_model) return nullptr;
    const int idx = m_protocol.vfoA();
    for (auto* s : m_model->slices()) {
        if (s->sliceId() == idx) return s;
    }
    const auto& slices = m_model->slices();
    return slices.isEmpty() ? nullptr : slices.first();
}

} // namespace AetherSDR
