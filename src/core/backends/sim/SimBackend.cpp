#include "core/backends/sim/SimBackend.h"

#include <QtEndian>
#include <QThread>

#include "core/RadioConnection.h"
#include "core/PanadapterStream.h"
#include "core/backends/sim/SimSignalSource.h"

namespace AetherSDR {

SimBackend::SimBackend(QObject* parent) : IRadioBackend(parent)
{
    // The synthetic RX engine, on its own worker (#4878). It was the only RX
    // producer in the app on the GUI thread — a 3 ms Qt::PreciseTimer pinning
    // the event dispatcher plus ~200 allocating emissions/s competing with
    // paintEvent, which on software-GL machines is the reported multi-second
    // input backlog. Signals cross back queued and are re-emitted over the
    // seam below, the same topology as FlexBackend's wire objects (#502).
    m_signalThread = new QThread(this);
    m_signalThread->setObjectName("SimSignalSource");
    m_signalSource = new SimSignalSource;   // no parent — moved to thread
    m_signalSource->moveToThread(m_signalThread);
    m_signalThread->start();

    connect(m_signalSource, &SimSignalSource::audioFrameReady,
            this, &IRadioBackend::audioFrameReady);
    connect(m_signalSource, &SimSignalSource::sliceAudioFrameReady,
            this, &IRadioBackend::sliceAudioFrameReady);
    connect(m_signalSource, &SimSignalSource::spectrumFrameReady,
            this, &IRadioBackend::spectrumFrameReady);

    // ---- Path B (RFC #4288): own a RadioConnection + PanadapterStream in
    // synthetic-demo mode, mirroring FlexBackend's ctor (same load-bearing #502
    // order: panStream thread FIRST, then connection). RadioModel harvests these
    // via connection()/panStream() and drives them exactly as it drives Flex's.
    // The synthetic wire behaviour lives in RadioConnection
    // (startSyntheticDemoConnect on a demo target) — no new wire logic here. The
    // PanadapterStream we vend stays deliberately IDLE: this backend produces the
    // demo's audio and spectrum itself, from the one NoiseMixer below, so a single
    // scene drives both what is heard and what is displayed.
    m_networkThread = new QThread(this);
    m_networkThread->setObjectName("SimPanadapterStream");
    m_panStream = new PanadapterStream;   // no parent — moved to thread
    m_panStream->moveToThread(m_networkThread);
    connect(m_networkThread, &QThread::started, m_panStream, &PanadapterStream::init);
    m_networkThread->start();

    m_connThread = new QThread(this);
    m_connThread->setObjectName("SimRadioConnection");
    m_connection = new RadioConnection;   // no parent — moved to thread
    m_connection->moveToThread(m_connThread);
    connect(m_connThread, &QThread::started, m_connection, &RadioConnection::init);
    m_connThread->start();

    // Re-emit wire lifecycle as the interface's own signals (as FlexBackend does).
    connect(m_connection, &RadioConnection::connected,
            this, &IRadioBackend::connected);
    connect(m_connection, &RadioConnection::disconnected,
            this, &IRadioBackend::disconnected);
    connect(m_connection, &RadioConnection::errorOccurred,
            this, &IRadioBackend::connectionError);

    // RFC #4288 (the VFO=0 fix): on the live hybrid path the synthetic wire
    // connection delivers Flex-format pan/slice status, but RadioModel decodes
    // that wire status ONLY through FlexBackend (decodeSliceStatus /
    // decodePanCenterBandwidth are m_flexBackend-gated), which is null in demo
    // mode — so the frequency never reaches the models and the VFO reads 0. We
    // bridge that by emitting the same information as NORMALIZED typed deltas
    // through the IRadioBackend seam (radioChanged/panCenterBandwidthChanged/
    // sliceChanged), which RadioModel wires for EVERY backend. Cross-thread
    // (m_connection is on its worker thread) so this is a queued connection.
    // Delayed 150ms so it lands AFTER RadioModel has created the SliceModel /
    // claimed the pan from the wire status (~50ms) — sliceChanged only applies to
    // an already-existing slice. m_connected gates onAudioTick(); set it here so
    // the audio/spectrum tick also starts producing on the live path.
    connect(m_connection, &RadioConnection::connected, this, [this]() {
        m_connected = true;
        QMetaObject::invokeMethod(m_signalSource, &SimSignalSource::start,
                                  Qt::QueuedConnection);
        QTimer::singleShot(150, this, [this]() {
            if (m_connected) emitInitialState();
        });
    });
    connect(m_connection, &RadioConnection::disconnected, this, [this]() {
        m_connected = false;
        QMetaObject::invokeMethod(m_signalSource, &SimSignalSource::stop,
                                  Qt::QueuedConnection);
    });
}

SimBackend::~SimBackend()
{
    // Mirror FlexBackend teardown: sever our lifecycle observation first, then
    // tear down connection (BlockingQueued disconnect → deleteLater → thread
    // quit/wait), then panStream — the #502 order.
    if (m_connection)
        disconnect(m_connection, nullptr, this, nullptr);

    // The producer goes first: stop the tick on its own thread, then let the
    // thread drain — the same BlockingQueued-stop shape as the two below.
    if (m_signalSource && m_signalThread && m_signalThread->isRunning()) {
        QMetaObject::invokeMethod(m_signalSource, &SimSignalSource::stop,
                                  Qt::BlockingQueuedConnection);
        m_signalSource->deleteLater();
        m_signalThread->quit();
        m_signalThread->wait(3000);
    } else {
        delete m_signalSource;
    }
    m_signalSource = nullptr;

    if (m_connection && m_connThread && m_connThread->isRunning()) {
        QMetaObject::invokeMethod(m_connection, &RadioConnection::disconnectFromRadio,
                                  Qt::BlockingQueuedConnection);
        m_connection->deleteLater();
        m_connThread->quit();
        m_connThread->wait(3000);
    } else {
        delete m_connection;
    }
    m_connection = nullptr;

    if (m_panStream && m_networkThread && m_networkThread->isRunning()) {
        QMetaObject::invokeMethod(m_panStream, &PanadapterStream::stop,
                                  Qt::BlockingQueuedConnection);
        m_panStream->deleteLater();
        m_networkThread->quit();
        m_networkThread->wait(3000);
    } else {
        delete m_panStream;
    }
    m_panStream = nullptr;
}

void SimBackend::setDemoNoiseEnabled(const QString& channel, bool on)
{
    QMetaObject::invokeMethod(m_signalSource, [src = m_signalSource, channel, on] {
        src->setNoiseEnabled(channel, on);
    }, Qt::QueuedConnection);
}

void SimBackend::setDemoNoiseLevel(const QString& channel, double levelDb)
{
    QMetaObject::invokeMethod(m_signalSource, [src = m_signalSource, channel, levelDb] {
        src->setNoiseLevel(channel, levelDb);
    }, Qt::QueuedConnection);
}

void SimBackend::setDemoNoiseKnob(const QString& channel, const QString& knob,
                                  double value)
{
    // The birdie "hz" re-anchor logic lives with the mixer in the source —
    // it needs the VFO and sideband atomically with the audio it changes.
    QMetaObject::invokeMethod(m_signalSource,
                              [src = m_signalSource, channel, knob, value] {
        src->setNoiseKnob(channel, knob, value);
    }, Qt::QueuedConnection);
}

void SimBackend::loadDemoNoisePreset(const QString& presetName)
{
    QMetaObject::invokeMethod(m_signalSource, [src = m_signalSource, presetName] {
        src->loadPreset(presetName);
    }, Qt::QueuedConnection);
}

void SimBackend::setDemoAnf(bool on)
{
    QMetaObject::invokeMethod(m_signalSource, [src = m_signalSource, on] {
        src->setAnf(on);
    }, Qt::QueuedConnection);
}

void SimBackend::setDemoNb(bool on)
{
    QMetaObject::invokeMethod(m_signalSource, [src = m_signalSource, on] {
        src->setNb(on);
    }, Qt::QueuedConnection);
}

QString SimBackend::demoModelName() { return QStringLiteral("AetherSDR Demo"); }
QString SimBackend::demoSerial()    { return QStringLiteral("DEMO-0001"); }
QString SimBackend::familyName()    { return QStringLiteral("sim"); }

RadioCapabilities SimBackend::capabilities() const
{
    RadioCapabilities caps;
    caps.family = familyName();
    caps.manufacturer = QStringLiteral("AetherSDR");
    caps.model  = demoModelName();
    caps.maxSlices = 1;          // Phase 1: a single slice. Phase 2 raises this.
    caps.maxPanadapters = 1;
    caps.sampleRatesHz = {};     // no data plane yet
    // A simulator must never look like something that can key a transmitter
    // (Principle VI). TX stays off in the skeleton.
    caps.canTransmit = false;
    caps.txPowerMaxWatts = 0.0;
    caps.hasTuner = false;
    caps.hasAmplifier = false;
    caps.hasExtendedDsp = false;
    caps.hasLmsNoiseFilters = false;
    caps.hasManualNotch = false;
    // Synthesised signals come out exactly where the demo says they are; there
    // is no oscillator to be wrong about.
    caps.hostFrequencyCalibration = false;
    // The simulator has no profile store to list, load or save into.
    caps.hasProfiles = false;
    caps.hasSelectableMicInputs = false;

    // The demo has no transmitter and no radio to ship audio to.
    caps.takesTxAudioOverSeam = false;
    // Continuous/unknown — the operator keeps their own width list.
    caps.rxFilterWidthsHz = {};
    // Synthetic audio only; nothing to route to a virtual device.
    caps.hasDaxStreams = false;
    caps.hasRadioSideDsp = false;        // synthetic scene; no firmware DSP
    // No radio-side display engine either — the demo's rows carry no black level.
    caps.hasRadioSideWaterfallAutoBlack = false;
    // No command plane at all: no radio-side CW text keyer, no voice keyer, and
    // a simulator that cannot key has nothing to be full duplex about.
    caps.hasRadioSideCwKeyer = false;
    caps.hasVoiceKeyer = false;
    caps.hasFullDuplex = false;
    caps.hasWaveforms = false;
    caps.hasMultiClientSessions = false;
    // No manual notch. The synthetic scene has an auto-notch in its mixer, but
    // nothing implements a placed, tracking null — so the +TNF button and the
    // panadapter's add-notch entries stay hidden here rather than appearing and
    // doing nothing.
    caps.maxNotchFilters = 0;
    caps.notchHasDepth = false;
    caps.notchMinWidthHz = 0.0;
    caps.notchMaxWidthHz = 0.0;
    caps.hasGpsLocation = false;         // synthetic radio has no position source
    caps.hasSupplyVoltageTelemetry = false;   // synthetic scene; no PA rail
    // The demo radio regenerates its synthetic scene on every connect; there
    // is no operating state worth resurrecting across sessions.
    caps.clientSettingsDomains = {};
    return caps;
}

void SimBackend::connectRadio(const RadioConnectRequest& /*request*/)
{
    if (m_connected) {
        return;   // idempotent — a second connect is a no-op, not a reset
    }
    m_connected = true;
    emit connected();
    emit capabilitiesChanged();
    emitInitialState();
    QMetaObject::invokeMethod(m_signalSource, &SimSignalSource::start,
                              Qt::QueuedConnection);   // synthetic RX begins
}

void SimBackend::disconnectRadio()
{
    if (!m_connected) {
        return;
    }
    QMetaObject::invokeMethod(m_signalSource, &SimSignalSource::stop,
                              Qt::QueuedConnection);
    m_connected = false;
    emit sliceRemoved(kSliceId);

    // Tear the synthetic connection down too, rather than only emitting our own
    // disconnected(). This is a Route A hybrid: RadioModel dials the demo through
    // the vended RadioConnection (see the m_connection branch of
    // connectToRadio()), so the connection is what the model treats as the live
    // link — leaving it "Connected" while the backend says otherwise is what made
    // isConnected() disagree with reality after `sim disconnect`.
    //
    // RadioConnection::disconnectFromRadio() has a synthetic branch that drops the
    // handle, sets Disconnected and emits disconnected() — which reaches
    // RadioModel::onDisconnected through the ONE wire-lifecycle path, and reaches
    // our own ctor lambda (which clears m_connected / stops the timers) and our
    // re-emit of IRadioBackend::disconnected for any seam listener. Queued: the
    // connection lives on its own thread.
    // Condition on whether the WIRE will report the disconnect, not merely on
    // whether a connection object exists. Only RadioConnection's synthetic branch
    // emits disconnected(); if we were never dialled through it (a bare
    // connectRadio() seam intent, as in the unit test), delegating would tear down
    // nothing and NOTHING would report the disconnect at all.
    if (m_connection && m_connection->isSyntheticDemo()) {
        QMetaObject::invokeMethod(m_connection,
                                  [conn = m_connection] { conn->disconnectFromRadio(); });
        // No emit here: that teardown comes back as RadioConnection::disconnected,
        // which our ctor re-emits as IRadioBackend::disconnected. Emitting now
        // would report the same disconnect twice on the seam.
    } else {
        emit disconnected();
    }
}

bool SimBackend::isConnected() const { return m_connected; }

void SimBackend::emitInitialState()
{
    // Radio-global snapshot: identity + free-slot count. Present-only fields.
    RadioDelta radio;
    radio.model = demoModelName();
    radio.nickname = QStringLiteral("Demo Simulator");
    radio.slicesAvailable = capabilities().maxSlices;
    emit radioChanged(radio);

    // Pan center/bandwidth via the typed seam. On the live (hybrid) path this is
    // what actually drives the VFO/display: RadioModel decodes real Flex pan wire
    // status only through FlexBackend (decodePanCenterBandwidth is m_flexBackend-
    // gated), which is null in demo mode — so the wire "display pan center=14.100"
    // the synthetic connection sends is delivered but never decoded. Emitting the
    // normalized panCenterBandwidthChanged here bridges that gap through the seam
    // RadioModel already wires for every backend. panId must match the wire pan id
    // ("0x40000000") RadioModel claimed. (RFC #4288 — the VFO=0 fix.)
    emit panCenterBandwidthChanged(QStringLiteral("0x40000000"),
                                   m_sliceFreqMhz, kDemoPanBandwidthMhz);

    // …and the waterfall rate, for exactly the same reason. The synthetic connect
    // declares it on a proper `display waterfall` status line, but RadioModel
    // decodes that through m_flexBackend->decodeWaterfallLineDuration(), which is
    // null in demo mode — so the value was parsed off the wire and dropped, and
    // PanadapterModel::waterfallLineDuration() stayed 0 while the neutral row
    // pacer fell back to its own default. panWaterfallLineDurationChanged is the
    // universal seam signal RadioModel wires for every backend, so emitting it
    // here closes the gap.
    emit panWaterfallLineDurationChanged(QStringLiteral("0x40000000"),
                                         kWaterfallRate);

    // One active slice on a sensible default, so the UI has something live to
    // show the moment demo mode connects. Same rationale as the pan above: the
    // wire "slice 0 RF_frequency=14.100" is delivered but decodeSliceStatus is
    // Flex-only, so without this typed emit the slice model never gets a
    // frequency and the VFO reads 0.
    SliceDelta slice;
    slice.letter = QStringLiteral("A");
    slice.panId = QStringLiteral("0x40000000");
    slice.frequency = m_sliceFreqMhz;
    slice.mode = m_sliceMode;
    slice.filterLow = m_filterLowHz;
    slice.filterHigh = m_filterHighHz;
    slice.active = true;
    slice.inUse = true;
    emit sliceChanged(kSliceId, slice);
}

void SimBackend::setSliceFrequency(int sliceId, double hz)
{
    if (!m_connected || sliceId != kSliceId) {
        return;
    }
    m_sliceFreqMhz = hz / 1.0e6;
    // Tuning must move the audio we actually hear — queued to the source.
    QMetaObject::invokeMethod(m_signalSource,
                              [src = m_signalSource, mhz = m_sliceFreqMhz] {
        src->setVfoMhz(mhz);
    }, Qt::QueuedConnection);
    SliceDelta d;
    d.frequency = m_sliceFreqMhz;
    emit sliceChanged(kSliceId, d);   // radio is authoritative: echo the change back (Principle II)
}

void SimBackend::setSliceMode(int sliceId, const QString& mode)
{
    if (!m_connected || sliceId != kSliceId) {
        return;
    }
    m_sliceMode = mode;
    // Lower-sideband family: LSB, DIGL, CWL. Everything else demodulates
    // USB-style. Drives which side of the VFO the birdie is audible on.
    const QString up = mode.toUpper();
    const bool lsb = (up == QLatin1String("LSB") || up == QLatin1String("DIGL")
                      || up == QLatin1String("CWL"));
    QMetaObject::invokeMethod(m_signalSource, [src = m_signalSource, lsb] {
        src->setLowerSideband(lsb);
    }, Qt::QueuedConnection);
    SliceDelta d;
    d.mode = m_sliceMode;
    emit sliceChanged(kSliceId, d);
}

void SimBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    if (!m_connected || sliceId != kSliceId) {
        return;
    }
    m_filterLowHz = lowHz;
    m_filterHighHz = highHz;
    SliceDelta d;
    d.filterLow = m_filterLowHz;
    d.filterHigh = m_filterHighHz;
    emit sliceChanged(kSliceId, d);
}

