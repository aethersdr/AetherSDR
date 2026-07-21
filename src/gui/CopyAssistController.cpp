#include "CopyAssistController.h"

#include "CopyAssistPanel.h"
#include "CopyAssistSettingsDialog.h"

#include "asr/AsrEngine.h"
#include "asr/AsrModelCatalog.h"
#include "asr/AsrModelManager.h"
#include "asr/RemoteAsrBackend.h"
#include "asr/SherpaOnnxBackend.h"
#include "asr/WhisperAsrBackend.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/AsrAudioTap.h"

#include <QPushButton>

#include <QDate>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLineEdit>
#include <QObject>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>

namespace {

constexpr const char* kRemoteTierId = "remote";
constexpr const char* kCustomTierId = "custom";
constexpr const char* kSherpaTierId = "sherpa";

// Map a 1–100 "sensitivity" (higher = more sensitive) to the VAD's RMS energy
// threshold (lower = more sensitive), spanning a practical HF-voice range.
float sensitivityToRms(int percent)
{
    percent = std::clamp(percent, 1, 100);
    constexpr float leastSensitive = 0.050f;
    constexpr float mostSensitive = 0.001f;
    return leastSensitive - (percent - 1) / 99.0f * (leastSensitive - mostSensitive);
}

void saveInt(const char* key, int value)
{
    auto& s = AetherSDR::AppSettings::instance();
    s.setValue(QString::fromLatin1(key), QString::number(value));
    s.save();
}

// Turn the user's base log path into a per-day file by inserting today's date
// before the extension: "logs/net.txt" → "logs/net-2026-07-21.txt" (a file with
// no extension just gets "-2026-07-21" appended). Computed per write, so it rolls
// to a new file at midnight without any timer.
QString datedLogPath(const QString& base)
{
    const QFileInfo fi(base);
    const QString date = QDate::currentDate().toString(Qt::ISODate); // YYYY-MM-DD
    QString name = fi.completeBaseName() + QLatin1Char('-') + date;
    if (!fi.suffix().isEmpty()) {
        name += QLatin1Char('.') + fi.suffix();
    }
    return fi.dir().filePath(name);
}

AetherSDR::RemoteAsrConfig readRemoteConfig()
{
    auto& s = AetherSDR::AppSettings::instance();
    AetherSDR::RemoteAsrConfig cfg;
    cfg.url = s.value(QStringLiteral("AsrRemoteUrl"), QString()).toString();
    cfg.apiKey = s.value(QStringLiteral("AsrRemoteApiKey"), QString()).toString();
    cfg.model = s.value(QStringLiteral("AsrRemoteModel"), QStringLiteral("whisper-1")).toString();
    cfg.language = QStringLiteral("en");
    return cfg;
}

} // namespace

