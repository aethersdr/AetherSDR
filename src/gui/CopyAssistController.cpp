#include "CopyAssistController.h"

#include "CopyAssistPanel.h"
#include "CopyAssistSettings.h"
#include "CopyAssistSettingsDialog.h"

#include "asr/AsrEngine.h"
#include "asr/AsrModelCatalog.h"
#include "asr/AsrModelManager.h"
#include "asr/RemoteAsrBackend.h"
#include "asr/SherpaOnnxBackend.h"
#include "asr/WhisperAsrBackend.h"
#include "core/LogManager.h"
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
#include <QFutureWatcher>
#include <QLineEdit>
#include <QObject>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <exception>

namespace {

constexpr const char* kRemoteTierId = "remote";
constexpr const char* kCustomTierId = "custom";
constexpr const char* kSherpaTierId = "sherpa";
constexpr int kGpuDiscoveryTimeoutMs = 30000;

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
    // Persists into the nested Copy Assist config object (setValue saves).
    AetherSDR::CopyAssistSettings::setValue(QString::fromLatin1(key), QString::number(value));
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
    // This helper lives in the file's anonymous namespace (outside
    // AetherSDR::), so CopyAssistSettings must be fully qualified here.
    using AetherSDR::CopyAssistSettings::value;
    AetherSDR::RemoteAsrConfig cfg;
    cfg.url = value(QStringLiteral("AsrRemoteUrl"), QString()).toString();
    cfg.apiKey = value(QStringLiteral("AsrRemoteApiKey"), QString()).toString();
    cfg.model = value(QStringLiteral("AsrRemoteModel"), QStringLiteral("whisper-1")).toString();
    // Share the Copy Assist language selection with the remote endpoint. "auto"
    // is left empty so an OpenAI-compatible server does its own detection rather
    // than being handed a non-standard language value.
    const QString lang = value(QStringLiteral("AsrLanguage"), QStringLiteral("en")).toString();
    cfg.language = (lang == QStringLiteral("auto")) ? QString() : lang;
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
        CopyAssistSettings::value(QStringLiteral("AsrCustomModelPath"), QString()).toString();
    if (!m_customModelPath.isEmpty()) {
        m_settings->setTierLabel(QString::fromLatin1(kCustomTierId),
                                 tr("Custom: %1").arg(QFileInfo(m_customModelPath).fileName()));
    }
    m_sherpaModelDir =
        CopyAssistSettings::value(QStringLiteral("AsrSherpaModelDir"), QString()).toString();
    if (!m_sherpaModelDir.isEmpty()) {
        m_settings->setTierLabel(QString::fromLatin1(kSherpaTierId),
                                 tr("Sherpa: %1").arg(QDir(m_sherpaModelDir).dirName()));
    }