void SimBackend::setSliceAgc(int sliceId, const QString& mode, int thresholdDb)
{
    // The demo has no hardware AGC and no engine-side DSP chain to configure, so
    // there is nothing to translate the intent onto — but dropping it silently
    // would leave the UI showing a setting the "radio" never acknowledged. Record
    // it and echo it back, so the demo behaves like a radio that accepted the
    // change (Principle II: the radio is authoritative about its own state).
    if (!m_connected || sliceId != kSliceId) {
        return;
    }
    m_agcMode = mode;
    m_agcThresholdDb = thresholdDb;
    SliceDelta d;
    d.agcMode = m_agcMode;
    d.agcThreshold = m_agcThresholdDb;
    emit sliceChanged(kSliceId, d);
}

void SimBackend::setPanCenter(const QString& panId, double hz, PanCenterIntent)
{
    // The demo's panadapter centre is driven by its owned PanadapterStream (Route
    // A), which paints a fixed synthetic window around the slice, so there is no
    // DDC/NCO to move here. Accept the intent and re-assert the pan state so the
    // display and the model stay in agreement rather than drifting apart.
    Q_UNUSED(panId);
    if (!m_connected) {
        return;
    }
    emit panCenterBandwidthChanged(QStringLiteral("0x40000000"),
                                   hz / 1.0e6, kDemoPanBandwidthMhz);
}