namespace AetherSDR {

CopyAssistController::CopyAssistController(AudioEngine* audio, CopyAssistPanel* panel,
                                          QObject* parent)
    : QObject(parent)
    , m_audio(audio)
    , m_panel(panel)
    , m_models(new AsrModelManager(this))
{
    // The model + compute-device pickers live in a modeless settings dialog
    // opened by the panel's ⚙ button (parented to the panel so it's cleaned up
    // with it).
    m_settings = new CopyAssistSettingsDialog(m_panel);

    // Tier selector: the downloadable model tiers, then a "Custom model…" entry
    // for a user-supplied local .bin/.gguf, then a "Remote server…" entry that
    // routes to the RemoteAsrBackend.
    for (const AsrModelTier& tier : AsrModelCatalog::tiers()) {
        m_settings->addTier(tier.id, tier.displayName);
    }
    m_settings->addTier(QString::fromLatin1(kCustomTierId), tr("Custom model…"));
    if (sherpaOnnxAvailable()) {
        m_settings->addTier(QString::fromLatin1(kSherpaTierId), tr("sherpa-onnx model…"));
    }
    m_settings->addTier(QString::fromLatin1(kRemoteTierId), tr("Remote server…"));

    // Remember a previously-picked custom model so its filename shows in the list
    // (and the file-picker defaults to it) across restarts.
    m_customModelPath =
        AppSettings::instance().value(QStringLiteral("AsrCustomModelPath"), QString()).toString();
    if (!m_customModelPath.isEmpty()) {
        m_settings->setTierLabel(QString::fromLatin1(kCustomTierId),
                                 tr("Custom: %1").arg(QFileInfo(m_customModelPath).fileName()));
    }
    m_sherpaModelDir =
        AppSettings::instance().value(QStringLiteral("AsrSherpaModelDir"), QString()).toString();
    if (!m_sherpaModelDir.isEmpty()) {
        m_settings->setTierLabel(QString::fromLatin1(kSherpaTierId),
                                 tr("Sherpa: %1").arg(QDir(m_sherpaModelDir).dirName()));
    }

    // Initial backend: remote if previously configured+enabled, else the
    // GPU-class model when a GPU exists, else the platform default.
    const bool remoteConfigured =
        AppSettings::instance()
                .value(QStringLiteral("AsrRemoteEnabled"), QStringLiteral("False"))
                .toString() == QStringLiteral("True")
        && !readRemoteConfig().url.isEmpty();
    if (remoteConfigured) {
        m_backend = AsrBackendKind::Remote;
        m_tierId = QString::fromLatin1(kRemoteTierId);
    } else {
        m_backend = AsrBackendKind::Whisper;
        m_tierId = asrGpuAvailable() ? QStringLiteral("large-v3-turbo")
                                     : AsrModelCatalog::defaultTierId();
    }
    m_settings->setCurrentTier(m_tierId);

    // Style the checkable toggles (Enable/Disable and the ↵ newline toggle) like
    // the applet toggle buttons: the checked state fills with the dim-cyan accent
    // so the on state is visibly distinct.
    const QString appletToggleStyle = QStringLiteral(
        "QPushButton { background: {{color.background.1}};"
        " border: 1px solid {{color.background.2}}; border-radius: 3px;"
        " padding: 3px 10px; font-weight: bold; color: {{color.text.primary}}; }"
        "QPushButton:hover { background: {{color.background.2}}; }"
        "QPushButton:checked { background: {{color.accent.dim}};"
        " color: {{color.text.primary}}; border: 1px solid {{color.accent.bright}}; }");
    ThemeManager::instance().applyStyleSheet(m_panel->enableButton(), appletToggleStyle);
    ThemeManager::instance().applyStyleSheet(m_panel->newlineButton(), appletToggleStyle);

    // ⚙ settings button — modeled on the band-stack gear button but themed and
    // sized (via the shared toggle padding) to sit flush with this row's buttons.
    ThemeManager::instance().applyStyleSheet(m_panel->settingsButton(),
        QStringLiteral(
            "QPushButton { background: {{color.background.1}};"
            " border: 1px solid {{color.background.2}}; border-radius: 3px;"
            " padding: 3px 8px; font-weight: bold; color: {{color.text.secondary}}; }"
            "QPushButton:hover { background: {{color.background.2}};"
            " color: {{color.text.primary}}; }"));
    // The ⚙ glyph renders taller than the text buttons; pin the gear to the
    // Enabled button's height so the row stays flush.
    m_panel->settingsButton()->setFixedHeight(m_panel->enableButton()->sizeHint().height());

    // Compute-device selector — shown whenever a GPU exists, so the user can pick
    // a GPU (or several) or force CPU. Hidden on GPU-less hosts (always CPU).
    const std::vector<AsrGpuDevice> gpus = asrGpuDevices();
    if (!gpus.empty()) {
        for (const AsrGpuDevice& g : gpus) {
            m_settings->addGpuDevice(g.index, g.name);
        }
        m_settings->addGpuDevice(-1, tr("CPU")); // force-CPU option
        int saved = AppSettings::instance()
                        .value(QStringLiteral("AsrGpuDevice"), QStringLiteral("0")).toString().toInt();
        if (saved != -1 && (saved < 0 || saved >= static_cast<int>(gpus.size()))) {
            saved = 0;
        }
        m_settings->setCurrentGpu(saved);
        m_settings->setGpuSelectorVisible(true);
    }

    // Panel intent. The ⚙ button toggles the modeless settings dialog; model/GPU
    // changes come from the dialog itself.
    connect(m_panel, &CopyAssistPanel::enableToggled, this, &CopyAssistController::onEnableToggled);
    connect(m_panel, &CopyAssistPanel::settingsRequested, this, [this] {
        if (m_settings->isVisible()) {
            m_settings->hide();
        } else {
            m_settings->show();
            m_settings->raise();
            m_settings->activateWindow();
        }
    });
    connect(m_settings, &CopyAssistSettingsDialog::tierChanged, this, &CopyAssistController::onTierChanged);
    connect(m_settings, &CopyAssistSettingsDialog::gpuChanged, this, [this](int index) {
        saveInt("AsrGpuDevice", index);
        if (m_backend != AsrBackendKind::Remote) {
            m_tap->setEnabled(false);
            buildEngine(); // rebuild the local engine on the chosen GPU
            if (m_enabled) {
                beginEnable();
            }
        }
    });

    // Transcript-to-file logging. Restore the path first, then the checkbox, so
    // the toggle handler sees a path and doesn't prompt during restore.
    m_settings->setLogFilePath(
        AppSettings::instance().value(QStringLiteral("AsrLogFilePath"), QString()).toString());
    m_settings->setLogToFile(
        AppSettings::instance().value(QStringLiteral("AsrLogToFile"), QStringLiteral("False"))
            .toString() == QStringLiteral("True"));
    connect(m_settings, &CopyAssistSettingsDialog::logToFileToggled, this, [this](bool on) {
        auto& st = AppSettings::instance();
        st.setValue(QStringLiteral("AsrLogToFile"), on ? QStringLiteral("True") : QStringLiteral("False"));
        st.save();
        if (on && m_settings->logFilePath().isEmpty()) {
            promptLogFile(); // enabling with no file yet → ask for one
        }
    });
    connect(m_settings, &CopyAssistSettingsDialog::browseLogFileRequested, this,
            [this] { promptLogFile(); });

    // Learned Silero VAD. Restore path then checkbox (so the toggle handler sees
    // the path and doesn't prompt during restore).
    m_settings->setVadModelPath(
        AppSettings::instance().value(QStringLiteral("AsrVadModelPath"), QString()).toString());
    m_settings->setUseSileroVad(
        AppSettings::instance().value(QStringLiteral("AsrVadEnabled"), QStringLiteral("False"))
            .toString() == QStringLiteral("True"));
    // Separate download manager for the Silero VAD model (auto-fetched + SHA-
    // verified + cached like the whisper tiers, so enabling it just works).
    m_vadModels = new AsrModelManager(this);
    connect(m_vadModels, &AsrModelManager::progress, this, [this](qint64 got, qint64 total) {
        m_panel->setStatus(total > 0
                               ? tr("Downloading Silero VAD… %1%").arg(static_cast<int>(got * 100 / total))
                               : tr("Downloading Silero VAD…"));
    });
    connect(m_vadModels, &AsrModelManager::alreadyPresent, this,
            [this](const QString& path) { onVadModelReady(path); });
    connect(m_vadModels, &AsrModelManager::finished, this,
            [this](const QString& path) { onVadModelReady(path); });
    connect(m_vadModels, &AsrModelManager::failed, this, [this](const QString& err) {
        m_panel->setStatus(tr("Silero VAD download failed: %1").arg(err));
        m_settings->setUseSileroVad(false);
    });
    connect(m_settings, &CopyAssistSettingsDialog::useSileroVadToggled, this, [this](bool on) {
        auto& st = AppSettings::instance();
        st.setValue(QStringLiteral("AsrVadEnabled"), on ? QStringLiteral("True") : QStringLiteral("False"));
        st.save();
        if (!m_constructed) {
            return; // restore: the initial buildEngine() already applies the VAD
        }
        if (on) {
            ensureVadModel(); // cached → use it; else auto-download, then rebuild
        } else {
            rebuildEngine();
        }
    });
    connect(m_settings, &CopyAssistSettingsDialog::browseVadModelRequested, this,
            [this] { promptVadModel(); });

    // Speaker-embedding model (auto-download + cache, same as the others) for
    // per-utterance A/B/C labeling.
    m_speakerModels = new AsrModelManager(this);
    connect(m_speakerModels, &AsrModelManager::progress, this, [this](qint64 got, qint64 total) {
        m_panel->setStatus(total > 0
                               ? tr("Downloading speaker model… %1%").arg(static_cast<int>(got * 100 / total))
                               : tr("Downloading speaker model…"));
    });
    connect(m_speakerModels, &AsrModelManager::alreadyPresent, this,
            [this](const QString& path) { onSpeakerModelReady(path); });
    connect(m_speakerModels, &AsrModelManager::finished, this,
            [this](const QString& path) { onSpeakerModelReady(path); });
    connect(m_speakerModels, &AsrModelManager::failed, this, [this](const QString& err) {
        m_panel->setStatus(tr("Speaker model download failed: %1").arg(err));
        m_settings->setLabelSpeakers(false);
    });
    m_settings->setSpeakerModelPath(
        AppSettings::instance().value(QStringLiteral("AsrSpeakerModelPath"), QString()).toString());
    m_settings->setSpeakerThreshold(
        AppSettings::instance().value(QStringLiteral("AsrSpeakerThreshold"), QStringLiteral("50"))
            .toString().toInt());
    m_settings->setLabelSpeakers(
        AppSettings::instance().value(QStringLiteral("AsrSpeakerEnabled"), QStringLiteral("False"))
            .toString() == QStringLiteral("True"));
    connect(m_settings, &CopyAssistSettingsDialog::speakerThresholdChanged, this, [this](int pct) {
        saveInt("AsrSpeakerThreshold", pct);
        m_asr->setSpeakerThreshold(pct / 100.0f); // live, no engine rebuild
    });
    connect(m_settings, &CopyAssistSettingsDialog::labelSpeakersToggled, this, [this](bool on) {
        auto& st = AppSettings::instance();
        st.setValue(QStringLiteral("AsrSpeakerEnabled"), on ? QStringLiteral("True") : QStringLiteral("False"));
        st.save();
        if (!m_constructed) {
            return;
        }
        if (on) {
            ensureSpeakerModel();
        } else {
            rebuildEngine();
        }
    });
    connect(m_settings, &CopyAssistSettingsDialog::browseSpeakerModelRequested, this,
            [this] { promptSpeakerModel(); });

    // Model download → engine load (the handlers read m_asr at call time, so they
    // survive an engine rebuild on backend switch).
    connect(m_models, &AsrModelManager::progress, this, [this](qint64 got, qint64 total) {
        m_panel->setStatus(total > 0
                               ? tr("Downloading model… %1%").arg(static_cast<int>(got * 100 / total))
                               : tr("Downloading model…"));
    });
    connect(m_models, &AsrModelManager::verifying, this,
            [this] { m_panel->setStatus(tr("Verifying model…")); });
    connect(m_models, &AsrModelManager::alreadyPresent, this, [this](const QString& path) {
        m_panel->setStatus(tr("Loading model…"));
        m_asr->setModelPath(path);
    });
    connect(m_models, &AsrModelManager::finished, this, [this](const QString& path) {
        m_panel->setStatus(tr("Loading model…"));
        m_asr->setModelPath(path);
    });
    connect(m_models, &AsrModelManager::failed, this, [this](const QString& err) {
        m_panel->setBusy(false);
        m_panel->setStatus(tr("Model download failed: %1").arg(err));
        m_panel->setAsrEnabled(false);
    });

    // Live VAD tuning (reads m_asr at call time → survives engine rebuild).
    auto& s = AppSettings::instance();
    m_panel->setBufferMs(s.value(QStringLiteral("AsrDecodeBufferMs"), QStringLiteral("20000")).toString().toInt());
    m_panel->setSensitivity(s.value(QStringLiteral("AsrSensitivity"), QStringLiteral("80")).toString().toInt());
    m_panel->setSilenceMs(s.value(QStringLiteral("AsrSilenceMs"), QStringLiteral("300")).toString().toInt());
    m_panel->setFontPx(s.value(QStringLiteral("AsrFontPx"), QStringLiteral("13")).toString().toInt());
    m_panel->setNewlineOnSilence(
        s.value(QStringLiteral("AsrNewlineOnSilence"), QStringLiteral("False")).toString()
        == QStringLiteral("True"));
    connect(m_panel, &CopyAssistPanel::bufferMsChanged, this, [this](int ms) {
        m_asr->setDecodeBufferMs(ms);
        saveInt("AsrDecodeBufferMs", ms);
    });
    connect(m_panel, &CopyAssistPanel::sensitivityChanged, this, [this](int pct) {
        m_asr->setSpeechRms(sensitivityToRms(pct));
        saveInt("AsrSensitivity", pct);
    });
    connect(m_panel, &CopyAssistPanel::silenceMsChanged, this, [this](int ms) {
        m_asr->setSilenceDurationMs(ms);
        saveInt("AsrSilenceMs", ms);
    });
    connect(m_panel, &CopyAssistPanel::fontPxChanged, this,
            [](int px) { saveInt("AsrFontPx", px); });
    connect(m_panel, &CopyAssistPanel::newlineOnSilenceChanged, this, [](bool on) {
        auto& st = AppSettings::instance();
        st.setValue(QStringLiteral("AsrNewlineOnSilence"),
                    on ? QStringLiteral("True") : QStringLiteral("False"));
        st.save();
    });

    buildEngine();
    m_constructed = true; // subsequent VAD toggles may download/rebuild
}

CopyAssistController::~CopyAssistController() = default;

void CopyAssistController::clearDecode()
{
    m_panel->clearText();
    m_asr->reset(); // drop any half-built utterance so it doesn't cross frequencies
}

void CopyAssistController::onRetune(double freqMhz)
{
    m_currentFreqMhz = freqMhz;
    if (m_enabled) {
        // Mark the new frequency in the log before the new frequency's text.
        writeFreqMarkerIfNeeded();
    }
    clearDecode();
}

void CopyAssistController::setCurrentFrequency(double freqMhz)
{
    m_currentFreqMhz = freqMhz;
}

void CopyAssistController::buildEngine()
{
    // Tear down any previous engine+tap (order: tap first — it references the
    // engine) and rebuild for the current backend.
    delete m_tap;
    m_tap = nullptr;
    delete m_asr;

    const int gpuDevice = AppSettings::instance()
                              .value(QStringLiteral("AsrGpuDevice"), QStringLiteral("0"))
                              .toString().toInt();
    // Optional learned (Silero) VAD — an .onnx path enables it in the worker;
    // empty (or the toggle off) keeps the built-in energy VAD.
    AsrSegmenter::Config segConfig;
    auto& appSettings = AppSettings::instance();
    if (appSettings.value(QStringLiteral("AsrVadEnabled"), QStringLiteral("False")).toString()
        == QStringLiteral("True")) {
        segConfig.vadModelPath =
            appSettings.value(QStringLiteral("AsrVadModelPath"), QString()).toString().toStdString();
    }
    // Optional speaker labeling (A/B/C…): a speaker-embedding .onnx path + the
    // cosine match threshold (stored 0–100 → 0.0–1.0).
    if (appSettings.value(QStringLiteral("AsrSpeakerEnabled"), QStringLiteral("False")).toString()
        == QStringLiteral("True")) {
        segConfig.speakerModelPath =
            appSettings.value(QStringLiteral("AsrSpeakerModelPath"), QString()).toString().toStdString();
    }
    segConfig.speakerThreshold =
        appSettings.value(QStringLiteral("AsrSpeakerThreshold"), QStringLiteral("50"))
            .toString().toInt() / 100.0f;
    switch (m_backend) {
    case AsrBackendKind::Remote:
        m_asr = new AsrEngine(remoteAsrBackendFactory(readRemoteConfig()), segConfig, this);
        break;
    case AsrBackendKind::Whisper:
        m_asr = new AsrEngine(whisperAsrBackendFactory(QStringLiteral("en"), gpuDevice),
                              segConfig, this);
        break;
    case AsrBackendKind::SherpaOnnx:
        m_asr = new AsrEngine(sherpaOnnxBackendFactory(), segConfig, this);
        break;
    }
    m_tap = new AsrAudioTap(m_audio, m_asr, this);

    connect(m_asr, &AsrEngine::ready, this, [this] {
        m_panel->setBusy(false);
        if (m_enabled) {
            m_tap->setEnabled(true);
            m_panel->setStatus(m_backend == AsrBackendKind::Remote ? tr("Listening (remote)…")
                                                                   : tr("Listening…"));
            writeFreqMarkerIfNeeded(); // "on start": head the log with the frequency
        }
    });
    connect(m_asr, &AsrEngine::loadFailed, this, [this](const QString& err) {
        m_panel->setBusy(false);
        m_panel->setStatus(tr("Model load failed: %1").arg(err));
        m_panel->setAsrEnabled(false);
    });
    connect(m_asr, &AsrEngine::finalText, this,
            [this](const QString& text, float confidence, int speaker) {
                // Prefix a speaker label ([A], [B]…) when labeling is on.
                const QString labeled =
                    speaker >= 0
                        ? QStringLiteral("[%1] %2").arg(QChar(u'A' + speaker)).arg(text)
                        : text;
                m_panel->appendText(labeled, confidence);
                appendToLogFile(labeled);
            });
    connect(m_asr, &AsrEngine::error, this, [this](const QString& err) { m_panel->setStatus(err); });
    connect(m_asr, &AsrEngine::backlogChanged, m_panel, &CopyAssistPanel::setBacklog);

    applyTuning();
}

void CopyAssistController::applyTuning()
{
    auto& s = AppSettings::instance();
    m_asr->setDecodeBufferMs(s.value(QStringLiteral("AsrDecodeBufferMs"), QStringLiteral("20000")).toString().toInt());
    m_asr->setSpeechRms(sensitivityToRms(
        s.value(QStringLiteral("AsrSensitivity"), QStringLiteral("80")).toString().toInt()));
    m_asr->setSilenceDurationMs(s.value(QStringLiteral("AsrSilenceMs"), QStringLiteral("300")).toString().toInt());
}

void CopyAssistController::onEnableToggled(bool on)
{
    m_enabled = on;
    if (on) {
        m_lastFreqMarkerKey.clear(); // force a fresh start marker for this session
        beginEnable();
    } else {
        m_tap->setEnabled(false);
        m_panel->setBusy(false);
        m_panel->setStatus(tr("Disabled"));
    }
}

void CopyAssistController::onTierChanged(const QString& tierId)
{
    if (tierId == m_tierId) {
        return;
    }

    if (tierId == QString::fromLatin1(kRemoteTierId)) {
        if (!promptRemoteConfig()) {
            m_settings->setCurrentTier(m_tierId); // user cancelled — revert
            return;
        }
        setBackend(AsrBackendKind::Remote, tierId);
    } else if (tierId == QString::fromLatin1(kCustomTierId)) {
        const QString path = promptCustomModel();
        if (path.isEmpty()) {
            m_settings->setCurrentTier(m_tierId); // user cancelled — revert
            return;
        }
        m_customModelPath = path;
        AppSettings::instance().setValue(QStringLiteral("AsrCustomModelPath"), path);
        AppSettings::instance().save();
        m_settings->setTierLabel(QString::fromLatin1(kCustomTierId),
                                 tr("Custom: %1").arg(QFileInfo(path).fileName()));
        setBackend(AsrBackendKind::Whisper, tierId);
    } else if (tierId == QString::fromLatin1(kSherpaTierId)) {
        const QString dir = promptSherpaModel();
        if (dir.isEmpty()) {
            m_settings->setCurrentTier(m_tierId); // user cancelled — revert
            return;
        }
        m_sherpaModelDir = dir;
        AppSettings::instance().setValue(QStringLiteral("AsrSherpaModelDir"), dir);
        AppSettings::instance().save();
        m_settings->setTierLabel(QString::fromLatin1(kSherpaTierId),
                                 tr("Sherpa: %1").arg(QDir(dir).dirName()));
        setBackend(AsrBackendKind::SherpaOnnx, tierId);
    } else {
        setBackend(backendForTier(tierId), tierId);
    }

    if (m_enabled) {
        m_tap->setEnabled(false);
        beginEnable();
    }
}

AsrBackendKind CopyAssistController::backendForTier(const QString& tierId)
{
    if (tierId == QString::fromLatin1(kRemoteTierId)) {
        return AsrBackendKind::Remote;
    }
    if (tierId == QString::fromLatin1(kSherpaTierId)) {
        return AsrBackendKind::SherpaOnnx;
    }
    // A catalog tier routes by its declared engine family; the "custom" file and
    // any unknown id fall through to local whisper.
    if (const AsrModelTier* tier = AsrModelCatalog::tierById(tierId)) {
        switch (tier->family) {
        case AsrModelFamily::Whisper:
            return AsrBackendKind::Whisper;
        case AsrModelFamily::SherpaOnnx:
            return AsrBackendKind::SherpaOnnx;
        }
    }
    return AsrBackendKind::Whisper;
}

void CopyAssistController::setBackend(AsrBackendKind kind, const QString& tierId)
{
    const AsrBackendKind prev = m_backend;
    m_backend = kind;
    m_tierId = tierId;

    // Leaving the remote backend clears the persisted auto-connect flag so the
    // next launch starts on the local engine.
    if (prev == AsrBackendKind::Remote && kind != AsrBackendKind::Remote) {
        AppSettings::instance().setValue(QStringLiteral("AsrRemoteEnabled"), QStringLiteral("False"));
        AppSettings::instance().save();
    }

    // Only a change of backend kind needs a fresh engine; switching models within
    // the same backend (e.g. base → small, or a custom file) reloads via
    // beginEnable() without tearing the engine down.
    if (prev != kind) {
        m_tap->setEnabled(false);
        buildEngine();
    }
}

void CopyAssistController::beginEnable()
{
    m_panel->setBusy(true);
    if (m_backend == AsrBackendKind::Remote) {
        // No local model to fetch — the remote endpoint is contacted per
        // utterance. load() just marks the backend ready.
        m_panel->setStatus(tr("Connecting to remote server…"));
        m_asr->setModelPath(QString());
    } else if (m_tierId == QString::fromLatin1(kCustomTierId)) {
        // User-supplied model: load the picked file directly, bypassing the
        // catalog download + SHA verification (we don't know its checksum).
        if (m_customModelPath.isEmpty() || !QFileInfo::exists(m_customModelPath)) {
            m_panel->setBusy(false);
            m_panel->setStatus(tr("Custom model file not found — pick it again."));
            m_panel->setAsrEnabled(false);
            return;
        }
        m_panel->setStatus(tr("Loading model…"));
        m_asr->setModelPath(m_customModelPath);
    } else if (m_backend == AsrBackendKind::SherpaOnnx) {
        // sherpa-onnx model: load the picked directory directly (the backend
        // discovers the bundle's files). No download/verify.
        if (m_sherpaModelDir.isEmpty() || !QDir(m_sherpaModelDir).exists()) {
            m_panel->setBusy(false);
            m_panel->setStatus(tr("sherpa-onnx model folder not found — pick it again."));
            m_panel->setAsrEnabled(false);
            return;
        }
        m_panel->setStatus(tr("Loading model…"));
        m_asr->setModelPath(m_sherpaModelDir);
    } else {
        m_panel->setStatus(tr("Preparing model…"));
        requestModel(m_tierId);
    }
}

void CopyAssistController::requestModel(const QString& tierId)
{
    const AsrModelTier* tier = AsrModelCatalog::tierById(tierId);
    if (tier == nullptr) {
        m_panel->setStatus(tr("Unknown model tier: %1").arg(tierId));
        m_panel->setAsrEnabled(false);
        return;
    }
    m_models->ensure(*tier); // emits alreadyPresent / finished / failed
}

bool CopyAssistController::promptRemoteConfig()
{
    const RemoteAsrConfig current = readRemoteConfig();

    QDialog dialog(m_settings);
    dialog.setWindowTitle(tr("Remote ASR Server"));
    auto* form = new QFormLayout(&dialog);

    auto* urlEdit = new QLineEdit(current.url, &dialog);
    urlEdit->setPlaceholderText(tr("http://host:8080/v1/audio/transcriptions"));
    urlEdit->setMinimumWidth(360);
    form->addRow(tr("Endpoint URL:"), urlEdit);

    auto* keyEdit = new QLineEdit(current.apiKey, &dialog);
    keyEdit->setEchoMode(QLineEdit::Password);
    keyEdit->setPlaceholderText(tr("optional (Bearer token)"));
    form->addRow(tr("API key:"), keyEdit);

    auto* modelEdit = new QLineEdit(current.model, &dialog);
    form->addRow(tr("Model:"), modelEdit);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (dialog.exec() != QDialog::Accepted || urlEdit->text().trimmed().isEmpty()) {
        return false;
    }

    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("AsrRemoteUrl"), urlEdit->text().trimmed());
    s.setValue(QStringLiteral("AsrRemoteApiKey"), keyEdit->text());
    s.setValue(QStringLiteral("AsrRemoteModel"), modelEdit->text().trimmed());
    s.setValue(QStringLiteral("AsrRemoteEnabled"), QStringLiteral("True"));
    s.save();
    return true;
}

