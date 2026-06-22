#include "AutomationServer.h"
#include "LogManager.h"
#include "TxKeyingMarker.h"       // kTxKeyingProperty — authoritative TX-guard marker
#include "models/RadioModel.h"   // RadioModel, SliceModel, PanadapterModel (get())

#include <QLocalServer>
#include <QLocalSocket>
#include <QApplication>
#include <QWidget>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QImage>
#include <QPixmap>
#include <QBuffer>
#include <QPoint>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QTimer>
#include <QDateTime>
#include <QTime>

#include <limits>

// Best-effort value extraction for common control types.
#include <QAbstractButton>
#include <QAbstractSlider>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QSpinBox>
#include <QProgressBar>

#ifdef AETHER_GPU_SPECTRUM
#include <QRhiWidget>
#endif

namespace AetherSDR {

namespace {

// Human-meaningful "value" for a control, so an assertion can read state
// without a screenshot. Returns a null QString for widgets that have no
// natural scalar/text value (containers, custom-painted surfaces).
QString widgetValue(const QWidget* w)
{
    if (auto* s = qobject_cast<const QAbstractSlider*>(w))
        return QString::number(s->value());
    if (auto* b = qobject_cast<const QAbstractButton*>(w)) {
        if (b->isCheckable())
            return b->isChecked() ? QStringLiteral("checked")
                                  : QStringLiteral("unchecked");
        return b->text();
    }
    if (auto* cb = qobject_cast<const QComboBox*>(w))
        return cb->currentText();
    if (auto* le = qobject_cast<const QLineEdit*>(w)) {
        // Never serialize masked fields (password / PIN / keychain secrets):
        // dumpTree is written to a temp tree.json, so returning the cleartext
        // would exfiltrate credentials. Reporting a placeholder keeps the field
        // assertable (present / non-empty) without leaking the value. (#3646)
        if (le->echoMode() != QLineEdit::Normal)
            return le->text().isEmpty() ? QString() : QStringLiteral("<hidden>");
        return le->text();
    }
    if (auto* sb = qobject_cast<const QSpinBox*>(w))
        return QString::number(sb->value());
    if (auto* ds = qobject_cast<const QDoubleSpinBox*>(w))
        return QString::number(ds->value());
    if (auto* pb = qobject_cast<const QProgressBar*>(w))
        return QString::number(pb->value());
    if (auto* lb = qobject_cast<const QLabel*>(w))
        return lb->text();
    return QString();  // null -> omitted from snapshot
}

// Short class name without the AetherSDR:: (or any) namespace prefix.
QString shortClassName(const QObject* o)
{
    return QString::fromUtf8(o->metaObject()->className())
        .section(QStringLiteral("::"), -1);
}

QJsonObject describeWidget(const QWidget* w)
{
    QJsonObject o;
    o[QStringLiteral("class")] = QString::fromUtf8(w->metaObject()->className());
    if (!w->objectName().isEmpty())
        o[QStringLiteral("objectName")] = w->objectName();
    if (!w->accessibleName().isEmpty())
        o[QStringLiteral("accessibleName")] = w->accessibleName();
    o[QStringLiteral("enabled")] = w->isEnabled();
    o[QStringLiteral("visible")] = w->isVisible();

    // Geometry in global screen coordinates so a driver can correlate with
    // computer-use / screenshots if it ever needs to.
    const QPoint gp = w->mapToGlobal(QPoint(0, 0));
    QJsonObject geo;
    geo[QStringLiteral("x")] = gp.x();
    geo[QStringLiteral("y")] = gp.y();
    geo[QStringLiteral("w")] = w->width();
    geo[QStringLiteral("h")] = w->height();
    o[QStringLiteral("geometry")] = geo;

    const QString val = widgetValue(w);
    if (!val.isNull())
        o[QStringLiteral("value")] = val;

    // Range for numeric controls — lets a driver validate against the real
    // bounds (scale) and detect wrapping/circular sliders without guessing
    // extremes (#3646).
    if (auto* s = qobject_cast<const QAbstractSlider*>(w))
        o[QStringLiteral("range")] = QJsonObject{{QStringLiteral("min"), s->minimum()},
                                                 {QStringLiteral("max"), s->maximum()}};
    else if (auto* sb = qobject_cast<const QSpinBox*>(w))
        o[QStringLiteral("range")] = QJsonObject{{QStringLiteral("min"), sb->minimum()},
                                                 {QStringLiteral("max"), sb->maximum()}};
    else if (auto* ds = qobject_cast<const QDoubleSpinBox*>(w))
        o[QStringLiteral("range")] = QJsonObject{{QStringLiteral("min"), ds->minimum()},
                                                 {QStringLiteral("max"), ds->maximum()}};

    // Surface the TX-keying marker so an agent can see which controls invoke()
    // will refuse before trying them (#3646).
    if (w->property(kTxKeyingProperty).toBool())
        o[QStringLiteral("keying")] = true;

    QJsonArray kids;
    const QObjectList children = w->children();
    for (const QObject* child : children) {
        if (auto* cw = qobject_cast<const QWidget*>(child))
            kids.append(describeWidget(cw));
    }
    if (!kids.isEmpty())
        o[QStringLiteral("children")] = kids;

    return o;
}

// Depth-first match by class name (full or short) or accessibleName.
QWidget* matchRecursive(QWidget* w, const QString& target)
{
    const QString fullClass = QString::fromUtf8(w->metaObject()->className());
    if (fullClass == target
        || shortClassName(w) == target
        || w->accessibleName() == target) {
        return w;
    }
    const QObjectList children = w->children();
    for (QObject* child : children) {
        if (auto* cw = qobject_cast<QWidget*>(child)) {
            if (QWidget* m = matchRecursive(cw, target))
                return m;
        }
    }
    return nullptr;
}

// Last-resort match by a button's visible text — agents often know a control
// only by its label ("Send", "Transmit"). Lowest priority so an objectName /
// accessibleName / class always wins first.
QWidget* matchByButtonText(QWidget* w, const QString& target)
{
    if (auto* b = qobject_cast<QAbstractButton*>(w))
        if (b->text() == target)
            return w;
    const QObjectList children = w->children();
    for (QObject* child : children) {
        if (auto* cw = qobject_cast<QWidget*>(child)) {
            if (QWidget* m = matchByButtonText(cw, target))
                return m;
        }
    }
    return nullptr;
}

// Capture a widget to an image. The QRhi panadapter needs a framebuffer
// readback (QWidget::grab() would return empty/garbage for a GPU surface),
// so route QRhiWidget through its own grab().
QImage grabWidget(QWidget* w)
{
#ifdef AETHER_GPU_SPECTRUM
    // QRhiWidget inherits QWidget::grab() (which returns an empty pixmap for a
    // GPU surface); grabFramebuffer() is the real readback and returns a QImage.
    if (auto* rhi = qobject_cast<QRhiWidget*>(w))
        return rhi->grabFramebuffer();
#endif
    return w->grab().toImage();
}

QJsonObject err(const QString& msg)
{
    return QJsonObject{{QStringLiteral("ok"), false},
                       {QStringLiteral("error"), msg}};
}

// Parse a textual boolean from an invoke value: 1/true/on/yes/checked → true.
bool parseBool(const QString& v)
{
    const QString s = v.trimmed().toLower();
    return s == QLatin1String("1") || s == QLatin1String("true")
        || s == QLatin1String("on") || s == QLatin1String("yes")
        || s == QLatin1String("checked");
}

// TX-safety guard for invoke(): refuse to drive a control that keys the
// transmitter unless the operator sets AETHER_AUTOMATION_ALLOW_TX. A test bridge
// must never key a live radio by accident.
//
// Authoritative mechanism — a positive marker. Genuinely-keying controls
// (MOX/PTT, TUNE, ATU, CWX send, packet send) are tagged at their creation site
// with markTxKeying() (the "aetherTxKeying" dynamic property). The guard honors
// that property, so a control is blocked because it was *declared* keying, not
// because its label happened to contain a magic word. This closed the holes the
// old substring blocklist missed — notably the CW and packet "Send" buttons,
// which key TX but match no keyword (#3646 review).
//
// Belt-and-suspenders fallback — a button-scoped name heuristic, retained only
// to catch a keying control that predates or forgot the marker. It is *button*
// scoped because only a discrete button action can key (setpoint sliders like
// "Tune power"/"RF power" never transmit by being moved). When the fallback
// fires we log a warning: that control should get an explicit markTxKeying().
bool isTransmitControl(const QWidget* w)
{
    if (w->property(kTxKeyingProperty).toBool())
        return true;  // authoritative positive marker

    const auto* btn = qobject_cast<const QAbstractButton*>(w);
    if (!btn)
        return false;  // sliders / combos / spinboxes can't trigger TX

    static const QStringList kDeny = {
        QStringLiteral("mox"), QStringLiteral("ptt"), QStringLiteral("tune"),
        QStringLiteral("transmit"), QStringLiteral("vox"), QStringLiteral("cwx"),
        QStringLiteral("atu"),
    };
    const QStringList hay{w->objectName().toLower(),
                          w->accessibleName().toLower(),
                          btn->text().toLower()};
    for (const QString& h : hay) {
        for (const QString& d : kDeny) {
            if (h.contains(d)) {
                qCWarning(lcAutomation).noquote()
                    << "TX guard fell back to name match on" << btn->text()
                    << "— add markTxKeying() at its creation site if it keys TX";
                return true;
            }
        }
    }
    return false;
}

// ---- Model snapshots for get(). Hand-built from existing getters so we don't
// have to annotate every model field as a Q_PROPERTY; one call returns the full
// assertable state an agent needs. ----

QJsonObject sliceSnapshot(const SliceModel* s)
{
    return QJsonObject{
        {QStringLiteral("sliceId"),    s->sliceId()},
        {QStringLiteral("letter"),     s->letter()},
        {QStringLiteral("panId"),      s->panId()},
        {QStringLiteral("frequency"),  s->frequency()},   // MHz
        {QStringLiteral("mode"),       s->mode()},
        {QStringLiteral("filterLow"),  s->filterLow()},
        {QStringLiteral("filterHigh"), s->filterHigh()},
        {QStringLiteral("active"),     s->isActive()},
        {QStringLiteral("txSlice"),    s->isTxSlice()},
        {QStringLiteral("rxAntenna"),  s->rxAntenna()},
        {QStringLiteral("rfGain"),     s->rfGain()},
        {QStringLiteral("audioGain"),  s->audioGain()},
        {QStringLiteral("audioPan"),   s->audioPan()},
        {QStringLiteral("locked"),     s->isLocked()},
        {QStringLiteral("nb"),         s->nbOn()},
        {QStringLiteral("nbLevel"),    s->nbLevel()},
        {QStringLiteral("nr"),         s->nrOn()},
        {QStringLiteral("nrLevel"),    s->nrLevel()},
        {QStringLiteral("anf"),        s->anfOn()},
        {QStringLiteral("apf"),        s->apfOn()},
        {QStringLiteral("apfLevel"),   s->apfLevel()},
        {QStringLiteral("squelch"),    s->squelchOn()},
        {QStringLiteral("squelchLevel"), s->squelchLevel()},
        {QStringLiteral("agcMode"),    s->agcMode()},
        {QStringLiteral("agcThreshold"), s->agcThreshold()},
    };
}

QJsonObject panSnapshot(const PanadapterModel* p)
{
    return QJsonObject{
        {QStringLiteral("panId"),        p->panId()},
        {QStringLiteral("centerMhz"),    p->centerMhz()},
        {QStringLiteral("bandwidthMhz"), p->bandwidthMhz()},
        {QStringLiteral("minDbm"),       p->minDbm()},
        {QStringLiteral("maxDbm"),       p->maxDbm()},
        {QStringLiteral("rxAntenna"),    p->rxAntenna()},
        {QStringLiteral("rfGain"),       p->rfGain()},
        {QStringLiteral("wide"),         p->wideActive()},
        {QStringLiteral("fps"),          p->fps()},
    };
}

QJsonObject radioSnapshot(const RadioModel* r)
{
    // Multi-Flex slot occupancy across the radio's whole slice capacity: each
    // slot is ours / foreign (another client, e.g. a Maestro) / empty. This is
    // why an `slice add` can be refused even when we hold only one slice — the
    // capacity is radio-wide, shared across clients. (#3646)
    QJsonArray slotArr;   // not "slots" — that's a Qt macro
    const int maxSlices = r->maxSlices();
    for (int id = 0; id < maxSlices; ++id) {
        QString state = QStringLiteral("empty");
        if (r->isSlotOurs(id))         state = QStringLiteral("ours");
        else if (r->isSlotForeign(id)) state = QStringLiteral("foreign");
        QJsonObject slot{{QStringLiteral("id"), id}, {QStringLiteral("state"), state}};
        if (state == QLatin1String("foreign"))
            slot[QStringLiteral("owner")] = r->foreignSliceOwnerStation(id);
        slotArr.append(slot);
    }

    return QJsonObject{
        {QStringLiteral("name"),         r->name()},
        {QStringLiteral("model"),        r->model()},
        {QStringLiteral("version"),      r->version()},
        {QStringLiteral("serial"),       r->serial()},
        {QStringLiteral("callsign"),     r->callsign()},
        {QStringLiteral("nickname"),     r->nickname()},
        {QStringLiteral("connected"),    r->isConnected()},
        {QStringLiteral("transmitting"), r->isRadioTransmitting()},
        {QStringLiteral("txPower"),      r->txPower()},
        {QStringLiteral("paTemp"),       r->paTemp()},
        {QStringLiteral("sliceCount"),   r->slices().size()},
        {QStringLiteral("maxSlices"),    maxSlices},
        {QStringLiteral("slots"),        slotArr},
        {QStringLiteral("panCount"),     r->panadapters().size()},
    };
}

// TX-chain state (TransmitModel) — RF power, mic/processor, VOX/AM/DEXP, CW, ATU
// and APD. Lets a QA scenario assert that a TX/Phone/CW applet control actually
// reached the radio model, not just the widget (#3646 QA finding 2). Read-only:
// keying state (mox/tune/transmitting) is reported but never driven from here.
QString atuStatusName(ATUStatus s)
{
    switch (s) {
    case ATUStatus::None:         return QStringLiteral("none");
    case ATUStatus::NotStarted:   return QStringLiteral("not_started");
    case ATUStatus::InProgress:   return QStringLiteral("in_progress");
    case ATUStatus::Bypass:       return QStringLiteral("bypass");
    case ATUStatus::Successful:   return QStringLiteral("successful");
    case ATUStatus::OK:           return QStringLiteral("ok");
    case ATUStatus::FailBypass:   return QStringLiteral("fail_bypass");
    case ATUStatus::Fail:         return QStringLiteral("fail");
    case ATUStatus::Aborted:      return QStringLiteral("aborted");
    case ATUStatus::ManualBypass: return QStringLiteral("manual_bypass");
    }
    return QStringLiteral("unknown");
}

QJsonObject transmitSnapshot(const TransmitModel* t)
{
    return QJsonObject{
        // power / keying (read-only)
        {QStringLiteral("rfPower"),         t->rfPower()},
        {QStringLiteral("tunePower"),       t->tunePower()},
        {QStringLiteral("tuning"),          t->isTuning()},
        {QStringLiteral("mox"),             t->isMox()},
        {QStringLiteral("transmitting"),    t->isTransmitting()},
        {QStringLiteral("maxPowerLevel"),   t->maxPowerLevel()},
        {QStringLiteral("activeProfile"),   t->activeProfile()},
        // mic / monitor / processor
        {QStringLiteral("micSelection"),    t->micSelection()},
        {QStringLiteral("micLevel"),        t->micLevel()},
        {QStringLiteral("micAcc"),          t->micAcc()},
        {QStringLiteral("speechProc"),      t->speechProcessorEnable()},
        {QStringLiteral("speechProcLevel"), t->speechProcessorLevel()},
        {QStringLiteral("dax"),             t->daxOn()},
        {QStringLiteral("monitor"),         t->sbMonitor()},
        {QStringLiteral("monGainSb"),       t->monGainSb()},
        {QStringLiteral("activeMicProfile"),t->activeMicProfile()},
        // VOX / AM / DEXP / TX filter
        {QStringLiteral("voxEnable"),       t->voxEnable()},
        {QStringLiteral("voxLevel"),        t->voxLevel()},
        {QStringLiteral("voxDelay"),        t->voxDelay()},
        {QStringLiteral("amCarrierLevel"),  t->amCarrierLevel()},
        {QStringLiteral("dexp"),            t->dexpOn()},
        {QStringLiteral("dexpLevel"),       t->dexpLevel()},
        {QStringLiteral("txFilterLow"),     t->txFilterLow()},
        {QStringLiteral("txFilterHigh"),    t->txFilterHigh()},
        // CW
        {QStringLiteral("cwSpeed"),         t->cwSpeed()},
        {QStringLiteral("cwPitch"),         t->cwPitch()},
        {QStringLiteral("cwBreakIn"),       t->cwBreakIn()},
        {QStringLiteral("cwDelay"),         t->cwDelay()},
        {QStringLiteral("cwSidetone"),      t->cwSidetone()},
        {QStringLiteral("cwIambic"),        t->cwIambic()},
        {QStringLiteral("monGainCw"),       t->monGainCw()},
        {QStringLiteral("monPanCw"),        t->monPanCw()},
        // ATU / APD
        {QStringLiteral("atuEnabled"),      t->atuEnabled()},
        {QStringLiteral("atuMemories"),     t->memoriesEnabled()},
        {QStringLiteral("atuStatus"),       atuStatusName(t->atuStatus())},
        {QStringLiteral("apdEnabled"),      t->apdEnabled()},
    };
}

// 8-band graphic EQ (RX + TX). Lets a scenario assert EQ-applet slider changes
// reached the model (#3646). Bands keyed by their short labels (63 … 8k).
QJsonObject equalizerSnapshot(const EqualizerModel* e)
{
    QJsonObject rx, tx;
    for (int i = 0; i < EqualizerModel::BandCount; ++i) {
        const auto band = static_cast<EqualizerModel::Band>(i);
        const QString key = EqualizerModel::bandLabel(band);
        rx[key] = e->rxBand(band);
        tx[key] = e->txBand(band);
    }
    return QJsonObject{
        {QStringLiteral("rxEnabled"), e->rxEnabled()},
        {QStringLiteral("txEnabled"), e->txEnabled()},
        {QStringLiteral("rx"), rx},
        {QStringLiteral("tx"), tx},
    };
}

// Annotate meters known to be unreliable on the connected radio, mirroring the
// curation the UI already does, so a consumer of the raw `all` table doesn't
// trust a bad reading. Today this is exactly one entry: PACURRENT on the
// FLEX-8000 series, where the declared 10 A meter range is below real PA draw so
// it clips (SMART-11281) — the GUI omits it (see MeterApplet.h), and freshness
// (age_ms) can't catch a fresh-but-clipped value. Keep this list in sync with
// the UI; it is intentionally a small explicit table, not a heuristic. (#3729)
QString unreliableMeterNote(const QString& meterName, const QString& radioModel)
{
    if (meterName == QLatin1String("PACURRENT")
        && radioModel.startsWith(QStringLiteral("FLEX-8"))) {
        return QStringLiteral("clips at the declared 10A cap on FLEX-8000 series "
                              "(SMART-11281); omitted from the UI — do not trust");
    }
    return QString();
}

// Live meter readout. The flat convenience fields are the headline TX meters
// with their freshness age (ms since last update, -1 if never) so a reader can
// reject stale values — critical because some meters (notably PACURRENT) are
// only reported ~1 s into a transmit. `all` carries every defined meter with
// per-meter index/source_index/age_ms so duplicate-named meters (one live, one
// floored) are distinguishable, plus a `reliable:false`+`note` flag on meters
// known-bad for the connected radio. (#3646, #3729)
QJsonObject metersSnapshot(MeterModel* m, const QString& radioModel)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto age = [now](qint64 ts) -> qint64 { return ts > 0 ? now - ts : -1; };

