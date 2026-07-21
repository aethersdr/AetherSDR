#pragma once

#include <QVector>
#include <QWidget>

class QPushButton;
class QSlider;
class QLabel;

namespace AetherSDR {

// DemoApplet — the docked control tile for demo mode (RFC #4288). Lets the user
// shape the synthetic RX noise the demo radio plays: a per-channel toggle + level
// slider (pink/white/qrn/birdie/…) and one-click scene presets (storm / night-40m
// / …). The AE-native counterpart of flex-sim's web "Noise Bench".
//
// It owns no engine state — it EMITS intents (demoNoiseToggled / …) that
// MainWindow forwards to PanadapterStream's Q_INVOKABLE demo-scene setters via a
// queued connection (the mixer lives on the network thread). Shown only while the
// demo radio is connected; AppletPanel gates its visibility.
class DemoApplet : public QWidget {
    Q_OBJECT

public:
    explicit DemoApplet(QWidget* parent = nullptr);

signals:
    // User intents — MainWindow routes these to PanadapterStream::setDemoNoise*.
    void demoNoiseToggled(const QString& channel, bool on);
    void demoNoiseLevelChanged(const QString& channel, double levelDb);
    void demoNoiseKnobChanged(const QString& channel, const QString& knob, double value);
    void demoPresetRequested(const QString& presetName);

private:
    void buildUI();
    // Reflect a preset into the controls locally (presets are absolute: everything
    // off, then the named channels on) so the sliders/toggles track a preset click.
    void applyPresetToControls(const QString& presetName);

    struct ChannelRow {
        QString   name;       // NoiseMixer channel name
        QString   knob;       // "hz"/"rate"/… or empty
        QPushButton* toggle{nullptr};
        QSlider*  level{nullptr};
        QLabel*   levelLabel{nullptr};
        QSlider*  knobSlider{nullptr};   // nullptr when knob is empty
        QLabel*   knobLabel{nullptr};
    };
    QVector<ChannelRow> m_rows;
};

}  // namespace AetherSDR