    // Initial backend: remote if previously configured+enabled, otherwise use
    // the platform default until the asynchronous GPU probe completes. On
    // macOS the first ggml device query can compile the embedded Metal shader
    // library and take many seconds, so it must never run in this GUI-thread
    // constructor.
    const bool remoteConfigured =
        CopyAssistSettings::value(QStringLiteral("AsrRemoteEnabled"), QStringLiteral("False"))
                .toString() == QStringLiteral("True")
        && !readRemoteConfig().url.isEmpty();
    if (remoteConfigured) {
        m_backend = AsrBackendKind::Remote;
        m_tierId = QString::fromLatin1(kRemoteTierId);
    } else {
        m_backend = AsrBackendKind::Whisper;
        m_tierId = AsrModelCatalog::defaultTierId();
        m_useGpuDefaultIfAvailable = true;
    }
    const QString savedGpu =
        CopyAssistSettings::value(QStringLiteral("AsrGpuDevice"), QString()).toString();
    if (!savedGpu.isEmpty()) {
        m_gpuDevice = savedGpu.toInt(); // an explicit compute-device choice wins, unless it is known bad
        m_gpuDeviceExplicit = true;
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

    // Language selector — every language the whisper build supports.
    // Multilingual models honor it; English-only models ignore it. (The
    // remote/sherpa backends read the same stored value where relevant.)
    // NOTE: the "Auto-detect" option was removed — whisper's detection wasn't
    // reliable on Copy Assist's short VAD segments (it keys off ~30 s of audio).
    // The backend still handles "auto" (see WhisperAsrBackend::transcribe), left
    // dormant so re-adding it is a one-line change once detection is understood.
    const std::vector<AsrLanguage> languages = asrWhisperLanguages();
    for (const AsrLanguage& lang : languages) {
        m_settings->addLanguage(lang.code, lang.name);
    }
    const QString savedLang =
        CopyAssistSettings::value(QStringLiteral("AsrLanguage"), QStringLiteral("en")).toString();
    // Fall back to English for any value the model can't decode — mirrors the
    // GPU-device clamp above. This also migrates the retired "auto" sentinel and
    // any empty/stale code, keeping the dropdown and the engine in sync (a
    // lingering "auto" would otherwise still trigger detection).
    const QString effectiveLang = asrLanguageOrDefault(savedLang, languages);
    if (effectiveLang != savedLang) {
        CopyAssistSettings::setValue(QStringLiteral("AsrLanguage"), effectiveLang);
    }
    m_settings->setCurrentLanguage(effectiveLang);
    // The language selector only affects whisper/remote; sherpa-onnx takes its
    // language from the model, so hide it when sherpa is the active backend.
    m_settings->setLanguageSelectorVisible(m_backend != AsrBackendKind::SherpaOnnx);

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
        if (index == CopyAssistSettingsDialog::kGpuDiscoveryPending) {
            return;
        }
        m_gpuDevice = index;
        m_gpuDeviceExplicit = true;
        saveInt("AsrGpuDevice", index);
        if (m_backend != AsrBackendKind::Remote) {
            m_tap->setEnabled(false);
            buildEngine(); // rebuild the local engine on the chosen GPU
            if (m_enabled) {
                requestEnable();
            }
        }
    });
    connect(m_settings, &CopyAssistSettingsDialog::languageChanged, this, [this](const QString& code) {
        CopyAssistSettings::setValue(QStringLiteral("AsrLanguage"), code);
        // Language is fixed at backend construction (whisper factory arg /
        // remote config), so a change needs an engine rebuild — same as GPU.
        m_tap->setEnabled(false);
        buildEngine();
        if (m_enabled) {
            requestEnable();
        }
    });

    // Transcript-to-file logging. Restore the path first, then the checkbox, so
    // the toggle handler sees a path and doesn't prompt during restore.
    m_settings->setLogFilePath(
        CopyAssistSettings::value(QStringLiteral("AsrLogFilePath"), QString()).toString());
    m_settings->setLogToFile(
        CopyAssistSettings::value(QStringLiteral("AsrLogToFile"), QStringLiteral("False"))
            .toString() == QStringLiteral("True"));
    connect(m_settings, &CopyAssistSettingsDialog::logToFileToggled, this, [this](bool on) {
        CopyAssistSettings::setValue(QStringLiteral("AsrLogToFile"), on ? QStringLiteral("True") : QStringLiteral("False"));
        if (on && m_settings->logFilePath().isEmpty()) {
            promptLogFile(); // enabling with no file yet → ask for one
        }
    });
    connect(m_settings, &CopyAssistSettingsDialog::browseLogFileRequested, this,
            [this] { promptLogFile(); });

    // Learned Silero VAD. Restore path then checkbox (so the toggle handler sees
    // the path and doesn't prompt during restore).
    m_settings->setVadModelPath(
        CopyAssistSettings::value(QStringLiteral("AsrVadModelPath"), QString()).toString());
    m_settings->setUseSileroVad(
        CopyAssistSettings::value(QStringLiteral("AsrVadEnabled"), QStringLiteral("False"))
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
        CopyAssistSettings::setValue(QStringLiteral("AsrVadEnabled"), on ? QStringLiteral("True") : QStringLiteral("False"));
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
        if (!m_defaultSpeakerRequestPending) {
            return; // labeling switched off mid-download — stop repainting progress
        }
        m_panel->setStatus(total > 0
                               ? tr("Downloading speaker model… %1%").arg(static_cast<int>(got * 100 / total))
                               : tr("Downloading speaker model…"));
    });
    connect(m_speakerModels, &AsrModelManager::alreadyPresent, this,
            [this](const QString& path) { onSpeakerModelReady(path); });
    connect(m_speakerModels, &AsrModelManager::finished, this,
            [this](const QString& path) { onSpeakerModelReady(path); });
    connect(m_speakerModels, &AsrModelManager::failed, this, [this](const QString& err) {
        const bool stillWanted = m_defaultSpeakerRequestPending && m_settings->labelSpeakers();
        // Always clear the intent, including on the path that swallows the
        // error: leaving it set would make the next completion look current.
        m_defaultSpeakerRequestPending = false;
        if (!stillWanted) {
            // The operator already turned labeling off. Drop the error, but
            // don't strand the panel on the abandoned download's progress text.
            restoreListeningStatus();
            return;
        }
        m_panel->setStatus(tr("Speaker model download failed: %1").arg(err));
        m_settings->setLabelSpeakers(false);
    });
    m_settings->setSpeakerModelPath(
        CopyAssistSettings::value(QStringLiteral("AsrSpeakerModelPath"), QString()).toString());
    m_settings->setSpeakerThreshold(
        CopyAssistSettings::value(QStringLiteral("AsrSpeakerThreshold"), QStringLiteral("50"))
            .toString().toInt());
    m_settings->setLabelSpeakers(
        CopyAssistSettings::value(QStringLiteral("AsrSpeakerEnabled"), QStringLiteral("False"))
            .toString() == QStringLiteral("True"));
    connect(m_settings, &CopyAssistSettingsDialog::speakerThresholdChanged, this, [this](int pct) {
        saveInt("AsrSpeakerThreshold", pct);
        m_asr->setSpeakerThreshold(pct / 100.0f); // live, no engine rebuild
    });
    connect(m_settings, &CopyAssistSettingsDialog::labelSpeakersToggled, this, [this](bool on) {
        CopyAssistSettings::setValue(QStringLiteral("AsrSpeakerEnabled"), on ? QStringLiteral("True") : QStringLiteral("False"));
        if (!m_constructed) {
            return;
        }
        m_asr->setSpeakerLabelingEnabled(on);
        if (on) {
            ensureSpeakerModel();
        } else {
            m_defaultSpeakerRequestPending = false;
            if (!m_speakerLoad.isPending()) {
                if (m_enabled && m_asr->isReady()) {
                    m_tap->setEnabled(true);
                }
                // A "Preparing…"/"Downloading…" message may still be on screen
                // from the enable this cancels; a load still in flight restores
                // it from onSpeakerModelLoaded() instead.
                restoreListeningStatus();
            }
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
        m_gpuFallbackNotice.clear(); // the fallback reload never got a model
        m_panel->setStatus(tr("Model download failed: %1").arg(err));
        m_panel->setAsrEnabled(false);
    });

    // Live VAD tuning (reads m_asr at call time → survives engine rebuild).
    m_panel->setBufferMs(CopyAssistSettings::value(QStringLiteral("AsrDecodeBufferMs"), QStringLiteral("20000")).toString().toInt());
    m_panel->setSensitivity(CopyAssistSettings::value(QStringLiteral("AsrSensitivity"), QStringLiteral("80")).toString().toInt());
    m_panel->setSilenceMs(CopyAssistSettings::value(QStringLiteral("AsrSilenceMs"), QStringLiteral("300")).toString().toInt());
    m_panel->setFontPx(CopyAssistSettings::value(QStringLiteral("AsrFontPx"), QStringLiteral("13")).toString().toInt());
    m_panel->setNewlineOnSilence(
        CopyAssistSettings::value(QStringLiteral("AsrNewlineOnSilence"), QStringLiteral("False")).toString()
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
        CopyAssistSettings::setValue(QStringLiteral("AsrNewlineOnSilence"),
                    on ? QStringLiteral("True") : QStringLiteral("False"));
    });

    buildEngine();
    m_constructed = true; // subsequent VAD toggles may download/rebuild
    startGpuDiscovery();
}

