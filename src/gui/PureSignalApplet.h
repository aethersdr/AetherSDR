#pragma once

#include <QPointer>
#include <QSet>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QFrame;
class QLabel;
class QPushButton;
class QSlider;

namespace AetherSDR {

class IRadioBackend;
struct RadioCapabilities;

// The "hl2" extension's "puresignal" status map as this applet reads it. The
// applet sits above the radio seam, so it reaches the HL2 only through
// IRadioBackend::invokeExtension / extensionStatus and keeps its own copy of
// the few numbers it needs; the map overrides them when it carries them.
struct PureSignalView {
    bool available = false;
    QString unavailableReason;
    bool enabled = false;
    bool autoCalibrate = false;
    bool autoAttenuate = true;
    bool calibrating = false;
    bool correcting = false;
    int feedbackLevel = 0;
    int calibrationCount = 0;
    int engineState = 0;
    int attenuationDb = 31;
    int attMin = -28;
    int attMax = 31;
    bool keyed = false;
    // Only the keys present in `fields` change `base`. (No `= {}` default:
    // GCC and Clang reject it inside the class's own definition.)
    [[nodiscard]] static PureSignalView fromFields(const QVariantMap& fields,
                                                   PureSignalView base);
};

// The feedback level bar: 0..256 with calcc's target window (128..181) marked,
// coloured the way Thetis colours its feedback indicator.
class PureSignalFeedbackMeter : public QWidget {
    Q_OBJECT
public:
    explicit PureSignalFeedbackMeter(QWidget* parent = nullptr);
    // `level` < 0 shows the empty track (no keyed data yet).
    void setLevel(int level);
    [[nodiscard]] int level() const noexcept { return m_level; }
    QSize sizeHint() const override { return {200, 18}; }
    QSize minimumSizeHint() const override { return {80, 18}; }
    // Thetis' colour bands, exposed for the test.
    enum class Band { None, Low, Marginal, Good, High };
    [[nodiscard]] static Band bandFor(int level) noexcept;
protected:
    void paintEvent(QPaintEvent*) override;
private:
    int m_level = -1;
};

// HL2 PureSignal controls. Shown only while a Hermes-Lite 2 backend is bound;
// AppletPanel owns the tile's visibility, this widget never hides itself.
class PureSignalApplet : public QWidget {
    Q_OBJECT
public:
    explicit PureSignalApplet(QWidget* parent = nullptr);
    ~PureSignalApplet() override;
    // Binds when the backend advertises the hl2 extension's puresignal verbs;
    // anything else (another family, or null) unbinds.
    void setBackend(IRadioBackend* backend);
    [[nodiscard]] static bool supports(const RadioCapabilities& caps);
    [[nodiscard]] bool isAvailable() const { return !m_backend.isNull(); }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    [[nodiscard]] static QString engineStateName(int state);
signals:
    void availabilityChanged(bool available);
private:
    void onStatus(const PureSignalView& status);
    void onFields(const QVariantMap& fields);
    // invokeExtension("hl2", "puresignal." + verb), tracked so the reply and a
    // refusal can be told apart from other clients' traffic.
    void send(const QString& verb, const QVariant& arg = {});
    void unbind();
    void setSettingsExpanded(bool expanded);
    void savePreferences();
    void pushPreferences();
    void showAttenuation(int att);

    QPointer<IRadioBackend> m_backend;
    QVector<QMetaObject::Connection> m_connections;
    QSet<quint64> m_pending;
    QString m_error;              // the last refusal, until the next request
    PureSignalView m_status;
    QPushButton* m_enable{nullptr};
    QPushButton* m_auto{nullptr};
    QPushButton* m_single{nullptr};
    PureSignalFeedbackMeter* m_meter{nullptr};
    QLabel* m_readout{nullptr};
    QLabel* m_state{nullptr};
    QPushButton* m_settingsToggle{nullptr};
    QFrame* m_settingsDrawer{nullptr};
    QCheckBox* m_autoAttenuate{nullptr};
    QSlider* m_attenuation{nullptr};
    QLabel* m_attenuationValue{nullptr};
    QPushButton* m_reset{nullptr};
    QPushButton* m_save{nullptr};
    QPushButton* m_restore{nullptr};
};

} // namespace AetherSDR