QString CopyAssistController::promptCustomModel()
{
    // Default the picker to the last-picked file's folder, else the models cache
    // dir (where a manually-dropped ggml-*.bin would live).
    QString startDir = QFileInfo(m_customModelPath).absolutePath();
    if (startDir.isEmpty()) {
        startDir = AsrModelManager::defaultModelsDir();
    }
    return QFileDialog::getOpenFileName(
        m_settings, tr("Choose a Whisper model"), startDir,
        tr("Whisper models (*.bin *.gguf);;All files (*)"));
}

QString CopyAssistController::promptSherpaModel()
{
    const QString startDir =
        m_sherpaModelDir.isEmpty()
            ? AsrModelManager::defaultModelsDir()
            : QFileInfo(m_sherpaModelDir).absolutePath();
    return QFileDialog::getExistingDirectory(
        m_settings, tr("Choose a sherpa-onnx model folder"), startDir);
}

void CopyAssistController::promptVadModel()
{
    const QString start =
        m_settings->vadModelPath().isEmpty()
            ? AsrModelManager::defaultModelsDir()
            : QFileInfo(m_settings->vadModelPath()).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        m_settings, tr("Choose a Silero VAD model"), start,
        tr("ONNX models (*.onnx);;All files (*)"));
    if (path.isEmpty()) {
        // Cancelled with no prior model: turn the toggle back off.
        if (m_settings->vadModelPath().isEmpty()) {
            m_settings->setUseSileroVad(false);
        }
        return;
    }
    m_settings->setVadModelPath(path);
    AppSettings::instance().setValue(QStringLiteral("AsrVadModelPath"), path);
    AppSettings::instance().save();
    rebuildEngine();
}