CopyAssistController::~CopyAssistController() = default;

PersistentDialog* CopyAssistController::settingsDialog() const
{
    return m_settings;
}

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

void CopyAssistController::startGpuDiscovery()
{
    // asrGpuDevices() initializes ggml's backend registry. With the embedded
    // Metal source used by whisper.cpp that first call may synchronously invoke
    // Apple's shader compiler for several seconds. Keep the panel immediately
    // usable and deliver the result back on this controller's thread.
    auto* watcher = new QFutureWatcher<std::vector<AsrGpuDevice>>(this);
    connect(watcher, &QFutureWatcher<std::vector<AsrGpuDevice>>::finished,
            this, [this, watcher] {
                std::vector<AsrGpuDevice> devices;
                try {
                    devices = watcher->result();
                } catch (const std::exception& error) {
                    qCWarning(lcGui) << "ASR compute-device discovery failed:"
                                     << error.what();
                } catch (...) {
                    qCWarning(lcGui) << "ASR compute-device discovery failed";
                }
                if (m_gpuDiscoveryPending) {
                    applyGpuDevices(devices);
                }
                watcher->deleteLater();
            });
    watcher->setFuture(QtConcurrent::run([] { return asrGpuDevices(); }));

    QTimer::singleShot(kGpuDiscoveryTimeoutMs, this, [this] {
        if (!m_gpuDiscoveryPending) {
            return;
        }
        qCWarning(lcGui) << "ASR compute-device discovery timed out after"
                         << kGpuDiscoveryTimeoutMs << "ms; using CPU for this session";
        applyGpuDevices({});
    });
}