    QJsonArray all = m->allMeters();
    for (int i = 0; i < all.size(); ++i) {
        QJsonObject meter = all[i].toObject();
        const QString note = unreliableMeterNote(meter.value(QStringLiteral("name")).toString(),
                                                 radioModel);
        if (!note.isEmpty()) {
            meter[QStringLiteral("reliable")] = false;
            meter[QStringLiteral("note")]     = note;
            all[i] = meter;
        }
    }

    return QJsonObject{
        {QStringLiteral("fwdPower"),        m->fwdPower()},           // Watts (smoothed)
        {QStringLiteral("fwdPowerInstant"), m->fwdPowerInstant()},    // Watts (peak)
        {QStringLiteral("fwdPowerAgeMs"),   age(m->fwdPowerUpdatedAtMs())},
        {QStringLiteral("swr"),             m->swr()},
        {QStringLiteral("swrAgeMs"),        age(m->swrUpdatedAtMs())},
        {QStringLiteral("paTemp"),          m->paTemp()},             // °C
        {QStringLiteral("supplyVolts"),     m->supplyVolts()},        // V
        {QStringLiteral("swAlc"),           m->swAlc()},              // dBFS post-ALC SSB peak
        {QStringLiteral("hwAlc"),           m->hwAlc()},              // dBFS external HW-ALC
        {QStringLiteral("micPeak"),         m->micPeak()},            // dBFS
        {QStringLiteral("micLevel"),        m->micLevel()},           // dBFS
        {QStringLiteral("compPeak"),        m->compPeak()},           // dB compression (peak)
        {QStringLiteral("compLevel"),       m->compLevel()},          // dB compression
        {QStringLiteral("hasCompression"),  m->hasCompressionMeterValue()},
        {QStringLiteral("sLevel"),          m->sLevel()},             // dBm
        {QStringLiteral("txMetersFresh"),   m->hasRecentTxMeters(2000)},
        {QStringLiteral("txMetersAgeMs"),   age(m->txMetersUpdatedAtMs())},
        {QStringLiteral("all"),             all},                     // every meter + age_ms + reliability
    };
}

} // namespace

