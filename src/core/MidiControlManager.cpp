#ifdef HAVE_MIDI

#include "MidiControlManager.h"
#include "CwTrace.h"
#include "LogManager.h"

#include <RtMidi.h>
#include <QDateTime>
#include <QDebug>
#include <cmath>

namespace AetherSDR {

namespace {

QString midiMsgTypeName(MidiBinding::MsgType type)
{
    switch (type) {
    case MidiBinding::CC:        return QStringLiteral("CC");
    case MidiBinding::NoteOn:    return QStringLiteral("NoteOn");
    case MidiBinding::NoteOff:   return QStringLiteral("NoteOff");
    case MidiBinding::PitchBend: return QStringLiteral("PitchBend");
    }
    return QStringLiteral("Unknown");
}

bool isCwMomentaryParamId(const QString& paramId)
{
    return paramId == QLatin1String("cwkey")
        || paramId == QLatin1String("cwdit")
        || paramId == QLatin1String("cwdah")
        || paramId == QLatin1String("cw.key")
        || paramId == QLatin1String("cw.dit")
        || paramId == QLatin1String("cw.dah");
}

bool isVfoTuneKnobParamId(const QString& paramId)
{
    return paramId == QLatin1String("rx.tuneKnob");
}

bool isUnitRelativeCcPulse(int value)
{
    return value == 1 || value == 127;
}

int relativeCcDelta(int value)
{
    return (value < 64) ? value : (value - 128);
}

} // namespace

// ── MidiBinding helpers ─────────────────────────────────────────────────────

QString MidiBinding::sourceDisplayName() const
{
    QString typeName;
    switch (msgType) {
    case CC:       typeName = QString("CC #%1").arg(number); break;
    case NoteOn:   typeName = QString("Note %1").arg(number); break;
    case NoteOff:  typeName = QString("NoteOff %1").arg(number); break;
    case PitchBend: typeName = "Pitch Bend"; break;
    }
    if (channel >= 0)
        return QString("Ch %1 %2").arg(channel + 1).arg(typeName);
    return typeName;
}

// ── MidiControlManager ──────────────────────────────────────────────────────

MidiControlManager::MidiControlManager(QObject* parent)
    : QObject(parent)
    , m_hotplugTimer(new QTimer(this))
    , m_relativeTimer(new QTimer(this))
{
    m_hotplugTimer->setInterval(5000);
    connect(m_hotplugTimer, &QTimer::timeout, this, [this] {
        // Hotplug: if disconnected and we have a saved port name, try to reconnect
        if (m_midiIn && m_midiIn->isPortOpen()) return;
        if (m_portName.isEmpty()) return;
        openPortByName(m_portName);
    });

    // Coalesce relative knob events every 20ms. Some generic relative controls
    // use density-based acceleration below, but VFO tune pulses are passed
    // through exactly so controller detents map to radio step increments.
    m_relativeTimer->setInterval(20);
    connect(m_relativeTimer, &QTimer::timeout, this, &MidiControlManager::flushRelativeAccum);
}

MidiControlManager::~MidiControlManager()
{
    closePort();
}

// ── Device management ───────────────────────────────────────────────────────

QStringList MidiControlManager::availablePorts() const
{
    QStringList result;
    try {
        RtMidiIn probe;
        unsigned int n = probe.getPortCount();
        for (unsigned int i = 0; i < n; ++i)
            result.append(QString::fromStdString(probe.getPortName(i)));
    } catch (const RtMidiError& e) {
        qCWarning(lcDevices) << "MidiControlManager: error enumerating ports:" << e.what();
    }
    return result;
}

bool MidiControlManager::openPort(int portIndex)
{
    closePort();
    try {
        m_midiIn = std::make_unique<RtMidiIn>();
        if (static_cast<unsigned>(portIndex) >= m_midiIn->getPortCount()) {
            qCWarning(lcDevices) << "MidiControlManager: port index" << portIndex << "out of range";
            m_midiIn.reset();
            return false;
        }
        m_portName = QString::fromStdString(m_midiIn->getPortName(portIndex));
        m_midiIn->openPort(portIndex);
        m_midiIn->setCallback(&rtmidiCallback, this);
        m_midiIn->ignoreTypes(true, true, true); // ignore sysex, timing, active sensing
        qCDebug(lcDevices) << "MidiControlManager: opened port" << m_portName;
        m_hotplugTimer->start();
        emit portOpened(m_portName);
        return true;
    } catch (const RtMidiError& e) {
        qCWarning(lcDevices) << "MidiControlManager: open failed:" << e.what();
        m_midiIn.reset();
        emit portError(QString::fromStdString(e.getMessage()));
        return false;
    }
}

bool MidiControlManager::openPortByName(const QString& portName)
{
    try {
        RtMidiIn probe;
        unsigned int n = probe.getPortCount();
        for (unsigned int i = 0; i < n; ++i) {
            if (QString::fromStdString(probe.getPortName(i)).contains(portName, Qt::CaseInsensitive))
                return openPort(static_cast<int>(i));
        }
    } catch (const RtMidiError& e) {
        qCWarning(lcDevices) << "MidiControlManager: error finding port:" << e.what();
        emit portError(QString::fromStdString(e.getMessage()));
    }
    return false;
}

void MidiControlManager::closePort()
{
    m_hotplugTimer->stop();
    if (m_midiIn) {
        try {
            if (m_midiIn->isPortOpen())
                m_midiIn->closePort();
        } catch (...) {}
        m_midiIn.reset();
    }
    emit portClosed();
}

bool MidiControlManager::isOpen() const
{
    return m_midiIn && m_midiIn->isPortOpen();
}

// ── Parameter registry ──────────────────────────────────────────────────────

void MidiControlManager::registerParam(const MidiParam& param)
{
    m_paramIndex[param.id] = m_params.size();
    m_params.append(param);
}

const MidiParam* MidiControlManager::findParam(const QString& id) const
{
    auto it = m_paramIndex.find(id);
    if (it == m_paramIndex.end()) return nullptr;
    return &m_params[it.value()];
}

// ── Binding management ──────────────────────────────────────────────────────

void MidiControlManager::addBinding(const MidiBinding& binding)
{
    // Remove any existing binding for this param
    removeBinding(binding.paramId);
    m_bindings.append(binding);
    rebuildIndex();
}

void MidiControlManager::removeBinding(const QString& paramId)
{
    for (int i = m_bindings.size() - 1; i >= 0; --i) {
        if (m_bindings[i].paramId == paramId) {
            m_bindings.removeAt(i);
        }
    }
    rebuildIndex();
}

void MidiControlManager::clearBindings()
{
    m_bindings.clear();
    m_bindingIndex.clear();
}

void MidiControlManager::rebuildIndex()
{
    m_bindingIndex.clear();
    for (int i = 0; i < m_bindings.size(); ++i)
        m_bindingIndex[m_bindings[i].key()] = i;
}

// ── MIDI Learn ──────────────────────────────────────────────────────────────

void MidiControlManager::startLearn(const QString& paramId)
{
    m_learning = true;
    m_learnParamId = paramId;
    qCDebug(lcDevices) << "MidiControlManager: learning for" << paramId;
}

void MidiControlManager::cancelLearn()
{
    m_learning = false;
    m_learnParamId.clear();
    emit learnCancelled();
}

// ── RtMidi callback → Qt main thread ────────────────────────────────────────

void MidiControlManager::rtmidiCallback(double deltatime,
                                         std::vector<unsigned char>* message,
                                         void* userData)
{
    if (!message || message->size() < 2) return;
    auto* self = static_cast<MidiControlManager*>(userData);
    int status = (*message)[0];
    int data1 = (*message)[1];
    int data2 = message->size() > 2 ? (*message)[2] : 0;
    const quint64 traceId = nextCwTraceId();
    const quint64 callbackMs = cwTraceNowMs();

    if (lcCw().isDebugEnabled()) {
        qCDebug(lcCw).noquote().nospace()
            << "CW MIDI raw trace=" << traceId
            << " t=" << callbackMs << "ms"
            << " status=0x" << QString("%1").arg(status, 2, 16, QChar('0')).toUpper()
            << " data1=" << data1
            << " data2=" << data2
            << " rtDeltaMs=" << QString::number(deltatime * 1000.0, 'f', 3);
    }

    // Bridge to Qt main thread
    QMetaObject::invokeMethod(self, [self, status, data1, data2,
                                     traceId, callbackMs, deltatime]() {
        self->onMidiMessage(status, data1, data2, traceId, callbackMs, deltatime);
    }, Qt::QueuedConnection);
}

void MidiControlManager::onMidiMessage(int status, int data1, int data2,
                                       quint64 traceId, quint64 midiCallbackMs,
                                       double rtDeltaSeconds)
{
    const quint64 dispatchMs = cwTraceNowMs();
    int channel = status & 0x0F;
    int type = status & 0xF0;

    // Determine message type and normalized value
    MidiBinding::MsgType msgType;
    int number = data1;
    float normValue = 0.0f;

    if (type == 0xB0) {          // Control Change
        msgType = MidiBinding::CC;
        normValue = data2 / 127.0f;
    } else if (type == 0x90) {   // Note On
        msgType = MidiBinding::NoteOn;
        normValue = data2 > 0 ? 1.0f : 0.0f;
    } else if (type == 0x80) {   // Note Off
        msgType = MidiBinding::NoteOff;
        normValue = 0.0f;
    } else if (type == 0xE0) {   // Pitch Bend
        msgType = MidiBinding::PitchBend;
        number = -1;
        normValue = (data1 | (data2 << 7)) / 16383.0f;
    } else {
        return; // ignore other message types
    }

    if (lcCw().isDebugEnabled()) {
        qCDebug(lcCw).noquote().nospace()
            << "CW MIDI dispatch trace=" << traceId
            << " t=" << dispatchMs << "ms"
            << " queueLagMs=" << static_cast<qint64>(dispatchMs - midiCallbackMs)
            << " ch=" << (channel + 1)
            << " type=" << midiMsgTypeName(msgType)
            << " number=" << number
            << " norm=" << QString::number(normValue, 'f', 3)
            << " rtDeltaMs=" << QString::number(rtDeltaSeconds * 1000.0, 'f', 3);
    }

    emit midiActivity(channel, static_cast<int>(msgType), number,
                      static_cast<int>(normValue * 127));

    // MIDI Learn mode — capture this message as a binding
    if (m_learning) {
        if (msgType == MidiBinding::NoteOff) return; // ignore NoteOff during learn
        const MidiParam* learnParam = findParam(m_learnParamId);
        if (learnParam
            && learnParam->type == MidiParamType::Slider
            && msgType != MidiBinding::CC
            && msgType != MidiBinding::PitchBend) {
            // Touch-sensitive sliders/knobs can emit a NoteOn before their
            // movement CC.  Slider targets need continuous input, so wait for
            // the actual movement message instead of binding the touch sensor.
            return;
        }

        MidiBinding binding;
        binding.channel = channel;
        binding.msgType = msgType;
        binding.number = (msgType == MidiBinding::PitchBend) ? -1 : data1;
        binding.paramId = m_learnParamId;
        binding.inverted = false;
        binding.relative = isVfoTuneKnobParamId(binding.paramId)
                           && msgType == MidiBinding::CC;

        addBinding(binding);
        m_learning = false;
        qCDebug(lcDevices) << "MidiControlManager: learned" << binding.sourceDisplayName()
                 << "→" << m_learnParamId;
        emit learnCompleted(m_learnParamId, binding);
        m_learnParamId.clear();
        return;
    }

    // Normal dispatch — look up binding and route value
    quint32 key = MidiBinding{channel, msgType, number, {}, false}.key();
    auto it = m_bindingIndex.find(key);

    // Try wildcard channel (-1 → 0xFF in key)
    if (it == m_bindingIndex.end()) {
        quint32 wildKey = (0xFF << 16) | (msgType << 8) |
                          ((msgType == MidiBinding::PitchBend) ? 0xFF : (number & 0x7F));
        it = m_bindingIndex.find(wildKey);
    }

    // For Gate params (CW key): NoteOff must also match NoteOn bindings
    if (it == m_bindingIndex.end() && msgType == MidiBinding::NoteOff) {
        quint32 noteOnKey = MidiBinding{channel, MidiBinding::NoteOn, number, {}, false}.key();
        it = m_bindingIndex.find(noteOnKey);
        if (it == m_bindingIndex.end()) {
            quint32 wildNoteOnKey = (0xFF << 16) | (MidiBinding::NoteOn << 8) | (number & 0x7F);
            it = m_bindingIndex.find(wildNoteOnKey);
        }
    }

    if (it == m_bindingIndex.end()) return;

    const auto& binding = m_bindings[it.value()];
    const bool cwBinding = isCwMomentaryParamId(binding.paramId);

    if (isVfoTuneKnobParamId(binding.paramId)
        && msgType != MidiBinding::CC
        && msgType != MidiBinding::PitchBend) {
        return;
    }

    // ── Relative knob mode: decode delta and accumulate ────────────────
    if (binding.relative && msgType == MidiBinding::CC) {
        // Relative CC encoding (two's complement style):
        // 1-63 = clockwise (1=slow, 63=fast)
        // 65-127 = counter-clockwise (127=-1, 126=-2)
        int delta = relativeCcDelta(data2);
        if (binding.inverted) delta = -delta;

        accumulateRelativeStep(binding.paramId, delta);

        emit paramValueChanged(binding.paramId, delta > 0 ? 1.0f : 0.0f);
        return;
    }

    // Existing VFO tune bindings learned before relative mode could treat
    // common endless-encoder pulses (1 / 127) as absolute endpoints, turning a
    // single detent into roughly +/-63 tuning steps.  Decode those unit pulses
    // as relative even if the saved binding did not mark the control relative.
    if (!binding.relative
        && isVfoTuneKnobParamId(binding.paramId)
        && msgType == MidiBinding::CC
        && isUnitRelativeCcPulse(data2)) {
        int delta = relativeCcDelta(data2);
        if (binding.inverted) delta = -delta;
        accumulateRelativeStep(binding.paramId, delta);
        emit paramValueChanged(binding.paramId, delta > 0 ? 1.0f : 0.0f);
        return;
    }

    // ── Absolute mode: existing behavior ───────────────────────────────
    float value = binding.inverted ? (1.0f - normValue) : normValue;

    // Scale to parameter range
    const MidiParam* param = findParam(binding.paramId);
    if (param) {
        float scaled = param->rangeMin + value * (param->rangeMax - param->rangeMin);

        // For toggles: use > 0.5 threshold; for triggers: only fire on > 0.5
        if (param->type == MidiParamType::Toggle) {
            // Toggle on any rising edge (NoteOn press, CC value > 0.5).
            // Silently ignore falling edges (NoteOff, NoteOn vel=0, CC=0)
            // so the control doesn't revert to off the moment the button
            // is released.
            if (value < 0.5f) return;
            scaled = -1.0f;  // sentinel: MainWindow reads getter and flips (#502)
        } else if (param->type == MidiParamType::Trigger) {
            if (value < 0.5f) return; // only fire on press, not release
            scaled = 1.0f;
        } else if (param->type == MidiParamType::Gate) {
            // Gate: NoteOn vel>0 = key down (1.0), NoteOff or vel=0 = key up (0.0)
            // No toggling — direct follow of key state
            scaled = normValue > 0.0f ? 1.0f : 0.0f;
        }

        if (cwBinding && lcCw().isDebugEnabled()) {
            qCDebug(lcCw).noquote().nospace()
                << "CW MIDI binding trace=" << traceId
                << " t=" << cwTraceNowMs() << "ms"
                << " param=" << binding.paramId
                << " source=\"" << binding.sourceDisplayName() << "\""
                << " value=" << QString::number(value, 'f', 3)
                << " scaled=" << QString::number(scaled, 'f', 3)
                << " callbackLagMs=" << static_cast<qint64>(cwTraceNowMs() - midiCallbackMs);
        }

        // Don't call setter directly — may be on a worker thread while
        // setters access main-thread objects. Emit signal instead. (#502)
        emit paramActionTrace(binding.paramId, scaled, traceId, midiCallbackMs, dispatchMs);
    }

    emit paramValueChanged(binding.paramId, value);
}

// ── Relative knob coalescing ───────────────────────────────────────────────
// Called every 20ms. VFO tuning is emitted as exact accumulated detents.
// Other relative controls keep the legacy density-based acceleration:
//   Slow  (≤2 events/window): ÷2 rate — halve steps for fine control
//   Medium (3-6):              1:1 — normal rate
//   Fast   (>6):               4× — multiply steps for rapid changes

void MidiControlManager::accumulateRelativeStep(const QString& paramId, int delta)
{
    auto& accum = m_relativeAccum[paramId];
    accum.steps += delta;
    accum.eventCount++;
    accum.lastEventMs = QDateTime::currentMSecsSinceEpoch();

    if (!m_relativeTimer->isActive())
        m_relativeTimer->start();
}

void MidiControlManager::flushRelativeAccum()
{
    bool anyActive = false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    for (auto it = m_relativeAccum.begin(); it != m_relativeAccum.end(); ++it) {
        auto& a = it.value();
        if (a.steps == 0 && (now - a.lastEventMs) > 100) continue;

        if (a.steps != 0) {
            int steps = a.steps;

            if (!isVfoTuneKnobParamId(it.key())) {
                // Apply acceleration based on event density in this 20ms window
                if (a.eventCount <= 2) {
                    // Slow: halve steps (skip if only 1 step and alternating)
                    if (steps == 1 || steps == -1) {
                        static int slowDivider = 0;
                        if (++slowDivider % 2 == 0) steps = 0;
                    }
                } else if (a.eventCount > 6) {
                    // Fast: 4× acceleration
                    steps *= 4;
                }
            }
            // Medium (3-6 events): 1:1, no change

            if (steps != 0)
                emit relativeAction(it.key(), steps);

            a.steps = 0;
            a.eventCount = 0;
            anyActive = true;
        }

        // Keep timer alive if we had recent events
        if ((now - a.lastEventMs) < 100)
            anyActive = true;
    }

    if (!anyActive)
        m_relativeTimer->stop();
}

} // namespace AetherSDR

#endif // HAVE_MIDI