void CopyAssistController::reconcileAfterGpuFallback()
{
    // Reached from the event loop, so re-check every condition the caller saw:
    // between queueing and running, the operator may have picked another device,
    // switched to a backend that has no GPU of its own (remote/sherpa), or an
    // earlier fallback may already have reconciled this one. Without the backend
    // check a stale queued reconcile would stamp a GPU notice over the status of
    // a backend the GPU has nothing to do with.
    if (m_backend != AsrBackendKind::Whisper || m_gpuDevice < 0
        || !asrGpuDeviceFailed(m_gpuDevice)) {
        return;
    }

    // Reached queued from either signal: after a load-time fallback (the
    // backend already retried on CPU and is running) or after a decode-time
    // failure (the engine is still aimed at the latched device). Relabel the
    // device and move the resolution off it — applyGpuDevices() rebuilds the
    // engine when that changes the resolved device, which is what actually
    // retires a decode-poisoned engine — then say so in the panel.
    const int failedDevice = m_gpuDevice;
    for (AsrGpuDevice& d : m_gpuDevices) {
        if (d.index == failedDevice) {
            d.usable = false;
        }
    }
    const QString failedName = [&] {
        for (const AsrGpuDevice& d : m_gpuDevices) {
            if (d.index == failedDevice) {
                return d.name;
            }
        }
        return tr("The selected GPU");
    }();

    // A device known bad must be overridden even when explicitly chosen.
    m_gpuDeviceExplicit = false;
    applyGpuDevices(m_gpuDevices);

    const bool onCpu = m_gpuDevice < 0;
    const QString notice = onCpu
        ? tr("%1 is unavailable — running on CPU. Transcription will be slower.")
              .arg(failedName)
        : tr("%1 is unavailable — switched to another GPU.").arg(failedName);

    // applyGpuDevices() moved the resolution off the latched device, which
    // rebuilt the engine — model-less, tap off. Left there, the panel looks
    // live while the pipeline decodes nothing until the operator toggles
    // Enable. Reload so the fallback ends in a running engine; the notice is
    // parked for the rebuilt engine's ready() handler, which would otherwise
    // overwrite it with "Listening…" the moment the reload lands.
    if (m_enabled && m_backend == AsrBackendKind::Whisper) {
        m_gpuFallbackNotice = notice;
        beginEnable();
    } else {
        m_panel->setStatus(notice);
    }
}

void CopyAssistController::applyGpuDevices(std::vector<AsrGpuDevice> gpus)
{
    m_gpuDevices = std::move(gpus);
    const int previousDevice = m_gpuDevice;
    int resolvedDevice = previousDevice;

    // Adding or clearing combo items changes its current index. Suppress the
    // dialog's outward gpuChanged/tierChanged signals while discovery state is
    // applied so the controller, rather than incidental combo transitions,
    // decides whether the engine needs rebuilding.
    {
        const QSignalBlocker blocker(m_settings);
        m_settings->clearGpuDevices();

        if (!m_gpuDevices.empty()) {
            for (const AsrGpuDevice& gpu : m_gpuDevices) {
                // An unusable device stays selectable for diagnostics, but the
                // combo says why it is not the default — whether it failed the
                // capability probe or a load attempt this session.
                m_settings->addGpuDevice(gpu.index,
                                         gpu.usable ? gpu.name
                                                    : tr("%1 (unavailable)").arg(gpu.name));
            }
            m_settings->addGpuDevice(-1, tr("CPU")); // explicit force-CPU option

            const bool outOfRange = resolvedDevice != -1
                && (resolvedDevice < 0 || resolvedDevice >= static_cast<int>(m_gpuDevices.size()));
            const bool resolvedUnusable = [&] {
                for (const AsrGpuDevice& g : m_gpuDevices) {
                    if (g.index == resolvedDevice) {
                        return !g.usable;
                    }
                }
                return false;
            }();
            // Fall back to the first usable device (else CPU) when the choice
            // was never explicit, has gone out of range, or names a device that
            // cannot run the decode. A device the operator picked explicitly is
            // still overridden once it is known-bad — leaving it selected would
            // re-enter the failure on the next load.
            // Not persisted: an unusable device can become usable again after a
            // driver fix or reboot, and writing a computed value over the saved
            // preference would pin that computation permanently.
            if (!m_gpuDeviceExplicit || outOfRange || resolvedUnusable) {
                resolvedDevice = asrResolveDefaultGpuIndex(m_gpuDevices);
            }
            m_settings->setCurrentGpu(resolvedDevice);
            m_settings->setGpuSelectorVisible(true);
            m_settings->setGpuSelectorEnabled(true);
        } else {
            // A failed, timed-out, or CPU-only probe must not trigger the same
            // registry initialization again from Whisper's worker. Force CPU
            // for this session, but keep the saved GPU preference so a later
            // launch can retry discovery.
            resolvedDevice = -1;
            m_settings->setGpuSelectorEnabled(false);
            m_settings->setGpuSelectorVisible(false);
        }

        // Tier follows the device resolution (asrReconcileDefaultTier): the
        // GPU-host default is preserved only while the operator has not made
        // an explicit model choice AND the decode will actually run on a
        // usable GPU — and an earlier auto-raise is walked back to the base
        // default the moment resolution falls off the GPU. Without the
        // walk-back, the load-time fallback arm kept large-v3-turbo running
        // on CPU: the "backlog climbing, no text" symptom this PR opens with
        // (#4767 review). An explicitly chosen tier is never changed.
        const bool resolvedGpuUsable = resolvedDevice >= 0 && [&] {
            for (const AsrGpuDevice& g : m_gpuDevices) {
                if (g.index == resolvedDevice) {
                    return g.usable;
                }
            }
            return false;
        }();
        const AsrTierResolution tier = asrReconcileDefaultTier(
            m_tierId, m_useGpuDefaultIfAvailable, m_gpuDefaultTierActive,
            resolvedGpuUsable, QStringLiteral("large-v3-turbo"),
            AsrModelCatalog::defaultTierId());
        m_gpuDefaultTierActive = tier.gpuDefaultActive;
        if (tier.tierId != m_tierId) {
            m_tierId = tier.tierId;
            m_settings->setCurrentTier(m_tierId);
        }
    }

    m_gpuDevice = resolvedDevice;
    if (resolvedDevice != previousDevice && m_backend == AsrBackendKind::Whisper) {
        m_tap->setEnabled(false);
        buildEngine();
    }

    m_gpuDiscoveryPending = false;
    if (m_enableAfterGpuDiscovery && m_enabled) {
        m_enableAfterGpuDiscovery = false;
        requestEnable();
    }
}

