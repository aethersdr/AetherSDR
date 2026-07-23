#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;

namespace AetherSDR {

// Modeless settings dialog for Copy Assist (RFC #4333). Houses the model-tier
// and compute-device (GPU/CPU) selectors — moved out of the panel's cramped
// control row — behind the panel's ⚙ button, with room for further options.
//
// Like CopyAssistPanel it stays ThemeManager-free (so it links in the
// lightweight offscreen unit test); the controller populates it, wires its
// signals, and applies any theming. It mirrors the panel's old model/GPU API so
// the controller's call sites move over unchanged.
class CopyAssistSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit CopyAssistSettingsDialog(QWidget* parent = nullptr);

    // Model tier selector (id + human label).
    void addTier(const QString& id, const QString& label);
    void setCurrentTier(const QString& id);
    void setTierLabel(const QString& id, const QString& label);
    QString currentTier() const;

    // Compute-device selector — shown only when the controller finds a GPU.
    void addGpuDevice(int index, const QString& name);
    void setCurrentGpu(int index);
    int currentGpu() const;
    void setGpuSelectorVisible(bool on);

    // Transcript-to-file logging: a checkbox + a (controller-populated) path. The
    // controller owns the file picker and the actual writing.
    void setLogToFile(bool on);
    bool logToFile() const;
    void setLogFilePath(const QString& path);
    QString logFilePath() const;

    // Learned Silero VAD (ONNX) in place of the energy VAD: a checkbox + an
    // .onnx path. The controller owns the file picker and rebuilds the engine.
    void setUseSileroVad(bool on);
    bool useSileroVad() const;
    void setVadModelPath(const QString& path);
    QString vadModelPath() const;

    // Per-utterance speaker labeling (A/B/C…) via a speaker-embedding .onnx.
    void setLabelSpeakers(bool on);
    bool labelSpeakers() const;
    void setSpeakerModelPath(const QString& path);
    QString speakerModelPath() const;
    // Cosine match threshold as a percent 0–100 (higher = stricter → more, finer
    // speaker splits; lower = looser → fewer, merged speakers).
    void setSpeakerThreshold(int percent);
    int speakerThreshold() const;

signals:
    void tierChanged(const QString& tierId);
    void gpuChanged(int index);
    void logToFileToggled(bool on);
    void browseLogFileRequested();
    void useSileroVadToggled(bool on);
    void browseVadModelRequested();
    void labelSpeakersToggled(bool on);
    void browseSpeakerModelRequested();
    void speakerThresholdChanged(int percent);

private:
    QComboBox* m_tier = nullptr;
    QComboBox* m_gpu = nullptr;
    QLabel* m_gpuLabel = nullptr;   // paired with m_gpu so both hide together
    QCheckBox* m_logToFile = nullptr;
    QLineEdit* m_logPath = nullptr; // read-only display of the chosen path
    QPushButton* m_logBrowse = nullptr;
    QCheckBox* m_useSilero = nullptr;
    QLineEdit* m_vadPath = nullptr;
    QPushButton* m_vadBrowse = nullptr;
    QCheckBox* m_labelSpeakers = nullptr;
    QLineEdit* m_spkPath = nullptr;
    QPushButton* m_spkBrowse = nullptr;
    QSlider* m_spkThreshold = nullptr;
    QLabel* m_spkThresholdValue = nullptr;
};

} // namespace AetherSDR
