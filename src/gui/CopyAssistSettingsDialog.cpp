#include "CopyAssistSettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

namespace AetherSDR {

CopyAssistSettingsDialog::CopyAssistSettingsDialog(QWidget* parent)
    // Tool window: modeless helper that floats above the app and stays out of
    // the taskbar, matching the pre-PersistentDialog behavior (#4414).
    : PersistentDialog(tr("Copy Assist Settings"),
                       QStringLiteral("CopyAssistSettingsDialogGeometry"),
                       parent, /*toolWindow=*/true)
{
    setObjectName(QStringLiteral("CopyAssistSettingsDialog"));

    auto* root = new QVBoxLayout(bodyWidget());
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);

    m_tier = new QComboBox(this);
    m_tier->setObjectName(QStringLiteral("CopyAssistModelCombo"));
    m_tier->setAccessibleName(tr("Copy Assist model"));
    m_tier->setToolTip(tr("Speech-recognition model (larger = more accurate, slower)"));
    m_tier->setMinimumWidth(240);
    connect(m_tier, &QComboBox::currentIndexChanged, this,
            [this](int) { emit tierChanged(currentTier()); });
    form->addRow(tr("Model:"), m_tier);

    m_gpu = new QComboBox(this);
    m_gpu->setObjectName(QStringLiteral("CopyAssistGpuCombo"));
    m_gpu->setAccessibleName(tr("Copy Assist compute device"));
    m_gpu->setToolTip(tr("Which device runs the model (a GPU, or CPU)"));
    m_gpu->addItem(tr("Detecting…"), kGpuDiscoveryPending);
    connect(m_gpu, &QComboBox::currentIndexChanged, this,
            [this](int) { emit gpuChanged(currentGpu()); });
    m_gpuLabel = new QLabel(tr("Compute:"), this);
    form->addRow(m_gpuLabel, m_gpu);
    // Device discovery can initialize Metal and compile its embedded shader
    // library. Keep the row visible but inactive while that work runs off the
    // GUI thread; CPU-only hosts hide it once discovery finishes.
    setGpuSelectorEnabled(false);

    m_language = new QComboBox(this);
    m_language->setObjectName(QStringLiteral("CopyAssistLanguageCombo"));
    m_language->setAccessibleName(tr("Copy Assist language"));
    m_language->setToolTip(tr("Spoken language to transcribe (multilingual models only)"));
    connect(m_language, &QComboBox::currentIndexChanged, this,
            [this](int) { emit languageChanged(currentLanguage()); });
    // Explicit label (kept so the whole row can hide together) — the language
    // selector only applies to the whisper/remote backends; sherpa-onnx picks
    // its language from the loaded model, so the controller hides this row then.
    m_languageLabel = new QLabel(tr("Language:"), this);
    m_languageLabel->setObjectName(QStringLiteral("CopyAssistLanguageLabel"));
    form->addRow(m_languageLabel, m_language);

    // Transcript-to-file logging. The checkbox is the master switch; the path row
    // (populated by the controller's file picker) enables with it.
    m_logToFile = new QCheckBox(tr("Save transcript to a file"), this);
    m_logToFile->setToolTip(tr("Append each finished utterance to a per-day text file"));
    connect(m_logToFile, &QCheckBox::toggled, this, [this](bool on) {
        m_logPath->setEnabled(on);
        m_logBrowse->setEnabled(on);
        emit logToFileToggled(on);
    });
    form->addRow(m_logToFile);

    auto* fileRow = new QHBoxLayout;
    m_logPath = new QLineEdit(this);
    m_logPath->setObjectName(QStringLiteral("CopyAssistLogPath"));
    m_logPath->setReadOnly(true);
    m_logPath->setPlaceholderText(tr("(no file chosen)"));
    m_logPath->setEnabled(false);
    m_logBrowse = new QPushButton(tr("Browse…"), this);
    m_logBrowse->setEnabled(false);
    connect(m_logBrowse, &QPushButton::clicked, this,
            &CopyAssistSettingsDialog::browseLogFileRequested);
    fileRow->addWidget(m_logPath, 1);
    fileRow->addWidget(m_logBrowse);
    form->addRow(tr("File:"), fileRow);

    // Clarify the per-day naming so the chosen name isn't the literal file.
    auto* logHint = new QLabel(tr("A per-day date is appended, e.g. name-2026-07-21.txt"), this);
    logHint->setEnabled(false); // dimmed, informational
    form->addRow(QString(), logHint);

    // Learned Silero VAD (ONNX) vs. the built-in energy VAD.
    m_useSilero = new QCheckBox(tr("Use Silero VAD (ONNX)"), this);
    m_useSilero->setToolTip(tr("Neural voice-activity detection — more robust in HF noise "
                               "than the energy threshold"));
    connect(m_useSilero, &QCheckBox::toggled, this, [this](bool on) {
        m_vadPath->setEnabled(on);
        m_vadBrowse->setEnabled(on);
        emit useSileroVadToggled(on);
    });
    form->addRow(m_useSilero);