AsrModelTier CopyAssistController::sileroVadTier()
{
    // The default learned VAD: Silero v5 ONNX (MIT), ~2 MB, from Hugging Face.
    AsrModelTier tier;
    tier.id = QStringLiteral("silero-vad");
    tier.displayName = QStringLiteral("Silero VAD");
    tier.fileName = QStringLiteral("silero_vad.onnx");
    tier.sizeBytes = 2243022;
    tier.sha256 = QStringLiteral("a4a068cd6cf1ea8355b84327595838ca748ec29a25bc91fc82e6c299ccdc5808");
    tier.sources = {
        QStringLiteral("https://huggingface.co/onnx-community/silero-vad/resolve/main/onnx/model.onnx?download=true"),
        // Release-asset mirror (upload alongside the whisper tiers on asr-models-v1);
        // SHA-verified, so it can't diverge from upstream undetected.
        QStringLiteral("https://github.com/aethersdr/AetherSDR/releases/download/asr-models-v1/silero_vad.onnx")};
    return tier;
}

void CopyAssistController::ensureVadModel()
{
    // A user-picked custom model that still exists wins; otherwise fetch (or
    // reuse the cached) default Silero model — no file hunting.
    const QString custom = m_settings->vadModelPath();
    if (!custom.isEmpty() && QFileInfo::exists(custom)
        && custom != m_vadModels->modelPath(sileroVadTier())) {
        rebuildEngine();
        return;
    }
    m_panel->setStatus(tr("Preparing Silero VAD model…"));
    m_vadModels->ensure(sileroVadTier()); // alreadyPresent / finished → onVadModelReady
}

