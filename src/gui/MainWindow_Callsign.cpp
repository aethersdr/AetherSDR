// MainWindow_Callsign.cpp — QRZ callsign-lookup wiring: CwCallsignSpotter (fed
// by the CW decode text; fires on "DE <call>"), CallsignLookupService (QRZ XML
// + 7-day disk cache), the opt-in live contacts window, and manual callsign
// lookup. The service is surface-agnostic.

#include "MainWindow.h"

#include "CallsignCard.h"
#include "CallsignLookupDialog.h"
#include "PanadapterApplet.h"
#include "core/CallsignLookupService.h"
#include "core/LogManager.h"
#include "core/MaidenheadLocator.h"
#include "models/RadioModel.h"
#include "models/CwDecodeSettings.h"

#include <QAction>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QScreen>

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

    // Results → the live contacts window. Match on the card's current
    // call so a dialog-initiated lookup for a different station doesn't
    // repaint the decoder card (and vice versa — the dialog filters too).
    connect(&svc, &CallsignLookupService::infoReady, this,
            [this](const CallsignInfo& info, bool fromCache) {
        if (!m_liveCwContactsAction->isChecked() || !m_liveCwContactsDialog
            || !m_liveCwContactsDialog->isVisible()) {
            return;
        }
        CallsignCard* card = m_liveCwContactsDialog->card();
        if (!card->isVisible() || card->currentCall() != info.call) {
            return;
        }
        card->showInfo(info, fromCache);
        const QString photo = CallsignLookupService::instance().photoPathFor(info.call);
        if (!photo.isEmpty())
            card->setPhotoPath(photo);
    });
    connect(&svc, &CallsignLookupService::photoReady, this,
            [this](const QString& call, const QString& imagePath) {
        if (m_liveCwContactsAction->isChecked() && m_liveCwContactsDialog
            && m_liveCwContactsDialog->isVisible()) {
            CallsignCard* card = m_liveCwContactsDialog->card();
            if (card->isVisible() && card->currentCall() == call) {
                card->setPhotoPath(imagePath);
            }
        }
    });
    connect(&svc, &CallsignLookupService::lookupFailed, this,
            [this](const QString& call, const QString& message) {
        if (m_liveCwContactsAction->isChecked() && m_liveCwContactsDialog
            && m_liveCwContactsDialog->isVisible()) {
            CallsignCard* card = m_liveCwContactsDialog->card();
            if (card->isVisible() && card->currentCall() == call) {
                card->showError(call, message);
            }
        }
    });

    if (m_liveCwContactsAction->isChecked()) {
        setLiveCwContactsVisible(true);
    }
}

void MainWindow::onCwCallsignSpotted(const QString& call)
{
    m_lastCwContactCall = call;
    if (!m_liveCwContactsAction->isChecked() || !m_liveCwContactsDialog
        || !m_liveCwContactsDialog->isVisible()) {
        return;
    }
    qCDebug(lcQrz) << "CW station identified:" << call;
    m_liveCwContactsDialog->showCallsign(call);
    CallsignLookupService::instance().lookup(call);
}

void MainWindow::setLiveCwContactsVisible(bool visible)
{
    const QByteArray geometry = m_liveCwContactsDialog
        ? m_liveCwContactsDialog->saveGeometry() : QByteArray();
    CwDecodeSettings::setLiveContactsEnabled(visible, geometry);
    if (!visible) {
        if (m_liveCwContactsDialog && m_liveCwContactsDialog->isVisible()) {
            m_liveCwContactsDialog->close();
        }
        return;
    }

    const bool firstShow = !m_liveCwContactsDialog;
    showOrRaisePersistent(m_liveCwContactsDialog, CwDecodeSettings::fontPx());
    if (firstShow) {
        LiveCwContactsDialog* dialog = m_liveCwContactsDialog;
        connect(dialog, &QDialog::finished, this, [this, dialog] {
            if (m_liveCwContactsDialog != dialog) {
                return;
            }
            CwDecodeSettings::setLiveContactsEnabled(false, dialog->saveGeometry());
            m_liveCwContactsDialog.clear();
            m_liveCwContactsAction->setChecked(false);
        });
        const QByteArray savedGeometry = CwDecodeSettings::liveContactsGeometry();
        if (!savedGeometry.isEmpty()) {
            m_liveCwContactsDialog->restoreGeometry(savedGeometry);
        } else if (m_cwDecoderApplet) {
            QWidget* decoder = m_cwDecoderApplet->findChild<QWidget*>(QStringLiteral("cwDecodePanel"));
            QScreen* display = screen();
            if (decoder && display) {
                const QRect textRect(decoder->mapToGlobal(QPoint()), decoder->size());
                const QRect available = display->availableGeometry();
                const QSize windowSize = m_liveCwContactsDialog->size();
                const QList<QPoint> candidates{
                    QPoint(available.right() - windowSize.width() + 1, available.top()),
                    available.topLeft(),
                    QPoint(available.left(), available.bottom() - windowSize.height() + 1),
                    QPoint(available.right() - windowSize.width() + 1,
                           available.bottom() - windowSize.height() + 1)};
                for (const QPoint& position : candidates) {
                    const QRect windowRect(position, windowSize);
                    if (available.contains(windowRect) && !textRect.intersects(windowRect)) {
                        m_liveCwContactsDialog->move(position);
                        break;
                    }
                }
            }
        }
    }
    if (!m_lastCwContactCall.isEmpty()) {
        onCwCallsignSpotted(m_lastCwContactCall);
    }
}

void MainWindow::clearLiveCwContact()
{
    m_lastCwContactCall.clear();
    if (m_liveCwContactsDialog) {
        m_liveCwContactsDialog->clearContact();
    }
}

void MainWindow::showCallsignLookupDialog(const QString& call)
{
    showOrRaisePersistent(m_callsignLookupDialog);
    if (!call.isEmpty())
        m_callsignLookupDialog->lookupCallsign(call);
}

} // namespace AetherSDR
