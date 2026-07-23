#pragma once

#include <QString>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QTextEdit;

namespace AetherSDR {

// Copy Assist — the speech-to-text decode panel (RFC #4333, Phase 5), modeled
// on the CW (ggmorse) decode panel in PanadapterApplet: a scrolling, read-only
// transcript whose text is color-coded by whisper's per-utterance confidence
// (green = high … red = low, the inverse of the CW decoder's cost coloring),
// plus enable / model-tier / clear controls and a status line.
//
// Pure view: it emits intent (enableToggled / tierChanged / clearRequested) and
// renders whatever appendText()/setStatus() it is given. The controller owns the
// ASR engine and wiring, so this widget links no ASR/whisper code and could be
// driven by streamed results in the thin-UI/aetherd future.
class CopyAssistPanel : public QWidget {
    Q_OBJECT
public:
    explicit CopyAssistPanel(QWidget* parent = nullptr);

    // The ⚙ settings button — exposed so the controller can apply the themed
    // style and toggle the modeless settings dialog (which now owns the model +
    // compute-device pickers). Panel stays ThemeManager-free.
    QPushButton* settingsButton() const { return m_settings; }

    void setStatus(const QString& text);
    // Set the always-visible transcription backlog (seconds of received audio not
    // yet transcribed). Colour escalates amber→red as it grows.
    void setBacklog(double seconds);
    // Show/hide the indeterminate loading indicator (model download/verify/load).
    void setBusy(bool on);
    bool isAsrEnabled() const;
    void setAsrEnabled(bool on);
    // The Enable/Disable toggle button — exposed so the app layer can apply the
    // themed applet-toggle style (the panel itself stays ThemeManager-free).
    QPushButton* enableButton() const { return m_enable; }
    // The ↵ newline-on-silence toggle — exposed so the controller can apply the
    // themed applet-toggle style (panel stays ThemeManager-free).
    QPushButton* newlineButton() const { return m_newline; }

    // When on, each utterance (VAD end-of-speech) begins on a new line.
    void setNewlineOnSilence(bool on);
    bool newlineOnSilence() const { return m_newlineOnSilence; }

    // Decode-buffer size in milliseconds (1000–20000). The slider works in
    // whole seconds; setBufferMs rounds/clamps into range.
    void setBufferMs(int ms);
    int bufferMs() const;

    // VAD sensitivity as a percentage 1–100 (higher = more sensitive).
    void setSensitivity(int percent);
    int sensitivity() const;

    // Silence duration (hangover) that ends an utterance, in ms (100–2000).
    void setSilenceMs(int ms);
    int silenceMs() const;

    // Transcript font size in px (8–32).
    void setFontPx(int px);
    int fontPx() const { return m_fontPx; }

public slots:
    // Append one transcribed utterance, colored by confidence in [0, 1].
    void appendText(const QString& text, float confidence);
    void clearText();

signals:
    void enableToggled(bool on);
    void settingsRequested();
    void clearRequested();
    void bufferMsChanged(int ms);
    void sensitivityChanged(int percent);
    void silenceMsChanged(int ms);
    void fontPxChanged(int px);
    void newlineOnSilenceChanged(bool on);

private:
    static QString colorForConfidence(float confidence);
    void applyFont();
    void adjustFont(int deltaPx);
    // Append "<label> [compact slider] <value>" inline to the control bar,
    // mirroring the CW decode bar's fixed-width sliders.
    QSlider* addSliderInline(QHBoxLayout* bar, const QString& label,
                             const QString& accessibleName, int lo, int hi, int value,
                             QLabel** valueLabelOut);

    QTextEdit* m_text = nullptr;
    QPushButton* m_enable = nullptr;   // checkable: "Enable" / "Disable"
    QPushButton* m_newline = nullptr;  // checkable ↵: newline on each silence
    QPushButton* m_settings = nullptr; // ⚙: opens the modeless settings dialog
    QLabel* m_status = nullptr;
    QLabel* m_backlog = nullptr; // always-visible transcription backlog (seconds)
    QPushButton* m_clear = nullptr;
    QSlider* m_buffer = nullptr;
    QLabel* m_bufferValue = nullptr;
    QSlider* m_sensitivity = nullptr;
    QLabel* m_sensitivityValue = nullptr;
    QSlider* m_silence = nullptr;
    QLabel* m_silenceValue = nullptr;
    QProgressBar* m_busy = nullptr;
    int m_fontPx = 13;
    bool m_newlineOnSilence = false;
};

} // namespace AetherSDR