void CopyAssistController::onVadModelReady(const QString& path)
{
    m_settings->setVadModelPath(path);
    AppSettings::instance().setValue(QStringLiteral("AsrVadModelPath"), path);
    AppSettings::instance().save();
    rebuildEngine();
}

AsrModelTier CopyAssistController::speakerEmbedderTier()
{
    // Default speaker-embedding model: WeSpeaker ECAPA-TDNN-512 ONNX (Apache-2.0),
    // ~24 MB, VoxCeleb2, 192-dim embeddings — from Hugging Face.
    AsrModelTier tier;
    tier.id = QStringLiteral("wespeaker-ecapa");
    tier.displayName = QStringLiteral("WeSpeaker ECAPA-TDNN");
    tier.fileName = QStringLiteral("wespeaker_ecapa512.onnx");
    tier.sizeBytes = 24861931;
    tier.sha256 = QStringLiteral("d71b85d9b48058ef68004f04f1b78acebefb9dfcf542e19b976a12a5ad1f10b0");
    tier.sources = {
        QStringLiteral("https://huggingface.co/Wespeaker/wespeaker-ecapa-tdnn512-LM/resolve/main/"
                       "voxceleb_ECAPA512_LM.onnx?download=true"),
        // Release-asset mirror (upload to asr-models-v1); SHA-verified fallback.
        QStringLiteral("https://github.com/aethersdr/AetherSDR/releases/download/asr-models-v1/"
                       "wespeaker_ecapa512.onnx")};
    return tier;
}