void SimBackend::setKeying(bool key)
{
    // RX-only (capabilities().canTransmit == false): the engine TX guard above
    // the seam already denies keying. We DON'T transmit — but we do forward the
    // intent so the source mutes the synthetic RX while "keyed": the demo never
    // plays receive audio over a (would-be) transmit (Principle VI).
    QMetaObject::invokeMethod(m_signalSource, [src = m_signalSource, key] {
        src->setKeyed(key);
    }, Qt::QueuedConnection);
}

void SimBackend::invokeExtension(const QString& ns, const QString& verb,
                                 quint64 requestId, const QVariant& arg)
{
    // The demo's only vendor namespace is "sim" — the fault-injection harness
    // (RFC #4288 #4). Everything else is unknown. A requestId of 0 means "no reply
    // expected"; anything else gets a correlated result/error so a caller waiting
    // on a reply is never left hanging.
    if (ns != QLatin1String("sim")) {
        if (requestId != 0)
            emit extensionError(requestId,
                                QStringLiteral("sim backend: unknown namespace '%1'").arg(ns));
        return;
    }
    if (!m_connected) {
        if (requestId != 0)
            emit extensionError(requestId,
                                QStringLiteral("sim: not connected — connect the demo first"));
        return;
    }
    const bool handled = applyFault(verb, arg);
    if (requestId != 0) {
        if (handled)
            emit extensionResult(requestId,
                                 QVariantMap{{QStringLiteral("fault"), verb},
                                             {QStringLiteral("applied"), true}});
        else
            emit extensionError(requestId,
                                QStringLiteral("sim: unknown fault '%1'").arg(verb));
    }
}