void CopyAssistController::buildEngine()
{
    // Tear down any previous engine+tap (order: tap first — it references the
    // engine) and rebuild for the current backend.
    delete m_tap;
    m_tap = nullptr;
    delete m_asr;

    const QString language =
        CopyAssistSettings::value(QStringLiteral("AsrLanguage"), QStringLiteral("en"))
            .toString();
    // Optional learned (Silero) VAD — an .onnx path enables it in the worker;
    // empty (or the toggle off) keeps the built-in energy VAD.
    AsrSegmenter::Config segConfig;
    if (CopyAssistSettings::value(QStringLiteral("AsrVadEnabled"), QStringLiteral("False")).toString()
        == QStringLiteral("True")) {
        segConfig.vadModelPath =
            CopyAssistSettings::value(QStringLiteral("AsrVadModelPath"), QString()).toString().toStdString();
    }
    // Speaker embedding is loaded only after the new engine's signals are
    // connected below. This makes replacement deterministic: a GPU/VAD/backend
    // rebuild never relies on an init-time completion from a dying engine.
    segConfig.speakerThreshold =
        CopyAssistSettings::value(QStringLiteral("AsrSpeakerThreshold"), QStringLiteral("50"))
            .toString().toInt() / 100.0f;
    switch (m_backend) {
    case AsrBackendKind::Remote:
        m_asr = new AsrEngine(remoteAsrBackendFactory(readRemoteConfig()), segConfig, this);
        break;
    case AsrBackendKind::Whisper:
        m_asr = new AsrEngine(whisperAsrBackendFactory(language, m_gpuDevice),
                              segConfig, this);
        break;
    case AsrBackendKind::SherpaOnnx:
        m_asr = new AsrEngine(sherpaOnnxBackendFactory(), segConfig, this);
        break;
    }
    m_tap = new AsrAudioTap(m_audio, m_asr, this);

    connect(m_asr, &AsrEngine::ready, this, [this] {
        m_panel->setBusy(false);
        if (m_enabled && !m_speakerLoad.isPending()) {
            m_tap->setEnabled(true);
            if (!m_gpuFallbackNotice.isEmpty()) {
                // This ready() is the reload after a GPU fallback: keep the
                // explanation on screen instead of a bare "Listening…", so
                // the operator learns why the decode moved (and why it may
                // now be slower) rather than seeing business as usual.
                m_panel->setStatus(m_gpuFallbackNotice);
                m_gpuFallbackNotice.clear();
            } else {
                m_panel->setStatus(m_backend == AsrBackendKind::Remote ? tr("Listening (remote)…")
                                                                       : tr("Listening…"));
            }
            writeFreqMarkerIfNeeded(); // "on start": head the log with the frequency
        }
        // The model loaded, but the backend may have got there by falling back
        // to CPU after the chosen GPU failed. The latch is the only signal —
        // a successful fallback still reports ready() — so ask it, then make
        // the selectors tell the truth instead of naming a device that is not
        // running the decode (#4502).
        //
        // Queued, not direct: reconciling can change the resolved device, and
        // that rebuilds the engine — which deletes the AsrEngine whose ready()
        // emission this lambda is running inside. Deferring to the event loop
        // lets that emission unwind before its sender is destroyed.
        if (m_backend == AsrBackendKind::Whisper && m_gpuDevice >= 0
            && asrGpuDeviceFailed(m_gpuDevice)) {
            QMetaObject::invokeMethod(
                this, [this] { reconcileAfterGpuFallback(); }, Qt::QueuedConnection);
        }
    });
    connect(m_asr, &AsrEngine::loadFailed, this, [this](const QString& err) {
        m_panel->setBusy(false);
        m_gpuFallbackNotice.clear(); // failed reload: this message wins instead
        m_panel->setStatus(tr("Model load failed: %1").arg(err));
        m_panel->setAsrEnabled(false);
    });
    connect(m_asr, &AsrEngine::finalText, this,
            [this](const QString& text, float confidence, int speaker) {
                // Prefix a speaker label ([A], [B]…) when labeling is on.
                const QString labeled =
                    m_settings->labelSpeakers() && speaker >= 0
                        ? QStringLiteral("[%1] %2").arg(QChar(u'A' + speaker)).arg(text)
                        : text;
                m_panel->appendText(labeled, confidence);
                appendToLogFile(labeled);
            });
    connect(m_asr, &AsrEngine::error, this, [this](const QString& err) {
        m_panel->setStatus(err);
        // A decode-time GPU failure latches the device in
        // WhisperAsrBackend::transcribe(); the engine itself is still aimed at
        // it and would throw again on every utterance. Reconcile off the error
        // signal exactly as the ready() path does — queued, because the
        // reconcile rebuilds the engine this handler's sender belongs to. The
        // entry guard in reconcileAfterGpuFallback() makes repeats harmless.
        if (m_backend == AsrBackendKind::Whisper && m_gpuDevice >= 0
            && asrGpuDeviceFailed(m_gpuDevice)) {
            QMetaObject::invokeMethod(
                this, [this] { reconcileAfterGpuFallback(); }, Qt::QueuedConnection);
        }
    });
    connect(m_asr, &AsrEngine::backlogChanged, m_panel, &CopyAssistPanel::setBacklog);
    connect(m_asr, &AsrEngine::speakerModelLoaded, this,
            &CopyAssistController::onSpeakerModelLoaded);

    applyTuning();
    replaySpeakerConfiguration();
}