AutomationServer::AutomationServer(QObject* parent)
    : QObject(parent)
{
}

AutomationServer::~AutomationServer()
{
    stop();
}

bool AutomationServer::start(const QString& serverName)
{
    if (m_server)
        return true;

    m_serverName = serverName;
    m_server = new QLocalServer(this);

    // Restrict the socket to the owning user (0600 / per-user pipe ACL). The
    // endpoint can key TX and its path is advertised in a shared-temp discovery
    // file, so another local user must not be able to connect and drive the GUI.
    m_server->setSocketOptions(QLocalServer::UserAccessOption);

    // Clear any stale socket left by a crashed run so we can rebind.
    QLocalServer::removeServer(serverName);

    if (!m_server->listen(serverName)) {
        qCWarning(lcAutomation) << "failed to listen on" << serverName << ':'
                                << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }

    connect(m_server, &QLocalServer::newConnection,
            this, &AutomationServer::onNewConnection);

    // Drop a discovery file so a driver can find the resolved endpoint without
    // knowing the platform-specific socket path. Best-effort; not fatal.
    m_discoveryFile = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                          .filePath(QStringLiteral("aethersdr-automation.json"));
    QJsonObject disc;
    disc[QStringLiteral("socket")]  = fullServerName();
    disc[QStringLiteral("name")]    = serverName;
    disc[QStringLiteral("pid")]     = QCoreApplication::applicationPid();
    disc[QStringLiteral("version")] = QCoreApplication::applicationVersion();
    QFile df(m_discoveryFile);
    if (df.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        df.write(QJsonDocument(disc).toJson(QJsonDocument::Compact));
        df.close();
    } else {
        m_discoveryFile.clear();
    }

    // TX safety rails (#3646): when the operator has enabled transmit
    // automation, arm a watchdog that force-unkeys the radio if it stays keyed
    // past a limit — a backstop independent of whatever script is driving us.
    m_txAllowed = qEnvironmentVariableIsSet("AETHER_AUTOMATION_ALLOW_TX");
    if (m_txAllowed) {
        if (qEnvironmentVariableIsSet("AETHER_AUTOMATION_TX_MAX_MS"))
            m_txMaxKeyMs = qEnvironmentVariableIntValue("AETHER_AUTOMATION_TX_MAX_MS");
        if (qEnvironmentVariableIsSet("AETHER_AUTOMATION_TX_MAX_POWER"))
            m_txMaxPower = qEnvironmentVariableIntValue("AETHER_AUTOMATION_TX_MAX_POWER");
        m_txWatchdog = new QTimer(this);
        m_txWatchdog->setInterval(500);
        connect(m_txWatchdog, &QTimer::timeout, this, &AutomationServer::onTxWatchdog);
        m_txWatchdog->start();
        qCInfo(lcAutomation).noquote()
            << "TX automation ENABLED — watchdog max key" << m_txMaxKeyMs << "ms,"
            << "power ceiling" << (m_txMaxPower < 0 ? QStringLiteral("none")
                                                    : QString::number(m_txMaxPower));
    }

    // Observability suite (#3646): start the monotonic clock, tap the log
    // funnel into a ring buffer, and arm the (idle) drain timer used by
    // `log subscribe`. The tap runs on arbitrary logging threads, so it only
    // touches the mutex-guarded ring — never a socket.
    m_monoClock.start();
    m_logTapId = LogManager::instance().addTap(
        [this](QtMsgType type, const QString& cat, const QString& msg) {
            QMutexLocker lk(&m_logMutex);
            LogEvent e;
            e.seq    = ++m_logSeq;
            e.monoUs = m_monoClock.nsecsElapsed() / 1000;
            e.wall   = QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"));
            e.type   = static_cast<int>(type);
            e.cat    = cat;
            e.msg    = msg;
            m_logRing.push_back(std::move(e));
            while (m_logRing.size() > static_cast<size_t>(kLogRingMax))
                m_logRing.pop_front();
        });
    m_logDrain = new QTimer(this);
    m_logDrain->setInterval(50);
    connect(m_logDrain, &QTimer::timeout, this, &AutomationServer::onLogDrain);

    qCInfo(lcAutomation).noquote()
        << "automation bridge listening on" << fullServerName()
        << "(verbs: ping, dumpTree, grab, invoke, get, txtest, atu, slice, tune, log, mark)";
    return true;
}