bool SimBackend::applyFault(const QString& fault, const QVariant& arg)
{
    // Fault-injection harness (RFC #4288 #4). Each verb drives AE down a
    // fail-closed path so the regression suite can assert safe degradation.
    // RX-only is preserved — nothing here keys TX.
    const QString f = fault.trimmed().toLower();
    if (f == QLatin1String("swr")) {
        bool ok = false;
        const double ratio = arg.toDouble(&ok);
        injectHighSwr(ok && ratio > 0 ? ratio : 3.0);   // default a protective 3.0:1
        return true;
    }
    if (f == QLatin1String("dropslice")) {
        injectDropSlice();
        return true;
    }
    if (f == QLatin1String("stallscope")) {
        QMetaObject::invokeMethod(m_signalSource, [src = m_signalSource] {
            src->setScopeStalled(true);   // spectrum freezes; audio keeps flowing
        }, Qt::QueuedConnection);
        return true;
    }
    if (f == QLatin1String("disconnect")) {
        // Force a mid-operation disconnect to exercise AE's session-teardown /
        // reconnect path. Route through disconnectRadio() so state + timers unwind
        // exactly as a user-initiated disconnect would.
        disconnectRadio();
        return true;
    }
    if (f == QLatin1String("malformed")) {
        injectMalformedStatus();
        return true;
    }
    if (f == QLatin1String("clear")) {
        clearFaults();
        return true;
    }
    return false;   // unknown verb
}