void CopyAssistController::applyTuning()
{
    m_asr->setDecodeBufferMs(CopyAssistSettings::value(QStringLiteral("AsrDecodeBufferMs"), QStringLiteral("20000")).toString().toInt());
    m_asr->setSpeechRms(sensitivityToRms(
        CopyAssistSettings::value(QStringLiteral("AsrSensitivity"), QStringLiteral("80")).toString().toInt()));
    m_asr->setSilenceDurationMs(CopyAssistSettings::value(QStringLiteral("AsrSilenceMs"), QStringLiteral("300")).toString().toInt());
}

void CopyAssistController::onEnableToggled(bool on)
{
    m_enabled = on;
    if (on) {
        m_lastFreqMarkerKey.clear(); // force a fresh start marker for this session
        requestEnable();
    } else {
        m_enableAfterGpuDiscovery = false;
        m_gpuFallbackNotice.clear(); // a pending fallback reload no longer matters
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
    m_useGpuDefaultIfAvailable = false;
    m_enableAfterGpuDiscovery = false;

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
        CopyAssistSettings::setValue(QStringLiteral("AsrCustomModelPath"), path);
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
        CopyAssistSettings::setValue(QStringLiteral("AsrSherpaModelDir"), dir);
        m_settings->setTierLabel(QString::fromLatin1(kSherpaTierId),
                                 tr("Sherpa: %1").arg(QDir(dir).dirName()));
        setBackend(AsrBackendKind::SherpaOnnx, tierId);
    } else {
        setBackend(backendForTier(tierId), tierId);
    }

    if (m_enabled) {
        m_tap->setEnabled(false);
        requestEnable();
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
    // The tier is now an explicit operator choice — even if it is the GPU-default
    // tier itself — so a later fallback must never walk it back. Cleared here
    // rather than at the top of onTierChanged() because the three prompting
    // branches (remote / custom / sherpa) can still be cancelled, and a cancel
    // leaves the auto-raised tier in place: clearing the flag early would strand
    // Large v3 Turbo on CPU after a fallback, the exact regression the flag exists
    // to prevent. setBackend() is reached only once the change is committed.
    m_gpuDefaultTierActive = false;

    // Language applies to whisper/remote only; sherpa-onnx picks it from the
    // model. Keep the selector's visibility in sync with the active backend.
    m_settings->setLanguageSelectorVisible(kind != AsrBackendKind::SherpaOnnx);

    // Leaving the remote backend clears the persisted auto-connect flag so the
    // next launch starts on the local engine.
    if (prev == AsrBackendKind::Remote && kind != AsrBackendKind::Remote) {
        CopyAssistSettings::setValue(QStringLiteral("AsrRemoteEnabled"), QStringLiteral("False"));
    }

    // Only a change of backend kind needs a fresh engine; switching models within
    // the same backend (e.g. base → small, or a custom file) reloads via
    // beginEnable() without tearing the engine down.
    if (prev != kind) {
        m_tap->setEnabled(false);
        buildEngine();
    }
}

void CopyAssistController::requestEnable()
{
    if (m_backend == AsrBackendKind::Whisper && m_gpuDiscoveryPending) {
        // Preserve the original compute device and model even if the operator
        // clicks Enable immediately. Discovery continues off the GUI thread
        // and activation resumes as soon as it finishes.
        m_enableAfterGpuDiscovery = true;
        m_panel->setBusy(true);
        m_panel->setStatus(tr("Detecting compute device…"));
        return;
    }

    m_enableAfterGpuDiscovery = false;
    m_useGpuDefaultIfAvailable = false;
    beginEnable();
}

void CopyAssistController::beginEnable()
{
    m_panel->setBusy(true);
    // Arm speaker labeling here rather than from buildEngine(): pressing Enable
    // is the operator action that justifies fetching the ~24 MB model when the
    // cache is missing it, whereas constructing an engine is not (#4737).
    // Idempotent — a model already loaded into this engine is a no-op, and a
    // download already in flight is left alone. Runs before the ASR-model
    // status lines below so the headline stays the whisper/remote load.
    if (m_settings->labelSpeakers()) {
        ensureSpeakerModel();
    }
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
            m_gpuFallbackNotice.clear(); // no reload will arrive to show it
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
            m_gpuFallbackNotice.clear(); // no reload will arrive to show it
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

    CopyAssistSettings::setValue(QStringLiteral("AsrRemoteUrl"), urlEdit->text().trimmed());
    CopyAssistSettings::setValue(QStringLiteral("AsrRemoteApiKey"), keyEdit->text());
    CopyAssistSettings::setValue(QStringLiteral("AsrRemoteModel"), modelEdit->text().trimmed());
    CopyAssistSettings::setValue(QStringLiteral("AsrRemoteEnabled"), QStringLiteral("True"));
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
    CopyAssistSettings::setValue(QStringLiteral("AsrVadModelPath"), path);
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
    CopyAssistSettings::setValue(QStringLiteral("AsrVadModelPath"), path);
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
        m_defaultSpeakerRequestPending = false;
        queueSpeakerModelLoad(custom);
        return;
    }
    const AsrModelTier tier = speakerEmbedderTier();
    const QString path = m_speakerModels->modelPath(tier);
    if (m_speakerLoad.isPending(path)) {
        m_panel->setStatus(tr("Preparing speaker model…"));
        return; // off/on while this worker load is in flight only changes intent
    }
    if (m_speakerLoad.isLoaded(path) && !m_speakerLoad.isPending()) {
        queueSpeakerModelLoad(path);
        return;
    }

    m_panel->setStatus(tr("Preparing speaker model…"));
    // ensure() emits failed synchronously while verifying/downloading. Treat a
    // second on/off/on as continued intent for the existing work instead.
    m_desiredSpeakerModelPath = path;
    m_defaultSpeakerRequestPending = true;
    if (!m_speakerModels->isBusy()) {
        m_speakerModels->ensure(tier);
    }
}