    auto* vadRow = new QHBoxLayout;
    m_vadPath = new QLineEdit(this);
    m_vadPath->setObjectName(QStringLiteral("CopyAssistVadPath"));
    m_vadPath->setReadOnly(true);
    m_vadPath->setPlaceholderText(tr("(energy VAD)"));
    m_vadPath->setEnabled(false);
    m_vadBrowse = new QPushButton(tr("Browse…"), this);
    m_vadBrowse->setEnabled(false);
    connect(m_vadBrowse, &QPushButton::clicked, this,
            &CopyAssistSettingsDialog::browseVadModelRequested);
    vadRow->addWidget(m_vadPath, 1);
    vadRow->addWidget(m_vadBrowse);
    form->addRow(tr("VAD model:"), vadRow);

    // Per-utterance speaker labeling (A/B/C…) via a speaker-embedding model.
    m_labelSpeakers = new QCheckBox(tr("Label speakers (A/B/C…)"), this);
    m_labelSpeakers->setToolTip(tr("Tag each utterance with a speaker label using an "
                                   "ONNX speaker-embedding model"));
    connect(m_labelSpeakers, &QCheckBox::toggled, this, [this](bool on) {
        m_spkPath->setEnabled(on);
        m_spkBrowse->setEnabled(on);
        m_spkThreshold->setEnabled(on);
        emit labelSpeakersToggled(on);
    });
    form->addRow(m_labelSpeakers);

    auto* spkRow = new QHBoxLayout;
    m_spkPath = new QLineEdit(this);
    m_spkPath->setObjectName(QStringLiteral("CopyAssistSpeakerPath"));
    m_spkPath->setReadOnly(true);
    m_spkPath->setPlaceholderText(tr("(off)"));
    m_spkPath->setEnabled(false);
    m_spkBrowse = new QPushButton(tr("Browse…"), this);
    m_spkBrowse->setEnabled(false);
    connect(m_spkBrowse, &QPushButton::clicked, this,
            &CopyAssistSettingsDialog::browseSpeakerModelRequested);
    spkRow->addWidget(m_spkPath, 1);
    spkRow->addWidget(m_spkBrowse);
    form->addRow(tr("Speaker model:"), spkRow);

    // Cosine match threshold (0.00–1.00). Higher = stricter (more, finer splits);
    // lower = looser (fewer, merged speakers). Applied live.
    auto* thrRow = new QHBoxLayout;
    m_spkThreshold = new QSlider(Qt::Horizontal, this);
    m_spkThreshold->setRange(0, 100);
    m_spkThreshold->setValue(50);
    m_spkThreshold->setEnabled(false);
    m_spkThreshold->setToolTip(tr("Cosine similarity above which two utterances are "
                                  "the same speaker"));
    m_spkThresholdValue = new QLabel(QStringLiteral("0.50"), this);
    m_spkThresholdValue->setMinimumWidth(36);
    m_spkThresholdValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(m_spkThreshold, &QSlider::valueChanged, this, [this](int v) {
        m_spkThresholdValue->setText(QStringLiteral("%1").arg(v / 100.0, 0, 'f', 2));
        emit speakerThresholdChanged(v);
    });
    thrRow->addWidget(m_spkThreshold, 1);
    thrRow->addWidget(m_spkThresholdValue);
    form->addRow(tr("Match threshold:"), thrRow);

    // Boundary-word recovery / segment overlap (RFC #4821): carry a little
    // trailing audio across a forced segment cut so a word split at the boundary
    // isn't lost. 0 = off (default); opt-in. Applied live, no engine rebuild.
    auto* ovRow = new QHBoxLayout;
    m_overlap = new QSlider(Qt::Horizontal, this);
    m_overlap->setObjectName(QStringLiteral("CopyAssistOverlapSlider"));
    m_overlap->setAccessibleName(tr("Copy Assist boundary overlap"));
    m_overlap->setRange(0, 2000);      // ms of trailing audio carried forward
    m_overlap->setSingleStep(250);
    m_overlap->setPageStep(250);
    m_overlap->setTickInterval(250);
    m_overlap->setTickPosition(QSlider::TicksBelow);
    m_overlap->setValue(0);
    m_overlap->setToolTip(tr("Carry this much trailing audio across a forced segment "
                             "cut to recover a word split at the boundary (0 = off)"));
    m_overlapValue = new QLabel(tr("Off"), this);
    m_overlapValue->setAccessibleName(tr("Boundary overlap value"));
    m_overlapValue->setMinimumWidth(48);
    m_overlapValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    connect(m_overlap, &QSlider::valueChanged, this, [this](int v) {
        m_overlapValue->setText(v > 0 ? tr("%1 ms").arg(v) : tr("Off"));
        emit boundaryOverlapChanged(v);
    });
    ovRow->addWidget(m_overlap, 1);
    ovRow->addWidget(m_overlapValue);
    form->addRow(tr("Boundary overlap:"), ovRow);