MeterDef SimBackend::swrMeterDef()
{
    MeterDef def;
    def.index = kSwrMeterIndex;
    def.source = QStringLiteral("TX");
    def.name = QStringLiteral("SWR");
    def.unit = QStringLiteral("SWR");
    def.low = 1.0;
    def.high = 10.0;
    def.description = QStringLiteral("Demo fault-injected SWR");
    return def;
}

void SimBackend::injectHighSwr(double ratio)
{
    // Define an SWR meter and report a high reading, exercising the meter decode
    // and display path — the meaningful RX-safe slice of SWR handling (the demo is
    // RX-only, so no real TX foldback fires).
    //
    // Scope note: this shows up in MeterModel and in automation `get meters`, but
    // NOT in the HLTH applet. That applet only credits SWR while forward power
    // qualifies (>= 0.75 W) and forces 1.0 otherwise, and the demo defines no
    // FWDPWR meter — so do not read a quiet HLTH panel as this verb failing.
    const MeterDef def = swrMeterDef();
    emit meterDefined(def);
    // Meter values ride the data plane as raw quint16 (fixed-point ×256 on Flex).
    // MeterModel::updateValues(ids, vals) applies them; hand it our meter id with
    // the scaled reading so the HealthApplet sees a live high-SWR value.
    // ⚠ The meter id MUST be "SOURCE:NAME". RadioModel's meterUpdate handler
    // splits on the colon and returns early when there is none, so the bare
    // "SWR" this used to emit was silently dropped: `sim swr 3.5` returned
    // ok:true while nothing ever reached MeterModel or the HealthApplet — a
    // no-op that read as a working verb. Source and name here must match the
    // MeterDef above (source "TX", name "SWR").
    emit meterUpdate(def.source + QLatin1Char(':') + def.name, ratio);
    m_lastSwr = ratio;
}