void CopyAssistController::ensureSpeakerModel()
{
    const QString custom = m_settings->speakerModelPath();
    if (!custom.isEmpty() && QFileInfo::exists(custom)
        && custom != m_speakerModels->modelPath(speakerEmbedderTier())) {
        rebuildEngine();
        return;
    }
    m_panel->setStatus(tr("Preparing speaker model…"));
    m_speakerModels->ensure(speakerEmbedderTier());
}

void CopyAssistController::onSpeakerModelReady(const QString& path)
{
    m_settings->setSpeakerModelPath(path);
    AppSettings::instance().setValue(QStringLiteral("AsrSpeakerModelPath"), path);
    AppSettings::instance().save();
    rebuildEngine();
}

void CopyAssistController::promptSpeakerModel()
{
    const QString start =
        m_settings->speakerModelPath().isEmpty()
            ? AsrModelManager::defaultModelsDir()
            : QFileInfo(m_settings->speakerModelPath()).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        m_settings, tr("Choose a speaker-embedding model"), start,
        tr("ONNX models (*.onnx);;All files (*)"));
    if (path.isEmpty()) {
        if (m_settings->speakerModelPath().isEmpty()) {
            m_settings->setLabelSpeakers(false);
        }
        return;
    }
    m_settings->setSpeakerModelPath(path);
    AppSettings::instance().setValue(QStringLiteral("AsrSpeakerModelPath"), path);
    AppSettings::instance().save();
    rebuildEngine();
}

