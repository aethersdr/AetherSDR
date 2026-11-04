#pragma once

#include "core/backends/hl2/Hl2PureSignal.h"

#include <QPointer>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QFrame;
class QLabel;
class QPushButton;
class QSlider;

namespace AetherSDR {

namespace hl2 { class Hl2Backend; }

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
    // Null unbinds (a non-HL2 radio, or none).
    void setBackend(hl2::Hl2Backend* backend);
    [[nodiscard]] bool isAvailable() const { return !m_backend.isNull(); }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    [[nodiscard]] static QString engineStateName(int state);
signals:
    void availabilityChanged(bool available);
private:
    void onStatus(const hl2::Hl2PureSignalStatus& status);
    void setSettingsExpanded(bool expanded);
    void savePreferences();
    void pushPreferences();
    void showAttenuation(int att);

    QPointer<hl2::Hl2Backend> m_backend;
    QVector<QMetaObject::Connection> m_connections;
    hl2::Hl2PureSignalStatus m_status;
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
