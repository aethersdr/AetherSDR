#pragma once

#ifdef HAVE_MIDI

#include <QObject>
#include <QTimer>
#include <QHash>
#include <QVector>
#include <QString>
#include <functional>
#include <memory>
#include <RtMidi.h>

namespace MasterSDR {

// ── Data types ──────────────────────────────────────────────────────────────

enum class MidiParamType {
    Slider,    // continuous 0.0–1.0 (CC value / 127, Pitch Bend / 16383)
    Toggle,    // on/off (CC > 63 = on, NoteOn velocity > 0 = toggle)
    Trigger,   // momentary (NoteOn = fire once)
    Gate,      // held on/off (NoteOn vel>0 = 1.0, NoteOff/vel=0 = 0.0) — for CW keying
};

struct MidiParam {
    QString        id;           // e.g., "rx.afGain"
    QString        displayName;  // e.g., "AF Gain"
    QString        category;     // e.g., "RX", "TX", "Phone/CW"
    MidiParamType  type;
    float          rangeMin{0};
    float          rangeMax{100};
    std::function<void(float)>  setter;   // called with scaled value (rangeMin..rangeMax)
    std::function<float()>      getter;   // returns current value (for UI sync)
};

struct MidiBinding {
    enum MsgType { CC = 0, NoteOn = 1, NoteOff = 2, PitchBend = 3 };

    int     channel{-1};     // 0-15, or -1 for "any channel"
    MsgType msgType{CC};
    int     number{0};       // CC# or note# (0-127), ignored for PitchBend
    QString paramId;         // target parameter ID
    bool    inverted{false}; // reverse the value range
    bool    relative{false}; // CC sends relative delta (1-63=CW, 65-127=CCW)

    // Unique key for hash lookup
    quint32 key() const {
        int ch = (channel < 0) ? 0xFF : channel;
        int num = (msgType == PitchBend) ? 0xFF : (number & 0x7F);
        return (ch << 16) | (msgType << 8) | num;
    }

    QString sourceDisplayName() const;
};

// ── Manager ─────────────────────────────────────────────────────────────────

class MidiControlManager : public QObject {
    Q_OBJECT

public:
    explicit MidiControlManager(QObject* parent = nullptr);
    ~MidiControlManager() override;

    // Device management
    QStringList availablePorts() const;
    bool openPort(int portIndex);
    bool openPortByName(const QString& portName);
    void closePort();
    bool isOpen() const;
    QString currentPortName() const { return m_portName; }

    // Parameter registry
    void registerParam(const MidiParam& param);
    const QVector<MidiParam>& params() const { return m_params; }
    const MidiParam* findParam(const QString& id) const;

    // Binding management
    void addBinding(const MidiBinding& binding);
    void removeBinding(const QString& paramId);
    void clearBindings();
    QVector<MidiBinding>& bindings() { return m_bindings; }
    const QVector<MidiBinding>& bindings() const { return m_bindings; }
    void rebuildIndex();

    // MIDI Learn
    void startLearn(const QString& paramId);
    void cancelLearn();
    bool isLearning() const { return m_learning; }
    QString learningParamId() const { return m_learnParamId; }

signals:
    void portOpened(const QString& name);
    void portClosed();
    void portError(const QString& error);
    void midiActivity(int channel, int msgType, int number, int value);
    void paramValueChanged(const QString& paramId, float normalizedValue);
    // Emitted with the final scaled value plus trace metadata for
    // MainWindow to dispatch the setter on the main thread (#502) and
    // correlate the event for the CW/netCW diagnostic trail (#2336).
    void paramActionTrace(const QString& paramId, float scaledValue,
                          quint64 traceId, quint64 midiCallbackMs,
                          quint64 midiDispatchMs);
    // Emitted for relative knobs: accumulated steps with acceleration.
    // Positive = clockwise, negative = counter-clockwise.
    void relativeAction(const QString& paramId, int steps);
    void learnCompleted(const QString& paramId, const MidiBinding& binding);
    void learnCancelled();

private:
    static void rtmidiCallback(double deltatime,
                               std::vector<unsigned char>* message,
                               void* userData);
    void onMidiMessage(int status, int data1, int data2,
                       quint64 traceId, quint64 midiCallbackMs,
                       double rtDeltaSeconds);

    std::unique_ptr<RtMidiIn> m_midiIn;
    QString m_portName;

    QVector<MidiParam>   m_params;
    QHash<QString, int>  m_paramIndex;  // paramId → index into m_params

    QVector<MidiBinding> m_bindings;
    QHash<quint32, int>  m_bindingIndex; // binding key → index into m_bindings

    bool    m_learning{false};
    QString m_learnParamId;

    QTimer* m_hotplugTimer;

    // Relative knob coalescing: accumulate steps over 20ms, apply acceleration
    struct RelativeAccum {
        int   steps{0};
        int   eventCount{0};     // events in this 20ms window for rate detection
        qint64 lastEventMs{0};   // for inter-event rate measurement
    };
    QHash<QString, RelativeAccum> m_relativeAccum; // paramId → accum
    QTimer* m_relativeTimer{nullptr};
    void accumulateRelativeStep(const QString& paramId, int delta);
    void flushRelativeAccum();
};

} // namespace MasterSDR

#endif // HAVE_MIDI