void SimBackend::injectDropSlice()
{
    // Yank the active slice out from under AE — exercises slice-gone teardown
    // (VFO removal, TX-slice re-evaluation). Push it as the wire status AE decodes
    // ("slice 0 … removed"), NOT the interface sliceRemoved signal: RadioModel
    // consumes slice removal only through handleSliceStatus (the Flex backend
    // never emits IRadioBackend::sliceRemoved), so the wire path is the one that
    // actually reaches the teardown. A reconnect restores it via emitInitialState.
    // AE detects slice removal via in_use=0 (RadioModel.cpp onStatusReceived),
    // not a "removed" token — send the wire form AE actually acts on.
    pushFaultStatus(QStringLiteral(
        "SDE300001|slice 0 client_handle=0xDE300001 in_use=0"));
}

void SimBackend::injectMalformedStatus()
{
    // Push a deliberately garbled slice status so AE's decoder must fail closed
    // (present-only + ok-guarded carry — decodeSliceStatus drops malformed present
    // fields rather than applying them). A real radio never sends this; AE must
    // NOT retune/reset the VFO to 0 or crash. Assertion: VFO/slice unaffected.
    // RF_frequency is non-numeric garbage; the ok-guarded toDouble drops it.
    pushFaultStatus(QStringLiteral(
        "SDE300001|slice 0 client_handle=0xDE300001 "
        "RF_frequency=NOT_A_NUMBER mode=ZZ filter_lo=abc filter_hi=xyz"));
}