void CopyAssistController::rebuildEngine()
{
    // The VAD is constructed in the worker's init(), so switching it requires a
    // fresh engine (same as a GPU change).
    m_tap->setEnabled(false);
    buildEngine();
    if (m_enabled) {
        beginEnable();
    }
}

void CopyAssistController::promptLogFile()
{
    const QString current = m_settings->logFilePath();
    const QString startDir =
        current.isEmpty()
            ? QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                  .filePath(QStringLiteral("aethersdr-transcript.txt"))
            : current;
    // We append (never truncate), so suppress the "replace existing file?" prompt.
    const QString path = QFileDialog::getSaveFileName(
        m_settings, tr("Save transcript to"), startDir,
        tr("Text files (*.txt);;All files (*)"), nullptr,
        QFileDialog::DontConfirmOverwrite);
    if (path.isEmpty()) {
        // Cancelled: if logging was just turned on without a file, turn it back off.
        if (m_settings->logFilePath().isEmpty()) {
            m_settings->setLogToFile(false);
        }
        return;
    }
    m_settings->setLogFilePath(path);
    AppSettings::instance().setValue(QStringLiteral("AsrLogFilePath"), path);
    AppSettings::instance().save();
}

bool CopyAssistController::appendLogRaw(const QString& text)
{
    // Per-day file derived from the user's base name; open/close each write so
    // the file survives external rotation and is always flushed. Callers ensure
    // logging is on with a non-empty base path.
    QFile file(datedLogPath(m_settings->logFilePath()));
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        m_panel->setStatus(tr("Transcript log write failed: %1").arg(file.errorString()));
        return false;
    }
    QTextStream out(&file);
    out << text;
    return true;
}

