#pragma once

#include <QObject>
#include <QString>

namespace AetherSDR {

class AudioEngine;
class CopyAssistPanel;
class CopyAssistSettingsDialog;
class AsrEngine;
class AsrModelManager;
class AsrAudioTap;
struct AsrModelTier;

// Which IAsrBackend the engine is currently built around. Selects the factory in
// buildEngine(); a local model's source (downloaded tier vs. user-supplied
// "custom" file) is an orthogonal axis keyed off the tier id. Extending to a new
// local engine family is a drop-in: add an enumerator, a buildEngine() case, and
// a mapping in backendForTier(). See AsrModelFamily.
enum class AsrBackendKind {
    Whisper,    // local whisper.cpp (a catalog tier or a user "custom" file)
    Remote,     // RemoteAsrBackend over HTTP
    SherpaOnnx, // sherpa-onnx offline model (a user-picked model directory)
};

// Wires the Copy Assist panel to the ASR subsystem (RFC #4333, Phase 5). Owns
// the AsrEngine, the model download manager, and the audio tap, and translates
// panel intent into the enable → (download model) → load → tap-on flow, routing
// transcripts and status back to the panel. Lives in the app layer so the panel
// stays a pure view and aethercore/aetherasr stay decoupled.
class CopyAssistController : public QObject {
    Q_OBJECT
public:
    CopyAssistController(AudioEngine* audio, CopyAssistPanel* panel, QObject* parent = nullptr);
    ~CopyAssistController() override;

    // Clear the transcript and drop any in-progress utterance — used on retune so
    // the decode window starts fresh for the new frequency.
    void clearDecode();

    // A retune (or active-slice switch): record the new frequency (MHz), write a
    // frequency marker to the log (when enabled), and clear the decode window.
    void onRetune(double freqMhz);
    // Seed the current frequency without side effects (used right after the
    // controller is created, before any retune event fires).
    void setCurrentFrequency(double freqMhz);

private slots:
    void onEnableToggled(bool on);
    void onTierChanged(const QString& tierId);

private:
    void buildEngine();  // (re)create the engine+tap for the current backend
    void applyTuning();  // push saved VAD tuning into the engine
    void beginEnable();
    void requestModel(const QString& tierId);
    bool promptRemoteConfig();  // edit + persist the remote endpoint; true if accepted
    QString promptCustomModel(); // pick a local ggml/gguf model file (empty if cancelled)
    QString promptSherpaModel(); // pick a sherpa-onnx model directory (empty if cancelled)
    void promptLogFile();        // pick + persist the transcript log path
    void appendToLogFile(const QString& text); // write one utterance if logging is on
    void promptVadModel();       // pick + persist a custom Silero VAD .onnx (rebuilds)
    void ensureVadModel();       // use the cached model, else auto-download it
    void onVadModelReady(const QString& path); // cached/downloaded → persist + rebuild
    void promptSpeakerModel();   // pick + persist a custom speaker-embedding .onnx
    void ensureSpeakerModel();   // use the cached model, else auto-download it
    void onSpeakerModelReady(const QString& path); // cached/downloaded → persist + rebuild
    void rebuildEngine();  // rebuild the engine so the worker picks up a model change
    static AsrModelTier sileroVadTier();       // default downloadable Silero VAD model
    static AsrModelTier speakerEmbedderTier(); // default downloadable speaker model
    void writeFreqMarkerIfNeeded(); // log a "=== <freq> MHz ===" line on start/retune/day-roll
    bool appendLogRaw(const QString& text); // append verbatim to the dated log; false on error
    // Which backend a selected tier id maps to (catalog family → backend kind;
    // the "custom" file and any unknown id default to local Whisper).
    static AsrBackendKind backendForTier(const QString& tierId);
    // Switch the active backend + tier, clearing the remote flag when leaving
    // remote and rebuilding the engine only when the backend kind actually changes.
    void setBackend(AsrBackendKind kind, const QString& tierId);

    AudioEngine* m_audio = nullptr;
    CopyAssistPanel* m_panel = nullptr;
    CopyAssistSettingsDialog* m_settings = nullptr; // modeless model/GPU/options dialog
    AsrEngine* m_asr = nullptr;
    AsrModelManager* m_models = nullptr;
    AsrModelManager* m_vadModels = nullptr; // separate manager for the Silero VAD model
    AsrModelManager* m_speakerModels = nullptr; // manager for the speaker-embedding model
    AsrAudioTap* m_tap = nullptr;
    bool m_constructed = false; // true after the initial buildEngine (guards restore)
    QString m_tierId;
    QString m_customModelPath; // user-picked local model (for the "Custom model…" tier)
    QString m_sherpaModelDir;  // user-picked sherpa-onnx model directory
    double m_currentFreqMhz = 0.0;  // active-slice frequency, for the log marker
    QString m_lastFreqMarkerKey;    // (dated-file|freq) last marked — dedups markers
    bool m_enabled = false;
    AsrBackendKind m_backend = AsrBackendKind::Whisper; // active inference backend
};

} // namespace AetherSDR