void SimBackend::pushFaultStatus(const QString& line)
{
    // Route a synthetic wire status line through the owned RadioConnection's
    // receive path (queued onto its worker thread), so AE decodes it exactly as a
    // radio-sent status. Used by the wire-format faults (dropslice, malformed).
    if (m_connection)
        QMetaObject::invokeMethod(m_connection, "injectFaultStatus",
                                  Qt::QueuedConnection, Q_ARG(QString, line));
}

void SimBackend::clearFaults()
{
    // Resume normal operation: un-stall the scope and drop SWR back to nominal.
    // (dropslice/disconnect are one-shot state changes recovered by reconnect,
    // not by clear — clear only reverses the *sustained* faults.)
    QMetaObject::invokeMethod(m_signalSource, [src = m_signalSource] {
        src->setScopeStalled(false);
    }, Qt::QueuedConnection);
    if (m_lastSwr > 1.0) {
        // Re-assert the DEFINITION before the value. A disconnect in between runs
        // RadioModel::onDisconnected -> MeterModel::clear(), which drops the def;
        // updateValueByName() then finds no such meter and the clear is silently
        // discarded. Defining is idempotent, so this is safe when it survived.
        emit meterDefined(swrMeterDef());
        // "SOURCE:NAME" — same requirement as injectHighSwr(); a bare "SWR" is
        // dropped by RadioModel's handler, so the meter would have stayed pinned
        // at the fault value even after a clear.
        emit meterUpdate(QStringLiteral("TX:SWR"), 1.1);   // nominal
        m_lastSwr = 1.1;
    }
}

}  // namespace AetherSDR