void AutomationServer::stop()
{
    if (!m_server)
        return;

    // Safety: never leave the radio keyed when the bridge shuts down.
    if (m_txAllowed)
        forceUnkey("automation bridge stopping");
    if (m_txWatchdog) {
        m_txWatchdog->stop();
        m_txWatchdog->deleteLater();
        m_txWatchdog = nullptr;
    }
    // Detach the log tap before teardown so no logging thread can touch us.
    if (m_logTapId >= 0) {
        LogManager::instance().removeTap(m_logTapId);
        m_logTapId = -1;
    }
    if (m_logDrain) {
        m_logDrain->stop();
        m_logDrain->deleteLater();
        m_logDrain = nullptr;
    }
    m_logSubscribers.clear();

    for (auto it = m_buffers.constBegin(); it != m_buffers.constEnd(); ++it)
        it.key()->deleteLater();
    m_buffers.clear();

    m_server->close();
    m_server->deleteLater();
    m_server = nullptr;

    if (!m_discoveryFile.isEmpty()) {
        QFile::remove(m_discoveryFile);
        m_discoveryFile.clear();
    }
}

bool AutomationServer::isRunning() const
{
    return m_server && m_server->isListening();
}

QString AutomationServer::fullServerName() const
{
    return m_server ? m_server->fullServerName() : QString();
}

void AutomationServer::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QLocalSocket* sock = m_server->nextPendingConnection();
        m_buffers.insert(sock, QByteArray());
        connect(sock, &QLocalSocket::readyRead,
                this, &AutomationServer::onReadyRead);
        connect(sock, &QLocalSocket::disconnected,
                this, &AutomationServer::onDisconnected);
        qCDebug(lcAutomation) << "client connected;" << m_buffers.size() << "active";
    }
}

void AutomationServer::onReadyRead()
{
    auto* sock = qobject_cast<QLocalSocket*>(sender());
    if (!sock || !m_buffers.contains(sock))
        return;

    QByteArray& buf = m_buffers[sock];
    buf.append(sock->readAll());

    int nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
        const QByteArray line = buf.left(nl);
        buf.remove(0, nl + 1);
        if (line.trimmed().isEmpty())
            continue;
        const QJsonObject resp = handleLine(line, sock);
        sock->write(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        sock->write("\n");
        sock->flush();
    }
}

void AutomationServer::onDisconnected()
{
    auto* sock = qobject_cast<QLocalSocket*>(sender());
    if (!sock)
        return;
    m_buffers.remove(sock);
    if (m_logSubscribers.remove(sock) && m_logSubscribers.isEmpty() && m_logDrain)
        m_logDrain->stop();
    sock->deleteLater();
    qCDebug(lcAutomation) << "client disconnected;" << m_buffers.size() << "active";
}