    root->addLayout(form);
    root->addStretch(1); // headroom for further options added here later
}

void CopyAssistSettingsDialog::addTier(const QString& id, const QString& label)
{
    m_tier->addItem(label, id);
}

void CopyAssistSettingsDialog::setCurrentTier(const QString& id)
{
    const int idx = m_tier->findData(id);
    if (idx >= 0) {
        m_tier->setCurrentIndex(idx);
    }
}

void CopyAssistSettingsDialog::setTierLabel(const QString& id, const QString& label)
{
    const int idx = m_tier->findData(id);
    if (idx >= 0) {
        m_tier->setItemText(idx, label);
    }
}

QString CopyAssistSettingsDialog::currentTier() const
{
    return m_tier->currentData().toString();
}

void CopyAssistSettingsDialog::addGpuDevice(int index, const QString& name)
{
    m_gpu->addItem(name, index);
}

void CopyAssistSettingsDialog::clearGpuDevices()
{
    m_gpu->clear();
}

void CopyAssistSettingsDialog::setCurrentGpu(int index)
{
    const int idx = m_gpu->findData(index);
    if (idx >= 0) {
        m_gpu->setCurrentIndex(idx);
    }
}

int CopyAssistSettingsDialog::currentGpu() const
{
    bool ok = false;
    const int index = m_gpu->currentData().toInt(&ok);
    return ok ? index : kGpuDiscoveryPending;
}

void CopyAssistSettingsDialog::setGpuSelectorVisible(bool on)
{
    m_gpuLabel->setVisible(on);
    m_gpu->setVisible(on);
}

void CopyAssistSettingsDialog::setGpuSelectorEnabled(bool on)
{
    m_gpuLabel->setEnabled(on);
    m_gpu->setEnabled(on);
}

void CopyAssistSettingsDialog::addLanguage(const QString& code, const QString& name)
{
    m_language->addItem(name, code);
}

void CopyAssistSettingsDialog::setCurrentLanguage(const QString& code)
{
    const int idx = m_language->findData(code);
    if (idx >= 0) {
        m_language->setCurrentIndex(idx);
    }
}

QString CopyAssistSettingsDialog::currentLanguage() const
{
    return m_language->currentData().toString();
}

void CopyAssistSettingsDialog::setLanguageSelectorVisible(bool on)
{
    m_languageLabel->setVisible(on);
    m_language->setVisible(on);
}

void CopyAssistSettingsDialog::setLogToFile(bool on)
{
    m_logToFile->setChecked(on); // fires toggled → enables the path row + emits
}

bool CopyAssistSettingsDialog::logToFile() const
{
    return m_logToFile->isChecked();
}

void CopyAssistSettingsDialog::setLogFilePath(const QString& path)
{
    m_logPath->setText(path);
    m_logPath->setToolTip(path);
}

QString CopyAssistSettingsDialog::logFilePath() const
{
    return m_logPath->text();
}

void CopyAssistSettingsDialog::setUseSileroVad(bool on)
{
    m_useSilero->setChecked(on); // fires toggled → enables the path row + emits
}

bool CopyAssistSettingsDialog::useSileroVad() const
{
    return m_useSilero->isChecked();
}

void CopyAssistSettingsDialog::setVadModelPath(const QString& path)
{
    m_vadPath->setText(path);
    m_vadPath->setToolTip(path);
}

QString CopyAssistSettingsDialog::vadModelPath() const
{
    return m_vadPath->text();
}

void CopyAssistSettingsDialog::setLabelSpeakers(bool on)
{
    m_labelSpeakers->setChecked(on);
}

bool CopyAssistSettingsDialog::labelSpeakers() const
{
    return m_labelSpeakers->isChecked();
}

void CopyAssistSettingsDialog::setSpeakerModelPath(const QString& path)
{
    m_spkPath->setText(path);
    m_spkPath->setToolTip(path);
}

QString CopyAssistSettingsDialog::speakerModelPath() const
{
    return m_spkPath->text();
}

void CopyAssistSettingsDialog::setSpeakerThreshold(int percent)
{
    m_spkThreshold->setValue(percent); // fires valueChanged → updates label + emits
}

int CopyAssistSettingsDialog::speakerThreshold() const
{
    return m_spkThreshold->value();
}

void CopyAssistSettingsDialog::setBoundaryOverlapMs(int ms)
{
    // Programmatic set: don't echo back out as an operator edit. Without this,
    // any post-construction call (settings reload, reset-to-defaults, a future
    // profile switch) would round-trip straight into saveInt("AsrBoundaryOverlapMs")
    // and re-write what it just read. The label — normally driven by valueChanged
    // — is updated explicitly here since that signal is suppressed.
    const QSignalBlocker block(m_overlap);
    m_overlap->setValue(ms);
    m_overlapValue->setText(ms > 0 ? tr("%1 ms").arg(ms) : tr("Off"));
}

int CopyAssistSettingsDialog::boundaryOverlapMs() const
{
    return m_overlap->value();
}

} // namespace AetherSDR
