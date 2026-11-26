#include "models/MicrohamTrioRadioLink.h"

#include "core/MicrohamTrioProtocol.h"
#include "core/RadioSettingsScope.h"
#include "models/MicrohamTrioModel.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QString>

namespace AetherSDR {

MicrohamTrioRadioLink::MicrohamTrioRadioLink(RadioModel* radio, MicrohamTrioModel* trio,
                                             QObject* parent)
    : QObject(parent)
    , m_radio(radio)
    , m_trio(trio)
{
    if (radio == nullptr || trio == nullptr) {
        return;
    }
    // Every command of a pick asks whether the radio may radiate.
    const QPointer<RadioModel> guard(radio);
    trio->setSwitchBlocked([guard]() { return guard.isNull() || guard->stationAntennaSwitchBlocked(); });
    radio->addStationAntennaSource(
        this, QLatin1String(kSourceId), [this]() { return antennaView(); },
        [this](const QString& token, bool tx) { return selectAntenna(token, tx); });
    connect(trio, &MicrohamTrioModel::changed, this, [this]() {
        if (!m_radio.isNull()) {
            m_radio->notifyStationAntennasChanged();
        }
    });
    connect(radio, &RadioModel::connectionStateChanged, this, [this](bool) { refreshRadio(); });
    connect(radio, &RadioModel::infoChanged, this, &MicrohamTrioRadioLink::refreshRadio);
    refreshRadio();
}

void MicrohamTrioRadioLink::refreshRadio()
{
    if (m_radio.isNull() || m_trio.isNull()) {
        return;
    }
    m_trio->setRadio(m_radio->isConnected() ? m_radio->settingsScope() : RadioSettingsScope());
}

namespace {

QString portLabel(const MicrohamTrio::Snapshot& snapshot, const QString& key)
{
    const MicrohamTrio::Button* button = snapshot.antenna.find(key);
    return button != nullptr && !button->label.isEmpty() ? button->label : key;
}

// What picking `key` in one field does: split is implied by RX differing from TX.
QString pickHint(const MicrohamTrio::Snapshot& snapshot, const QString& key, bool tx)
{
    const auto text = [](const char* source) {
        return QCoreApplication::translate("MicrohamTrioRadioLink", source);
    };
    const QString other = tx ? snapshot.antennaRx : snapshot.antennaTx;
    if (tx && !snapshot.split.on) {
        return text("TX and RX on this port");
    }
    if (key == other) {
        return snapshot.split.on ? text("TX and RX on this port; split off")
                                 : text("TX and RX on this port");
    }
    const QString otherLabel = portLabel(snapshot, other);
    if (tx) {
        return text("TX only; RX stays on %1").arg(otherLabel);
    }
    return snapshot.split.on ? text("RX only; TX stays on %1").arg(otherLabel)
                             : text("RX only: split on, TX stays on %1").arg(otherLabel);
}

} // namespace

StationAntennaView MicrohamTrioRadioLink::antennaView() const
{
    StationAntennaView out;
    if (m_radio.isNull() || m_trio.isNull() || !m_radio->isConnected()) {
        return out;
    }
    const MicrohamTrioModel::View view = m_trio->view();
    // Offered only for a radio the operator has set to a Trio port.
    if (!view.configured || !view.radioKnown
        || view.binding != MicrohamTrioBinding::Stored::Bound) {
        return out;
    }
    out.active = true;
    out.heading = tr("microHAM Trio");
    if (!view.snapshot) {
        out.rxLabel = out.txLabel = tr("Trio ?");
        out.unavailable = m_trio->unavailableReason();
        if (out.unavailable.isEmpty()) {
            out.unavailable = tr("The Trio has not been read yet.");
        }
        return out;
    }
    const MicrohamTrio::Snapshot& snapshot = *view.snapshot;
    const QString prefix = QLatin1String(kTokenPrefix);
    out.unavailable = m_trio->unavailableReason();
    for (const MicrohamTrio::Button& button : snapshot.antenna.buttons) {
        for (const bool tx : {false, true}) {
            // Offline, held by another client, focus elsewhere, transmitting:
            // the ports stay listed, disabled, with the reason.
            const MicrohamTrioModel::WriteVerdict verdict =
                tx ? m_trio->canSelectTxAntenna(button.key) : m_trio->canSelectRxAntenna(button.key);
            StationAntennaChoice choice;
            choice.token = prefix + button.key;
            choice.label = button.label.isEmpty() ? button.key : button.label;
            choice.enabled = verdict.allowed;
            choice.hint = pickHint(snapshot, button.key, tx);
            choice.reason = verdict.message;
            (tx ? out.tx : out.rx).append(choice);
        }
    }
    if (snapshot.radio.selected != view.slotKey) {
        // Another radio holds focus: the antenna shown is that radio's.
        const MicrohamTrio::Button* focus = snapshot.radio.selectedButton();
        const QString name = focus != nullptr && !focus->label.isEmpty() ? focus->label
                                                                         : snapshot.radio.selected;
        out.rxLabel = out.txLabel = tr("Trio: %1").arg(name);
        return out;
    }
    out.rxCurrent = prefix + snapshot.antennaRx;
    out.txCurrent = prefix + snapshot.antennaTx;
    out.rxLabel = portLabel(snapshot, snapshot.antennaRx);
    out.txLabel = portLabel(snapshot, snapshot.antennaTx);
    if (snapshot.split.on) {
        out.note = tr("Split: RX on %1, TX on %2.").arg(out.rxLabel, out.txLabel);
    }
    return out;
}

QString MicrohamTrioRadioLink::selectAntenna(const QString& token, bool tx)
{
    if (m_trio.isNull() || !token.startsWith(QLatin1String(kTokenPrefix))) {
        return tr("That antenna is not on offer now.");
    }
    // Asked again here: this is the last step before the command leaves.
    if (!m_radio.isNull() && m_radio->stationAntennaSwitchBlocked()) {
        return stationAntennaTransmitRefusal();
    }
    const QString key = token.mid(static_cast<int>(qstrlen(kTokenPrefix)));
    const MicrohamTrioModel::WriteVerdict verdict =
        tx ? m_trio->selectTxAntenna(key) : m_trio->selectRxAntenna(key);
    return verdict.allowed ? QString() : verdict.message;
}

} // namespace AetherSDR