QJsonObject AutomationServer::handleLine(const QByteArray& line, QLocalSocket* sock)
{
    QString cmd, target, path, action, value, model, selector, property;

    const QByteArray trimmed = line.trimmed();
    if (trimmed.startsWith('{')) {
        // JSON request, e.g.
        //   {"cmd":"invoke","target":"masterVolume","action":"setValue","value":"30"}
        //   {"cmd":"get","model":"slice","selector":"active","property":"frequency"}
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject())
            return err(QStringLiteral("invalid JSON: ") + perr.errorString());
        const QJsonObject obj = doc.object();
        cmd      = obj.value(QStringLiteral("cmd")).toString();
        target   = obj.value(QStringLiteral("target")).toString();
        path     = obj.value(QStringLiteral("path")).toString();
        action   = obj.value(QStringLiteral("action")).toString();
        // value may be a string, number, or bool — normalize to text.
        const QJsonValue v = obj.value(QStringLiteral("value"));
        if (v.isString())      value = v.toString();
        else if (v.isDouble()) value = QString::number(v.toDouble());
        else if (v.isBool())   value = v.toBool() ? QStringLiteral("true")
                                                  : QStringLiteral("false");
        model    = obj.value(QStringLiteral("model")).toString();
        selector = obj.value(QStringLiteral("selector")).toString();
        property = obj.value(QStringLiteral("property")).toString();
    } else {
        // Bare line. Positional by verb:
        //   grab   <target> [path]
        //   invoke <target> <action> [value...]   (value joins the rest)
        //   get    <model>  [selector] [property]
        const QList<QByteArray> p = trimmed.split(' ');
        auto tok = [&p](int i) { return QString::fromUtf8(p.value(i)); };
        cmd = tok(0);
        if (cmd == QLatin1String("invoke")) {
            target = tok(1);
            action = tok(2);
            QStringList rest;
            for (int i = 3; i < p.size(); ++i) rest << tok(i);
            value = rest.join(QLatin1Char(' '));
        } else if (cmd == QLatin1String("get")) {
            model = tok(1); selector = tok(2); property = tok(3);
        } else if (cmd == QLatin1String("txtest") || cmd == QLatin1String("atu")) {
            action = tok(1);  // e.g. "txtest twotone", "atu bypass"
        } else if (cmd == QLatin1String("slice")) {
            action = tok(1); value = tok(2);  // "slice add 14.2", "slice remove 1"
        } else if (cmd == QLatin1String("tune")) {
            value = tok(1);   // "tune 3.7"
        } else if (cmd == QLatin1String("log")) {
            action = tok(1);  // categories|get|set|reset|tail|subscribe|unsubscribe
            QStringList rest;
            for (int i = 2; i < p.size(); ++i) rest << tok(i);
            value = rest.join(QLatin1Char(' '));  // "aether.protocol on", "200 since=42"
        } else if (cmd == QLatin1String("mark")) {
            QStringList rest;
            for (int i = 1; i < p.size(); ++i) rest << tok(i);
            value = rest.join(QLatin1Char(' '));  // free-form annotation text
        } else {  // grab and friends
            target = tok(1); path = tok(2);
        }
    }

    qCDebug(lcAutomation) << "request:" << cmd << target << action << model << selector;

    if (cmd == QLatin1String("ping")) {
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("app"), QStringLiteral("AetherSDR")},
            {QStringLiteral("version"), QCoreApplication::applicationVersion()},
        };
    }
    if (cmd == QLatin1String("dumpTree"))
        return doDumpTree();
    if (cmd == QLatin1String("grab")) {
        if (target.isEmpty())
            return err(QStringLiteral("grab requires a target widget"));
        return doGrab(target, path);
    }
    if (cmd == QLatin1String("invoke")) {
        if (target.isEmpty() || action.isEmpty())
            return err(QStringLiteral("invoke requires a target and an action"));
        return doInvoke(target, action, value);
    }
    if (cmd == QLatin1String("get")) {
        if (model.isEmpty())
            return err(QStringLiteral("get requires a model (radio|transmit|meters|slice|slices|pan|pans)"));
        return doGet(model, selector, property);
    }
    if (cmd == QLatin1String("txtest")) {
        if (action.isEmpty())
            return err(QStringLiteral("txtest requires an action (twotone|off)"));
        return doTxTest(action);
    }
    if (cmd == QLatin1String("atu")) {
        if (action.isEmpty())
            return err(QStringLiteral("atu requires an action (bypass|start)"));
        return doAtu(action);
    }
    if (cmd == QLatin1String("slice")) {
        if (action.isEmpty())
            return err(QStringLiteral("slice requires an action (add|remove|select)"));
        return doSlice(action, value);
    }
    if (cmd == QLatin1String("tune")) {
        if (value.isEmpty())
            return err(QStringLiteral("tune requires a frequency in MHz"));
        return doTune(value);
    }
    if (cmd == QLatin1String("log"))
        return doLog(action.isEmpty() ? QStringLiteral("categories") : action, value, sock);
    if (cmd == QLatin1String("mark")) {
        if (value.isEmpty())
            return err(QStringLiteral("mark requires annotation text"));
        return doMark(value);
    }

    return err(QStringLiteral("unknown command: ") + cmd);
}

QJsonObject AutomationServer::doDumpTree() const
{
    QJsonArray roots;
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget* w : tops) {
        // Skip the transient/internal helper windows Qt creates so the
        // snapshot stays focused on real UI.
        if (w->objectName() == QLatin1String("qt_scrollarea_viewport"))
            continue;
        roots.append(describeWidget(w));
    }
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("roots"), roots},
    };
}

QJsonObject AutomationServer::doGrab(const QString& target, const QString& path) const
{
    QWidget* w = resolveWidget(target);
    if (!w) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("widget not found: ") + target}};
    }

    const QImage img = grabWidget(w);
    if (img.isNull()) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("grab produced an empty image for ") + target}};
    }

    QString outPath = path;
    if (outPath.isEmpty()) {
        QString safe = target;
        safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")),
                     QStringLiteral("_"));
        outPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                      .filePath(QStringLiteral("aether-grab-") + safe + QStringLiteral(".png"));
    }

    if (!img.save(outPath, "PNG")) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("failed to write PNG: ") + outPath}};
    }

    const qint64 bytes = QFileInfo(outPath).size();
    qCInfo(lcAutomation).noquote()
        << "grabbed" << shortClassName(w) << "->" << outPath
        << QStringLiteral("(%1x%2, %3 bytes)").arg(img.width()).arg(img.height()).arg(bytes);

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("target"), target},
        {QStringLiteral("class"), shortClassName(w)},
        {QStringLiteral("path"), outPath},
        {QStringLiteral("width"), img.width()},
        {QStringLiteral("height"), img.height()},
        {QStringLiteral("bytes"), bytes},
    };
}

QWidget* AutomationServer::resolveWidget(const QString& target)
{
    const QWidgetList tops = QApplication::topLevelWidgets();

    // 0. Scoped target "<scope>/<name>" disambiguates duplicate accessibleNames
    //    across applets — e.g. "RxApplet/AF gain" vs "PanadapterApplet/AF gain",
    //    which both exist and would otherwise both resolve to whichever comes
    //    first in tree order. <scope> matches an ancestor by objectName, class
    //    (short or full), or accessibleName; <name> is resolved within that
    //    subtree. Falls through to flat resolution if it doesn't resolve, so a
    //    literal '/' in a control name still works.
    const int slash = target.indexOf(QLatin1Char('/'));
    if (slash > 0) {
        const QString scope = target.left(slash);
        const QString inner = target.mid(slash + 1);
        for (QWidget* tlw : tops) {
            QWidget* sc = (tlw->objectName() == scope) ? tlw
                                                       : tlw->findChild<QWidget*>(scope);
            if (!sc)
                sc = matchRecursive(tlw, scope);
            if (sc) {
                if (QWidget* m = matchRecursive(sc, inner)) return m;
                if (QWidget* m = matchByButtonText(sc, inner)) return m;
            }
        }
    }

    // 1. Exact objectName (cheap, unambiguous).
    for (QWidget* tlw : tops) {
        if (tlw->objectName() == target)
            return tlw;
        if (QWidget* c = tlw->findChild<QWidget*>(target))
            return c;
    }
    // 2. Class name or accessibleName (e.g. "SpectrumWidget" for the panadapter).
    for (QWidget* tlw : tops) {
        if (QWidget* m = matchRecursive(tlw, target))
            return m;
    }
    // 3. Button visible text, last resort (e.g. "Send", "Transmit").
    for (QWidget* tlw : tops) {
        if (QWidget* m = matchByButtonText(tlw, target))
            return m;
    }
    return nullptr;
}