void CopyAssistController::writeFreqMarkerIfNeeded()
{
    if (!m_settings->logToFile()) {
        return;
    }
    const QString base = m_settings->logFilePath();
    if (base.isEmpty() || m_currentFreqMhz <= 0.0) {
        return;
    }
    // Key the marker to (today's file × frequency) so it's written once per
    // frequency per day-file — i.e. on start, on a real retune, and at the top of
    // a rolled-over day file — but never duplicated for unchanged context.
    const QString freq = QString::number(m_currentFreqMhz, 'f', 6);
    const QString key = datedLogPath(base) + QLatin1Char('|') + freq;
    if (key == m_lastFreqMarkerKey) {
        return;
    }
    const QString line = QLatin1Char('\n')
        + QDateTime::currentDateTime().toString(Qt::ISODate)
        + QStringLiteral("\t=== ") + freq + QStringLiteral(" MHz ===\n");
    if (appendLogRaw(line)) {
        m_lastFreqMarkerKey = key;
    }
}

void CopyAssistController::appendToLogFile(const QString& text)
{
    if (!m_settings->logToFile()) {
        return;
    }
    const QString base = m_settings->logFilePath();
    const QString trimmed = text.trimmed();
    if (base.isEmpty() || trimmed.isEmpty()) {
        return;
    }
    writeFreqMarkerIfNeeded(); // ensure the current frequency heads this file
    appendLogRaw(QDateTime::currentDateTime().toString(Qt::ISODate)
                 + QLatin1Char('\t') + trimmed + QLatin1Char('\n'));
}

} // namespace AetherSDR
