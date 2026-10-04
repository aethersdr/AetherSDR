// MainWindow_AetherClock.cpp — AetherClock wiring: AetherClockEngine (decodes
// WWV/WWVB from the bound slice's DAX audio; holds DAX via the injected
// provider), AetherClockModel (Q_PROPERTY mirror for the strip applet and
// `get clock`), AetherClockApplet (attach(); slice via AppletPanel::setSlice).
// The pan stream exists only with a live session: the provider resolves
// panStream() at call time and daxPcmReady is connected on runningChanged(true)
// and torn down on false. The engine ignores PCM for other channels/slices.

#include "MainWindow.h"

#include "AppletPanel.h"
#include "AetherClockApplet.h"
#include "core/AetherClockEngine.h"
#include "models/AetherClockModel.h"
#include "models/RadioModel.h"  // brings the pan stream seam (tracked baseline);
                                // no direct vendor include above the seam (EB3)

namespace AetherSDR {

void MainWindow::setupAetherClock()
{
    m_clockEngine = new AetherClockEngine(this);
    m_clockModel = new AetherClockModel(this);
    m_clockModel->attachEngine(m_clockEngine);

    // DAX hold via the central per-channel consumer registry (#3305
    // pattern). Resolved at call time — see the file comment.
    m_clockEngine->setDaxChannelProvider(
        [this](int ch) {
            if (auto* ps = m_radioModel.panStream())
                ps->acquireDaxChannel(ch, PanadapterStream::DaxConsumer::Clock);
        },
        [this](int ch) {
            if (auto* ps = m_radioModel.panStream())
                ps->releaseDaxChannel(ch, PanadapterStream::DaxConsumer::Clock);
        });

    // Whether this radio has a DAX plane at all. Resolved at call time for the
    // same reason as the provider above — no backend exists at construction.
    // Gates start()'s "no DAX channel assigned" warning, which is a correct
    // diagnosis on a Flex and a misleading one on a backend that demodulates
    // in-process and feeds feedRxSliceAudio() instead.
    m_clockEngine->setDaxAvailabilityProvider(
        [this] { return m_radioModel.hasDaxStreams(); });

    const auto bindAudio = [this] {
        disconnect(m_clockDaxConn);
        disconnect(m_clockSliceAudioConn);
        m_clockDaxConn = {};
        m_clockSliceAudioConn = {};
        if (!m_clockEngine->isRunning()) {
            return;
        }
        const quint64 generation = m_clockEngine->inputGeneration();
        if (auto* ps = m_radioModel.panStream()) {
            m_clockDaxConn = connect(
                ps, &PanadapterStream::daxPcmReady, m_clockEngine,
                [engine = m_clockEngine, generation](int channel, const PcmFrame& frame) {
                    engine->feedRxAudio(channel, frame, generation);
                }, Qt::QueuedConnection);
        }
        m_clockSliceAudioConn = connect(
            &m_radioModel, &RadioModel::backendSliceAudioFrameReady, m_clockEngine,
            [engine = m_clockEngine, generation](int sliceId, const PcmFrame& frame) {
                engine->feedRxSliceAudio(sliceId, frame, generation);
            }, Qt::QueuedConnection);
    };
    // Disconnect alone cannot cancel posted Qt events. Each production callback
    // carries the run/selection generation, checked inside the engine at receipt.
    connect(m_clockEngine, &AetherClockEngine::sourceGenerationChanged,
            this, [bindAudio](quint64) { bindAudio(); });
    connect(&m_radioModel, &RadioModel::connectionStateChanged, m_clockEngine,
            [engine = m_clockEngine](bool connected) {
                if (!connected) {
                    engine->stop();
                }
            });
    connect(&m_radioModel, &RadioModel::backendRebuilt,
            m_clockEngine, &AetherClockEngine::stop);
    connect(m_clockEngine, &AetherClockEngine::runningChanged,
            this, [this, bindAudio](bool running) {
                // The engine is the slice-binding authority; mirror it into
                // the model so `get clock` reports the bound slice.
                m_clockModel->setSliceId(running ? m_clockEngine->boundSliceId()
                                                 : -1);
                bindAudio();
            });

    if (m_appletPanel) {
        if (auto* applet = m_appletPanel->aetherClockApplet()) {
            applet->attach(m_clockEngine, m_clockModel);
            // DAX chooser follows the radio's slice capacity (#4854 review).
            applet->setMaxDaxChannels(m_radioModel.maxSlices());
            connect(&m_radioModel, &RadioModel::infoChanged, applet,
                    [this, applet] { applet->setMaxDaxChannels(m_radioModel.maxSlices()); });
            connect(&m_radioModel, &RadioModel::connectionStateChanged, applet,
                    [this, applet](bool) { applet->setMaxDaxChannels(m_radioModel.maxSlices()); });
        }
    }
}

} // namespace AetherSDR