QJsonObject AutomationServer::doInvoke(const QString& target, const QString& action,
                                       const QString& value) const
{
    QWidget* w = resolveWidget(target);
    if (!w)
        return err(QStringLiteral("widget not found: ") + target);

    // Refuse to drive a disabled control. Qt's setValue()/setChecked() still
    // mutate a disabled widget, so without this the bridge would report a happy
    // newValue while the radio never sees the change (the control is greyed out
    // for a reason — wrong mode, not connected, etc.). Surfacing it as an error
    // turns a silent no-op into an explicit, assertable signal. (#3646)
    if (!w->isEnabled()) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("refused: '") + target
                                + QStringLiteral("' is disabled — the radio won't accept the change")},
                           {QStringLiteral("disabled"), true},
                           {QStringLiteral("class"), shortClassName(w)}};
    }

    // TX-safety guard — never key a live radio from the test bridge unless the
    // operator has explicitly opted in. (#3646 Phase 1 safety requirement.)
    if (isTransmitControl(w)
        && !qEnvironmentVariableIsSet("AETHER_AUTOMATION_ALLOW_TX")) {
        qCWarning(lcAutomation).noquote()
            << "BLOCKED transmit-related invoke on" << target
            << "(" << shortClassName(w) << ")";
        return err(QStringLiteral("blocked: '") + target
                   + QStringLiteral("' is a transmit-keying control (TX-safety guard). "
                                    "Set AETHER_AUTOMATION_ALLOW_TX=1 to override."));
    }

    // Power-ceiling rail (#3646): clamp RF/Tune power setpoints to the
    // configured max (AETHER_AUTOMATION_TX_MAX_POWER) so automation can't
    // command more power than the connected load can take.
    QString effValue = value;
    if (m_txMaxPower >= 0 && action == QLatin1String("setValue")) {
        const QString an = w->accessibleName();
        if (an == QLatin1String("RF power") || an == QLatin1String("Tune power")) {
            bool okN = false;
            const int n = value.toInt(&okN);
            if (okN && n > m_txMaxPower) {
                effValue = QString::number(m_txMaxPower);
                qCWarning(lcAutomation).noquote()
                    << "power ceiling: clamped" << an << n << "->" << m_txMaxPower;
            }
        }
    }

    bool done = false;
    if (action == QLatin1String("click")) {
        if (auto* b = qobject_cast<QAbstractButton*>(w)) { b->click(); done = true; }
    } else if (action == QLatin1String("toggle")) {
        if (auto* b = qobject_cast<QAbstractButton*>(w)) {
            b->isCheckable() ? b->toggle() : b->click();
            done = true;
        }
    } else if (action == QLatin1String("setChecked")) {
        if (auto* b = qobject_cast<QAbstractButton*>(w); b && b->isCheckable()) {
            b->setChecked(parseBool(value)); done = true;
        }
    } else if (action == QLatin1String("setValue")) {
        bool okNum = false;
        const int n = effValue.toInt(&okNum);
        if (auto* s = qobject_cast<QAbstractSlider*>(w)) {
            if (!okNum) return err(QStringLiteral("setValue needs an integer"));
            s->setValue(n); done = true;
        } else if (auto* sb = qobject_cast<QSpinBox*>(w)) {
            if (!okNum) return err(QStringLiteral("setValue needs an integer"));
            sb->setValue(n); done = true;
        } else if (auto* ds = qobject_cast<QDoubleSpinBox*>(w)) {
            bool okD = false; const double d = effValue.toDouble(&okD);
            if (!okD) return err(QStringLiteral("setValue needs a number"));
            ds->setValue(d); done = true;
        }
    } else if (action == QLatin1String("setText")) {
        if (auto* le = qobject_cast<QLineEdit*>(w)) { le->setText(value); done = true; }
    } else if (action == QLatin1String("setCurrentText")) {
        if (auto* cb = qobject_cast<QComboBox*>(w)) { cb->setCurrentText(value); done = true; }
    } else if (action == QLatin1String("setCurrentIndex")) {
        if (auto* cb = qobject_cast<QComboBox*>(w)) { cb->setCurrentIndex(value.toInt()); done = true; }
    } else {
        return err(QStringLiteral("unknown action: ") + action);
    }

    if (!done)
        return err(QStringLiteral("action '") + action + QStringLiteral("' not applicable to ")
                   + shortClassName(w));

    qCInfo(lcAutomation).noquote()
        << "invoke" << action << "on" << target << "(" << shortClassName(w) << ")";

    QJsonObject r{
        {QStringLiteral("ok"), true},
        {QStringLiteral("target"), target},
        {QStringLiteral("class"), shortClassName(w)},
        {QStringLiteral("action"), action},
    };
    const QString nv = widgetValue(w);   // round-trip confirmation
    if (!nv.isNull())
        r[QStringLiteral("newValue")] = nv;
    return r;
}

QJsonObject AutomationServer::doGet(const QString& model, const QString& selector,
                                    const QString& property) const
{
    RadioModel* radio = m_radioModel;
    if (!radio)
        return err(QStringLiteral("no radio model available"));

    // Build the payload object for the requested model, then optionally narrow
    // to a single property.
    QJsonObject data;

    if (model == QLatin1String("radio")) {
        data = radioSnapshot(radio);
    } else if (model == QLatin1String("transmit")) {
        data = transmitSnapshot(&radio->transmitModel());
    } else if (model == QLatin1String("equalizer") || model == QLatin1String("eq")) {
        data = equalizerSnapshot(&radio->equalizerModel());
    } else if (model == QLatin1String("meters")) {
        data = metersSnapshot(&radio->meterModel(), radio->model());
    } else if (model == QLatin1String("slices")) {
        QJsonArray arr;
        for (const SliceModel* s : radio->slices()) arr.append(sliceSnapshot(s));
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("slices"), arr}};
    } else if (model == QLatin1String("pans")) {
        QJsonArray arr;
        for (const PanadapterModel* p : radio->panadapters()) arr.append(panSnapshot(p));
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("pans"), arr}};
    } else if (model == QLatin1String("slice")) {
        const SliceModel* s = nullptr;
        const QList<SliceModel*> slices = radio->slices();
        if (selector.isEmpty() || selector == QLatin1String("active")) {
            for (SliceModel* c : slices) if (c->isActive()) { s = c; break; }
            if (!s && !slices.isEmpty()) s = slices.first();
        } else if (selector == QLatin1String("tx")) {
            for (SliceModel* c : slices) if (c->isTxSlice()) { s = c; break; }
        } else {
            bool okId = false; const int id = selector.toInt(&okId);
            if (okId) s = radio->slice(id);
        }
        if (!s)
            return err(QStringLiteral("no slice for selector '") + selector + QStringLiteral("'"));
        data = sliceSnapshot(s);
    } else if (model == QLatin1String("pan")) {
        const PanadapterModel* p = nullptr;
        if (selector.isEmpty() || selector == QLatin1String("active"))
            p = radio->activePanadapter();
        else
            p = radio->panadapter(selector);   // by panId, e.g. "0x40000000"
        if (!p)
            return err(QStringLiteral("no panadapter for selector '") + selector + QStringLiteral("'"));
        data = panSnapshot(p);
    } else {
        return err(QStringLiteral("unknown model: ") + model
                   + QStringLiteral(" (use radio|transmit|equalizer|meters|slice|slices|pan|pans)"));
    }

    if (!property.isEmpty()) {
        if (!data.contains(property))
            return err(QStringLiteral("no property '") + property + QStringLiteral("' on ") + model);
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("model"), model},
            {QStringLiteral("property"), property},
            {QStringLiteral("value"), data.value(property)},
        };
    }

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("model"), model},
        {model, data},   // keyed by model name: "radio" / "slice" / "pan"
    };
}