void CopyAssistController::onSpeakerModelReady(const QString& path)
{
    if (!m_defaultSpeakerRequestPending || path != m_desiredSpeakerModelPath) {
        return;
    }
    m_defaultSpeakerRequestPending = false;
    m_settings->setSpeakerModelPath(path);
    CopyAssistSettings::setValue(QStringLiteral("AsrSpeakerModelPath"), path);
    queueSpeakerModelLoad(path);
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
    CopyAssistSettings::setValue(QStringLiteral("AsrSpeakerModelPath"), path);
    m_defaultSpeakerRequestPending = false;
    queueSpeakerModelLoad(path);
}

void CopyAssistController::queueSpeakerModelLoad(const QString& path)
{
    if (path.isEmpty()) {
        return;
    }

    m_desiredSpeakerModelPath = path;
    m_asr->setSpeakerLabelingEnabled(m_settings->labelSpeakers());
    if (m_speakerLoad.isPending(path)) {
        m_panel->setStatus(tr("Preparing speaker model…"));
        return; // applies equally to default and custom off/on sequences
    }
    if (m_speakerLoad.isLoaded(path) && !m_speakerLoad.isPending()) {
        if (m_enabled && m_asr->isReady()) {
            m_tap->setEnabled(true);
        }
        return;
    }

    m_speakerLoad.begin(path);
    m_panel->setStatus(tr("Preparing speaker model…"));
    // Stop feeding the worker while it prepares. This is not a priority boost:
    // loadSpeakerModel() is queued BEHIND whatever audio is already backlogged
    // on that same thread, so on a deep backlog "Preparing speaker model…" can
    // sit for a while either way. What it avoids is piling up further decodes
    // that the operator's pending labeling choice may make moot. The tap
    // resumes from onSpeakerModelLoaded() on the controller thread.
    m_tap->setEnabled(false);
    m_asr->setSpeakerModelPath(path);
}

