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

namespace AetherSDR {

void MainWindow::wireCallsignLookup()
{
    // Parent + objectName let the automation bridge find the spotter with
    // findChild and drive `qrz spottext` through the real detection path.
    m_cwCallsignSpotter.setParent(this);
    m_cwCallsignSpotter.setObjectName(QStringLiteral("cwCallsignSpotter"));

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
    // No account configured → no pending card that can never fill in.
    // A cached entry can still fill the card without credentials, so
    // cache hits pop regardless (also lets the offline case keep working).
    if (!svc.enabled() || (!svc.hasCredentials() && !svc.hasCachedEntry(call)))
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