// ── TX test-signal control (#3646) ──────────────────────────────────────────
// `txtest twotone` starts the radio's two-tone test (a modulated signal that
// exercises ALC / PEP / linearity meters a steady carrier can't). `txtest off`
// stops it. Keying is gated by AETHER_AUTOMATION_ALLOW_TX, and the TX watchdog
// still backstops it. NOTE: two-tone does not pass through the mic/speech
// processor, so it does not exercise the compression meter — that needs a real
// mic-audio source (DAX TX), which is a separate, larger effort.
QJsonObject AutomationServer::doTxTest(const QString& action)
{
    if (!m_radioModel)
        return err(QStringLiteral("no radio model available"));
    auto& tx = m_radioModel->transmitModel();

    if (action == QLatin1String("off") || action == QLatin1String("stop")) {
        tx.stopTune();
        m_txKeyedSinceMs = 0;
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("txtest"), QStringLiteral("off")}};
    }
    if (action == QLatin1String("twotone")) {
        if (!m_txAllowed)
            return err(QStringLiteral("blocked: txtest keys the transmitter — "
                                      "set AETHER_AUTOMATION_ALLOW_TX=1 to allow"));
        tx.startTwoToneTune();
        m_txKeyedSinceMs = QDateTime::currentMSecsSinceEpoch();  // arm watchdog window
        qCInfo(lcAutomation) << "txtest two-tone started (ALLOW_TX)";
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("txtest"), QStringLiteral("twotone")}};
    }
    return err(QStringLiteral("unknown txtest action: ") + action + QStringLiteral(" (twotone|off)"));
}

// ── ATU control (#3646) ─────────────────────────────────────────────────────
// `atu bypass` takes the tuner out of circuit (no TX), so meter readings see
// the raw load instead of a recalled antenna match — essential before TX meter
// measurements. `atu start` runs a tune cycle (keys TX → gated by ALLOW_TX).
QJsonObject AutomationServer::doAtu(const QString& action)
{
    if (!m_radioModel)
        return err(QStringLiteral("no radio model available"));
    auto& tx = m_radioModel->transmitModel();

    if (action == QLatin1String("bypass")) {
        tx.atuBypass();   // relay switch only — does not transmit
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("atu"), QStringLiteral("bypass")}};
    }
    if (action == QLatin1String("start") || action == QLatin1String("tune")) {
        if (!m_txAllowed)
            return err(QStringLiteral("blocked: atu start keys the transmitter — "
                                      "set AETHER_AUTOMATION_ALLOW_TX=1 to allow"));
        tx.atuStart();
        m_txKeyedSinceMs = QDateTime::currentMSecsSinceEpoch();
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("atu"), QStringLiteral("start")}};
    }
    return err(QStringLiteral("unknown atu action: ") + action + QStringLiteral(" (bypass|start)"));
}

// Emergency all-stop: drop tune, two-tone, and MOX immediately. Used by the
// watchdog and stop().
void AutomationServer::forceUnkey(const char* reason)
{
    if (!m_radioModel)
        return;
    auto& tx = m_radioModel->transmitModel();
    tx.stopTune();
    tx.setMox(false);
    m_txKeyedSinceMs = 0;
    qCWarning(lcAutomation).noquote() << "TX force-unkey:" << reason;
}

// TX safety watchdog (#3646). Runs only when AETHER_AUTOMATION_ALLOW_TX is set.
// Tracks how long the radio has been continuously keyed and force-unkeys past
// the limit, so a hung or abandoned automation script can never leave a live
// transmitter on. The limit is AETHER_AUTOMATION_TX_MAX_MS (default 20 s).
void AutomationServer::onTxWatchdog()
{
    if (!m_radioModel)
        return;
    const auto& tx = m_radioModel->transmitModel();
    const bool keyed = tx.isTransmitting() || tx.isTuning() || tx.isMox();
    if (!keyed) {
        m_txKeyedSinceMs = 0;
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_txKeyedSinceMs == 0)
        m_txKeyedSinceMs = now;
    else if (now - m_txKeyedSinceMs > m_txMaxKeyMs)
        forceUnkey("max continuous key time exceeded");
}

QJsonObject AutomationServer::doSlice(const QString& action, const QString& arg)
{
    if (!m_radioModel)
        return err(QStringLiteral("no radio model available"));
    RadioModel* radio = m_radioModel;

    if (action == QLatin1String("add")) {
        // Pre-check radio-wide slot capacity. Slices are a radio resource shared
        // across MultiFlex clients, so a create is refused when every slot is
        // taken — even if WE hold only one. Surface that synchronously instead
        // of issuing a command the radio will silently reject. (#3646)
        int freeSlots = 0;
        QString occupant;
        for (int id = 0; id < radio->maxSlices(); ++id) {
            if (!radio->isSlotOurs(id) && !radio->isSlotForeign(id)) freeSlots++;
            else if (radio->isSlotForeign(id) && occupant.isEmpty())
                occupant = radio->foreignSliceOwnerStation(id);
        }
        if (freeSlots == 0) {
            QString msg = QStringLiteral("refused: no free slice slot (radio at its ")
                + QString::number(radio->maxSlices()) + QStringLiteral("-slice limit");
            if (!occupant.isEmpty())
                msg += QStringLiteral("; a foreign client '") + occupant + QStringLiteral("' holds a slot");
            return err(msg + QStringLiteral(")"));
        }

        bool okF = false;
        const double freq = arg.toDouble(&okF);
        if (okF && freq > 0)
            radio->addSliceOnPan(radio->panId(), freq);   // specific frequency
        else
            radio->addSlice();                            // default (TX freq / active pan)
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("slice"), QStringLiteral("add")},
                           {QStringLiteral("freq"), okF ? QJsonValue(freq) : QJsonValue()},
                           {QStringLiteral("requested"), true},
                           {QStringLiteral("sliceCount"), radio->slices().size()}};
    }
    if (action == QLatin1String("remove")) {
        bool okId = false;
        const int id = arg.toInt(&okId);
        if (!okId)
            return err(QStringLiteral("slice remove requires a slice id"));
        if (radio->slices().size() <= 1)
            return err(QStringLiteral("refused: cannot remove the last slice"));
        if (!radio->slice(id))
            return err(QStringLiteral("no slice with id ") + arg);
        radio->sendCommand(QStringLiteral("slice remove %1").arg(id));
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("slice"), QStringLiteral("remove")},
                           {QStringLiteral("id"), id}};
    }
    if (action == QLatin1String("select")) {
        bool okId = false;
        const int id = arg.toInt(&okId);
        if (!okId || !radio->slice(id))
            return err(QStringLiteral("slice select requires a valid slice id"));
        radio->sendCommand(QStringLiteral("slice set %1 active=1").arg(id));
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("slice"), QStringLiteral("select")},
                           {QStringLiteral("id"), id}};
    }
    return err(QStringLiteral("unknown slice action: ") + action + QStringLiteral(" (add|remove|select)"));
}