void CopyAssistController::onSpeakerModelLoaded(const QString& path, bool loaded)
{
    if (!m_speakerLoad.complete(path, loaded)) {
        return; // a superseded custom/default request is still queued behind it
    }

    // Latched rather than re-derived: setLabelSpeakers(false) below flips
    // labelSpeakers() as a side effect, so testing it again afterwards would
    // overwrite the failure message with "Listening…".
    bool reportedFailure = false;
    if (!loaded && m_settings->labelSpeakers()) {
        m_panel->setStatus(tr("Speaker model load failed."));
        m_settings->setLabelSpeakers(false);
        reportedFailure = true;
    }
    m_asr->setSpeakerLabelingEnabled(m_settings->labelSpeakers());
    if (m_enabled && m_asr->isReady()) {
        m_tap->setEnabled(true);
    }
    if (!reportedFailure) {
        restoreListeningStatus();
    }
}

void CopyAssistController::restoreListeningStatus()
{
    if (!m_enabled || !m_asr->isReady()) {
        return; // not listening — leave "Disabled"/"Loading model…" alone
    }
    m_panel->setStatus(m_backend == AsrBackendKind::Remote ? tr("Listening (remote)…")
                                                           : tr("Listening…"));
    // The ready() lambda skips its whole body while a speaker load is pending,
    // so this resume point owes the log the same "on start" frequency header it
    // would have written. writeFreqMarkerIfNeeded() dedups, so the ordinary
    // "already marked" case costs nothing.
    writeFreqMarkerIfNeeded();
}

void CopyAssistController::replaySpeakerConfiguration()
{
    // This state belongs to the engine instance, unlike a model-manager
    // download/verification request. A replacement may destroy an old worker
    // before its queued completion reaches us, so begin the new instance with a
    // clean per-engine state and explicitly replay the latest user intent.
    m_speakerLoad.resetForEngineReplacement();
    const bool labelSpeakers = m_settings->labelSpeakers();
    m_asr->setSpeakerLabelingEnabled(labelSpeakers);
    if (!labelSpeakers) {
        return;
    }

    const QString requested = m_desiredSpeakerModelPath.isEmpty()
                                  ? m_settings->speakerModelPath()
                                  : m_desiredSpeakerModelPath;
    if (!requested.isEmpty() && QFileInfo::exists(requested)
        && requested != m_speakerModels->modelPath(speakerEmbedderTier())) {
        queueSpeakerModelLoad(requested);
        return;
    }

    // Building an engine must never START a download. ensure() fetches ~24 MB
    // unprompted when the file is absent, and buildEngine() runs from the
    // constructor and from every GPU/language/VAD/tier change — so without this
    // guard a stale "labeling on" setting would pull the model at app launch
    // with Copy Assist switched off. Fetching stays operator-driven (the
    // labeling toggle); replay only re-arms what is already on disk.
    if (!QFileInfo::exists(m_speakerModels->modelPath(speakerEmbedderTier()))) {
        return;
    }

    // This also preserves a busy default download across replacement: the file
    // is absent while it downloads, so the guard above returns early, but
    // m_defaultSpeakerRequestPending survives resetForEngineReplacement() (it is
    // controller state, not per-engine) and its completion loads into this new
    // engine via onSpeakerModelReady().
    ensureSpeakerModel();
}

void CopyAssistController::rebuildEngine()
{
    // The VAD is constructed in the worker's init(), so switching it requires a
    // fresh engine (same as a GPU change).
    m_tap->setEnabled(false);
    buildEngine();
    if (m_enabled) {
        requestEnable();
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
    CopyAssistSettings::setValue(QStringLiteral("AsrLogFilePath"), path);
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
