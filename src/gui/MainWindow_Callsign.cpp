// MainWindow_Callsign.cpp — QRZ callsign-lookup wiring for MainWindow.
//
// Connects the pieces of the callsign-lookup subsystem:
//
//   • CwCallsignSpotter — fed by the CW decode panel's RX text stream
//     (routeCwDecoderOutput() re-targets the feed on slice change) — fires
//     when a station identifies itself ("DE KI6BCJ KI6BCJ")
//   • CallsignLookupService — QRZ.com XML client + 7-day on-disk cache
//   • CallsignCard on the CW decode panel — the screen-pop
//   • CallsignLookupDialog — View → Callsign Lookup manual lookups
//
// The service is surface-agnostic: the future SSB voice-callsign decoder
// pops the same card from its own detection path.

#include "MainWindow.h"

#include "CallsignCard.h"
#include "CallsignLookupDialog.h"
#include "PanadapterApplet.h"
#include "core/CallsignLookupService.h"
#include "core/LogManager.h"
#include "core/MaidenheadLocator.h"
#include "models/RadioModel.h"

namespace AetherSDR {

namespace {

// Operator position for card distance/bearing: GPS fix when the radio has
// one, else the radio's grid-locator center (same preference order as the
// PSK Reporter map's home position).
void pushOwnLocationFromRadio(RadioModel* radio)
{
    auto& svc = CallsignLookupService::instance();
    if (!radio)
        return;
    bool ok = false;
    double lat = radio->gpsLat().toDouble(&ok);
    double lon = 0.0;
    if (ok)
        lon = radio->gpsLon().toDouble(&ok);
    if (!ok || (lat == 0.0 && lon == 0.0)) {
        if (!MaidenheadLocator::toLatLon(radio->gpsGrid(), lat, lon))
            return;  // keep whatever the service already has
    }
    svc.setOwnLocation(lat, lon);
}

} // namespace

void MainWindow::wireCallsignLookup()
{
    // Parent + objectName let the automation bridge find the spotter with
    // findChild and drive `qrz spottext` through the real detection path.
    m_cwCallsignSpotter.setParent(this);
    m_cwCallsignSpotter.setObjectName(QStringLiteral("cwCallsignSpotter"));

    // Country-level prefix fallback data (cty.dat is parsed once, by the
    // DXCC spot-coloring provider).
    CallsignLookupService::instance().setCtyParser(m_dxccProvider.ctyParser());

    // Distance/bearing needs the operator's own position; follow the
    // radio's GPS (or grid) as it becomes available and as it updates.
    connect(&m_radioModel, &RadioModel::gpsStatusChanged, this,
            [this] { pushOwnLocationFromRadio(&m_radioModel); });
    pushOwnLocationFromRadio(&m_radioModel);

    // No GPS? The operator's own QRZ record carries their grid — the radio
    // callsign keys that zero-config fallback (GPS overrides when present).
    connect(&m_radioModel, &RadioModel::callsignChanged, this, [this] {
        CallsignLookupService::instance().setOwnCallsign(m_radioModel.callsign());
    });
    if (!m_radioModel.callsign().isEmpty())
        CallsignLookupService::instance().setOwnCallsign(m_radioModel.callsign());

    connect(&m_cwCallsignSpotter, &CwCallsignSpotter::callsignSpotted,
            this, &MainWindow::onCwCallsignSpotted);

    auto& svc = CallsignLookupService::instance();

    // Results → the CW decode panel's card.  Match on the card's current
    // call so a dialog-initiated lookup for a different station doesn't
    // repaint the decoder card (and vice versa — the dialog filters too).
    connect(&svc, &CallsignLookupService::infoReady, this,
            [this](const CallsignInfo& info, bool fromCache) {
        if (!m_cwDecoderApplet)
            return;
        auto* card = m_cwDecoderApplet->cwCallsignCard();
        if (!card || !card->isVisible() || card->currentCall() != info.call)
            return;
        card->showInfo(info, fromCache);
        const QString photo = CallsignLookupService::instance().photoPathFor(info.call);
        if (!photo.isEmpty())
            card->setPhotoPath(photo);
    });
    connect(&svc, &CallsignLookupService::photoReady, this,
            [this](const QString& call, const QString& imagePath) {
        if (!m_cwDecoderApplet)
            return;
        auto* card = m_cwDecoderApplet->cwCallsignCard();
        if (card && card->isVisible() && card->currentCall() == call)
            card->setPhotoPath(imagePath);
    });
    connect(&svc, &CallsignLookupService::lookupFailed, this,
            [this](const QString& call, const QString& message) {
        if (!m_cwDecoderApplet)
            return;
        auto* card = m_cwDecoderApplet->cwCallsignCard();
        if (card && card->isVisible() && card->currentCall() == call)
            card->showError(call, message);
    });
}

void MainWindow::onCwCallsignSpotted(const QString& call)
{
    auto& svc = CallsignLookupService::instance();
    // Only pop a card that can actually fill in: QRZ credentials, a cache
    // entry, or at least cty.dat prefix data (country-level fallback card).
    if (!svc.enabled() || !svc.canResolve(call))
        return;
    if (!m_cwDecoderApplet)
        return;
    auto* card = m_cwDecoderApplet->cwCallsignCard();
    if (!card)
        return;

    qCDebug(lcQrz) << "CW station identified:" << call;
    card->showPending(call);
    card->setVisible(true);
    svc.lookup(call);
}

void MainWindow::showCallsignLookupDialog(const QString& call)
{
    showOrRaisePersistent(m_callsignLookupDialog);
    if (!call.isEmpty())
        m_callsignLookupDialog->lookupCallsign(call);
}

} // namespace AetherSDR