// ── VFO tuning (#3646) ──────────────────────────────────────────────────────
// Set the active slice's frequency (MHz). The most fundamental control the
// VfoWidget couldn't expose (it's custom-painted). Honors the slice lock guard.
QJsonObject AutomationServer::doTune(const QString& value)
{
    if (!m_radioModel)
        return err(QStringLiteral("no radio model available"));
    bool okF = false;
    const double mhz = value.toDouble(&okF);
    if (!okF || mhz <= 0)
        return err(QStringLiteral("tune requires a positive frequency in MHz"));

    SliceModel* s = nullptr;
    for (SliceModel* c : m_radioModel->slices())
        if (c->isActive()) { s = c; break; }
    if (!s && !m_radioModel->slices().isEmpty())
        s = m_radioModel->slices().first();
    if (!s)
        return err(QStringLiteral("no slice to tune"));
    if (s->isLocked())
        return err(QStringLiteral("refused: slice ") + s->letter() + QStringLiteral(" is VFO-locked"));

    s->setFrequency(mhz);
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("tune"), mhz},
                       {QStringLiteral("sliceId"), s->sliceId()}, {QStringLiteral("letter"), s->letter()}};
}

// ---------------------------------------------------------------------------
// Observability suite (#3646): log control, event ring, markers.
// ---------------------------------------------------------------------------

namespace {

const char* msgTypeName(int t)
{
    switch (t) {
    case QtDebugMsg:    return "D";
    case QtInfoMsg:     return "I";
    case QtWarningMsg:  return "W";
    case QtCriticalMsg: return "C";
    case QtFatalMsg:    return "F";
    default:            return "?";
    }
}

} // namespace

// Serialize one event for the wire. PII is redacted here, on egress, so the
// in-memory ring stays raw (cheap tap) but nothing sensitive ever leaves.
QJsonObject AutomationServer::logEventToJson(const LogEvent& e)
{
    return QJsonObject{
        {QStringLiteral("type"),    QStringLiteral("log")},
        {QStringLiteral("seq"),     static_cast<qint64>(e.seq)},
        {QStringLiteral("mono_us"), e.monoUs},
        {QStringLiteral("t"),       e.wall},
        {QStringLiteral("lvl"),     QString::fromLatin1(msgTypeName(e.type))},
        {QStringLiteral("cat"),     e.cat},
        {QStringLiteral("msg"),     redactPii(e.msg)},
    };
}

QJsonObject AutomationServer::doLog(const QString& action, const QString& arg,
                                    QLocalSocket* sock)
{
    auto& lm = LogManager::instance();

    if (action == QLatin1String("categories")) {
        QJsonArray arr;
        for (const auto& c : lm.categories())
            arr.append(QJsonObject{{QStringLiteral("id"), c.id},
                                   {QStringLiteral("label"), c.label},
                                   {QStringLiteral("enabled"), c.enabled}});
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("categories"), arr}};
    }

    if (action == QLatin1String("get")) {
        if (arg.isEmpty())
            return err(QStringLiteral("log get requires a category id"));
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("id"), arg},
                           {QStringLiteral("enabled"), lm.isEnabled(arg)}};
    }

    if (action == QLatin1String("set")) {
        // "<id> <on|off>" or "<id>=<on|off>"; id "all" toggles every category.
        const QString id = arg.section(QRegularExpression(QStringLiteral("[ =]")), 0, 0).trimmed();
        const QString st = arg.section(QRegularExpression(QStringLiteral("[ =]")), 1).trimmed().toLower();
        if (id.isEmpty() || st.isEmpty())
            return err(QStringLiteral("log set requires '<category> <on|off>'"));
        const bool on = (st == QLatin1String("on") || st == QLatin1String("true")
                         || st == QLatin1String("1") || st == QLatin1String("debug"));
        if (id == QLatin1String("all")) {
            lm.setAllEnabled(on);
            return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("id"), id},
                               {QStringLiteral("enabled"), on}};
        }
        lm.setEnabled(id, on);
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("id"), id},
                           {QStringLiteral("enabled"), lm.isEnabled(id)}};
    }

    if (action == QLatin1String("reset")) {
        lm.loadSettings();  // restore the operator's persisted category prefs
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("reset"), true}};
    }

    if (action == QLatin1String("tail")) {
        // "[n] [since=<seq>]" — newest n events, optionally only seq > since.
        int n = 100;
        quint64 since = 0;
        for (const QString& tokn : arg.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            if (tokn.startsWith(QLatin1String("since=")))
                since = tokn.mid(6).toULongLong();
            else
                n = tokn.toInt();
        }
        if (n <= 0) n = 100;
        QJsonArray arr;
        quint64 curSeq;
        {
            QMutexLocker lk(&m_logMutex);
            curSeq = m_logSeq;
            for (const auto& e : m_logRing)
                if (e.seq > since)
                    arr.append(logEventToJson(e));
        }
        while (arr.size() > n)              // keep the newest n
            arr.removeFirst();
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("events"), arr},
                           {QStringLiteral("seq"), static_cast<qint64>(curSeq)}};
    }

    if (action == QLatin1String("subscribe")) {
        if (!sock)
            return err(QStringLiteral("subscribe requires a connected client"));
        QMutexLocker lk(&m_logMutex);
        m_logSubscribers.insert(sock, m_logSeq);  // stream events from now on
        if (m_logDrain && !m_logDrain->isActive())
            m_logDrain->start();
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("subscribed"), true},
                           {QStringLiteral("seq"), static_cast<qint64>(m_logSeq)}};
    }

    if (action == QLatin1String("unsubscribe")) {
        const bool was = sock && m_logSubscribers.remove(sock) > 0;
        if (m_logSubscribers.isEmpty() && m_logDrain)
            m_logDrain->stop();
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("subscribed"), false},
                           {QStringLiteral("was"), was}};
    }

    return err(QStringLiteral("log action must be "
                              "categories|get|set|reset|tail|subscribe|unsubscribe"));
}

QJsonObject AutomationServer::doMark(const QString& text)
{
    // qCInfo runs the installed handler synchronously on this (main) thread, so
    // by the time it returns the tap has already assigned the marker its seq —
    // letting a driver bracket actions with `mark` then `log tail since=<seq>`.
    qCInfo(lcAutomation).noquote() << "MARK" << text;
    QMutexLocker lk(&m_logMutex);
    const qint64 mono = m_logRing.empty() ? 0 : m_logRing.back().monoUs;
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("seq"), static_cast<qint64>(m_logSeq)},
                       {QStringLiteral("mono_us"), mono},
                       {QStringLiteral("text"), text}};
}

void AutomationServer::onLogDrain()
{
    if (m_logSubscribers.isEmpty()) {
        if (m_logDrain) m_logDrain->stop();
        return;
    }

    // Copy just the new tail once, under the lock, then write to sockets
    // outside it so logging threads are never blocked on socket I/O.
    quint64 minLast = std::numeric_limits<quint64>::max();
    for (const quint64 v : m_logSubscribers)
        minLast = std::min(minLast, v);

    std::deque<LogEvent> fresh;
    quint64 curSeq;
    {
        QMutexLocker lk(&m_logMutex);
        curSeq = m_logSeq;
        for (const auto& e : m_logRing)
            if (e.seq > minLast)
                fresh.push_back(e);
    }
    if (fresh.empty())
        return;

    for (auto it = m_logSubscribers.begin(); it != m_logSubscribers.end(); ++it) {
        QLocalSocket* s = it.key();
        const quint64 last = it.value();
        for (const auto& e : fresh) {
            if (e.seq <= last)
                continue;
            s->write(QJsonDocument(logEventToJson(e)).toJson(QJsonDocument::Compact));
            s->write("\n");
        }
        s->flush();
        it.value() = curSeq;
    }
}
} // namespace AetherSDR
