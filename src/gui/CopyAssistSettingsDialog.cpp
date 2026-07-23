#include "CopyAssistSettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

namespace AetherSDR {

CopyAssistSettingsDialog::CopyAssistSettingsDialog(QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("CopyAssistSettingsDialog"));
    setWindowTitle(tr("Copy Assist Settings"));
    setModal(false); // modeless: never blocks the main window
    // Float above the app as a tool window (out of the taskbar), and let the
    // window close button just hide it — the panel's ⚙ toggles it back.
    setWindowFlags(windowFlags() | Qt::Tool);

    auto* root = new QVBoxLayout(this);
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
    connect(m_gpu, &QComboBox::currentIndexChanged, this,
            [this](int) { emit gpuChanged(currentGpu()); });
    m_gpuLabel = new QLabel(tr("Compute:"), this);
    form->addRow(m_gpuLabel, m_gpu);
    // Hidden until the controller reports a GPU exists (CPU-only hosts show no
    // device picker at all).
    m_gpuLabel->hide();
    m_gpu->hide();

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

void CopyAssistSettingsDialog::setCurrentGpu(int index)
{
    const int idx = m_gpu->findData(index);
    if (idx >= 0) {
        m_gpu->setCurrentIndex(idx);
    }
}

int CopyAssistSettingsDialog::currentGpu() const
{
    return m_gpu->currentData().toInt();
}

void CopyAssistSettingsDialog::setGpuSelectorVisible(bool on)
{
    m_gpuLabel->setVisible(on);
    m_gpu->setVisible(on);
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

} // namespace AetherSDR
