// MainWindow_WebSdr.cpp — WebSDR receive module wiring (M1, audio).
//
// Optional, passive bolt-on. Creates the WebSdrSource on its own worker thread
// (FreeDvClient pattern) and a dockable WebSdrPanel, then wires the panel's
// intent signals to the source and the source's audio to AudioEngine via the
// RX source gate. Flex stays the authority/master — nothing here writes the
// Flex model. Compiled only when HAVE_WEBSOCKETS is defined.

#include "MainWindow.h"

#ifdef HAVE_WEBSOCKETS

#include "WebSdrPanel.h"
#include "SliceColorManager.h"
#include "core/AudioEngine.h"
#include "core/WebSdrSource.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QThread>
#include <QMenuBar>
#include <QMenu>
#include <QMetaObject>
#include <QTimer>

namespace {
// Map a Flex slice mode to a WebSDR mode suffix.
QString mapFlexMode(const QString& flex) {
    const QString m = flex.toUpper();
    if (m == "LSB" || m == "DIGL") {
        return QStringLiteral("LSB");
    }
    if (m == "USB" || m == "DIGU") {
        return QStringLiteral("USB");
    }
    if (m == "CW" || m == "CWL" || m == "CWU") {
        return QStringLiteral("CW");
    }
    if (m == "AM" || m == "SAM") {
        return QStringLiteral("AM");
    }
    if (m == "FM" || m == "NFM" || m == "DFM") {
        return QStringLiteral("FM");
    }
    return QStringLiteral("USB");
}
}

namespace AetherSDR {

void MainWindow::wireWebSdrModule()
{
    // ── Source on a dedicated worker thread ────────────────────────────────
    m_webSdrSource = new WebSdrSource;          // no parent — moved to thread
    m_webSdrThread = new QThread(this);
    m_webSdrThread->setObjectName(QStringLiteral("WebSdr"));
    m_webSdrSource->moveToThread(m_webSdrThread);
    m_webSdrThread->start();
    // Construct sockets/timers on the worker thread (#1929).
    QMetaObject::invokeMethod(m_webSdrSource, &WebSdrSource::init, Qt::QueuedConnection);

    // ── Dockable panel ─────────────────────────────────────────────────────
    // Docked + visible by default so the module is discoverable. The user can
    // close it (and reopen via the WebSDR menu) or drag it out to float.
    m_webSdrPanel = new WebSdrPanel(this);
    addDockWidget(Qt::RightDockWidgetArea, m_webSdrPanel);

    // Add the show/hide toggle to the View menu. We use the stored m_viewMenu
    // pointer (set in buildMenuBar): the menu bar gets reparented into the
    // TitleBar, so menuBar() no longer returns the populated bar here.
    QAction* toggle = m_webSdrPanel->toggleViewAction();
    toggle->setText(tr("WebSDR Panel"));
    if (m_viewMenu) { m_viewMenu->addSeparator(); m_viewMenu->addAction(toggle); }

    // ── Panel → Source (queued across threads) ─────────────────────────────
    connect(m_webSdrPanel, &WebSdrPanel::connectRequested,
            m_webSdrSource, &WebSdrSource::connectToServer);
    connect(m_webSdrPanel, &WebSdrPanel::disconnectRequested,
            m_webSdrSource, &WebSdrSource::disconnectFromServer);
    connect(m_webSdrPanel, &WebSdrPanel::tuneRequested,
            m_webSdrSource, &WebSdrSource::tune);

    // ── Panel → AudioEngine source gate ────────────────────────────────────
    connect(m_webSdrPanel, &WebSdrPanel::audioToWebSdrToggled,
            this, [this](bool on) {
        // Ensure the RX sink is open, then flip the gate (both on the audio thread).
        QMetaObject::invokeMethod(m_audio, &AudioEngine::startRxStream, Qt::QueuedConnection);
        QMetaObject::invokeMethod(m_audio, [this, on] { m_audio->setRxSourceWebSdr(on); },
                                  Qt::QueuedConnection);
    });

    // ── Follow a chosen Flex slice (read-only) ─────────────────────────────
    // Per-slice buttons (A/B/…) select which slice to follow. We poll it and
    // drive the panel's freq/mode/passband. Reading the Flex model only.
    auto rebuildSliceButtons = [this] {
        QList<int> ids; QStringList labels, colors;
        for (SliceModel* s : m_radioModel.slices()) {
            ids << s->sliceId();
            labels << s->letter();
            colors << SliceColorManager::instance().hexActive(s->sliceId());
        }
        m_webSdrPanel->setSliceButtons(ids, labels, colors);
    };
    rebuildSliceButtons();
    connect(&m_radioModel, &RadioModel::sliceAdded, this,
            [rebuildSliceButtons](SliceModel*) { rebuildSliceButtons(); });
    connect(&m_radioModel, &RadioModel::sliceRemoved, this,
            [rebuildSliceButtons](int) { rebuildSliceButtons(); });
    connect(&SliceColorManager::instance(), &SliceColorManager::colorsChanged, this,
            [rebuildSliceButtons] { rebuildSliceButtons(); });

    auto pushSlice = [this] {
        SliceModel* s = (m_webSdrFollowSliceId >= 0) ? m_radioModel.slice(m_webSdrFollowSliceId)
                                                     : nullptr;
        if (!s) {
            return;
        }
        m_webSdrPanel->applyExternalTune(s->frequency() * 1000.0,  // MHz -> kHz
                                         mapFlexMode(s->mode()),
                                         s->filterLow()  / 1000.0,  // Hz -> kHz
                                         s->filterHigh() / 1000.0);
    };
    auto* followTimer = new QTimer(this);
    followTimer->setInterval(400);
    connect(followTimer, &QTimer::timeout, this, pushSlice);
    connect(m_webSdrPanel, &WebSdrPanel::followSliceChanged, this,
            [this, followTimer, pushSlice](int sliceId) {
        m_webSdrFollowSliceId = sliceId;
        if (sliceId >= 0) { pushSlice(); followTimer->start(); }
        else                followTimer->stop();
    });

    // ── Source → AudioEngine audio (queued to the audio thread) ────────────
    connect(m_webSdrSource, &WebSdrSource::audioReady,
            m_audio, &AudioEngine::feedWebSdrAudio);

    // ── Source → Panel UI (queued to the main thread) ──────────────────────
    connect(m_webSdrSource, &WebSdrSource::stateChanged,
            m_webSdrPanel, &WebSdrPanel::setSourceState);
    connect(m_webSdrSource, &WebSdrSource::bandSpan,
            m_webSdrPanel, &WebSdrPanel::setBandSpan);
    connect(m_webSdrSource, &WebSdrSource::rowReady,
            m_webSdrPanel, &WebSdrPanel::addWaterfallRow);

    // ── Clean shutdown of the worker thread ────────────────────────────────
    connect(m_webSdrThread, &QThread::finished,
            m_webSdrSource, &QObject::deleteLater);
    connect(this, &QObject::destroyed, this, [this] {
        if (m_webSdrThread) { m_webSdrThread->quit(); m_webSdrThread->wait(2000); }
    });
}

} // namespace AetherSDR

#endif // HAVE_WEBSOCKETS
