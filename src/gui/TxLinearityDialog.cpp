#include "TxLinearityDialog.h"

#include "TxLinearitySpectrumView.h"
#include "core/ThemeManager.h"
#include "core/TxLinearitySettings.h"
#include "core/TxKeyingMarker.h"
#include "core/txlinearity/FlexDaxIqCapture.h"
#include "models/BandPlanManager.h"
#include "models/MeterModel.h"
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>

#include <cmath>

namespace AetherSDR {
namespace {

// Feedback level window, dBFS RMS on the DAX IQ stream.
//
// The lower bound is set by SNR, not by taste. To read an IMD3 product 40 dB
// below the tones with 30 dB of margin over the instrument floor, the tones
// themselves have to sit roughly 70 dB above it — which on a 16-bit-equivalent
// path means the RMS cannot be down in the -50s. The upper bound is set by
// headroom: a two-tone's envelope peak is 6 dB above its RMS, so an RMS above
// about -9 dBFS puts the peaks against the rail and the analyzer starts
// measuring the ADC's clipping instead of the amplifier's distortion.
constexpr double kLevelTooLowDbfs = -35.0;
constexpr double kLevelGoodLowDbfs = -30.0;
constexpr double kLevelGoodHighDbfs = -12.0;
constexpr double kLevelTooHighDbfs = -9.0;

QString refText(double perTone, double pep, bool trusted)
{
    if (!trusted) {
        return TxLinearityDialog::tr("below noise floor");
    }
    // BOTH conventions, both labelled, every time. A bare "-32 dB" is the
    // single most common way to publish a wrong IMD figure.
    return QStringLiteral("%1 dBc/tone   %2 dBc/PEP")
        .arg(QString::number(perTone, 'f', 1))
        .arg(QString::number(pep, 'f', 1));
}

}  // namespace

TxLinearityDialog::TxLinearityDialog(RadioModel* model, BandPlanManager* bandPlan,
                                     QWidget* parent)
    : PersistentDialog(QStringLiteral("TX Linearity Analyzer"),
                       QStringLiteral("TxLinearityDialogGeometry"), parent)
    , m_model(model)
    , m_bandPlan(bandPlan)
{
    theme::setContainer(this, QStringLiteral("dialog/txLinearity"));
    setMinimumSize(760, 640);
    resize(900, 780);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(9);

    // One read of the owned config object, then every control seeds from it
    // (Principle V). load() has already defaulted and range-clamped, so no
    // control below has to second-guess what came back.
    m_settings = TxLinearitySettings::load();

    buildSetupGroup(root);
    buildTestGroup(root);
    buildResultArea(root);
    wireController();
    refreshPanChoices();
    refreshRunEnabled();
}

TxLinearityDialog::~TxLinearityDialog()
{
    // The controller unkeys in its own destructor, but stop it explicitly first
    // so the capture stream is torn down while the DaxIqModel it talks to is
    // still alive.
    m_controller.stopRun(txlin::AbortReason::OperatorStop);
}

// ── Setup ───────────────────────────────────────────────────────────────────

void TxLinearityDialog::buildSetupGroup(QVBoxLayout* root)
{
    auto* group = new QGroupBox(tr("1 — Feedback path"), bodyWidget());
    auto* grid = new QGridLayout(group);
    grid->setSpacing(6);

    auto* intro = new QLabel(
        tr("Route attenuated TX energy into a receive-only port (RX A or XVTR) and "
           "tune a slice there to the transmit frequency, then point a panadapter at "
           "it. Never connect the transmitter to a receive port without attenuation."));
    intro->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(
        intro, "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
    grid->addWidget(intro, 0, 0, 1, 4);

    grid->addWidget(new QLabel(tr("DAX IQ channel")), 1, 0);
    m_daxChannel = new QComboBox(group);
    for (int ch = 1; ch <= DaxIqModel::NUM_CHANNELS; ++ch) {
        m_daxChannel->addItem(QString::number(ch), ch);
    }
    m_daxChannel->setAccessibleName(tr("DAX IQ channel"));
    m_daxChannel->setAccessibleDescription(
        tr("Which of the radio's four DAX IQ channels captures the feedback signal."));
    m_daxChannel->setCurrentIndex(m_settings.daxChannel - 1);
    grid->addWidget(m_daxChannel, 1, 1);

    grid->addWidget(new QLabel(tr("Feedback panadapter")), 1, 2);
    m_feedbackPan = new QComboBox(group);
    m_feedbackPan->setAccessibleName(tr("Feedback panadapter"));
    m_feedbackPan->setAccessibleDescription(
        tr("The panadapter watching the receive-only port. The DAX IQ window follows "
           "this panadapter's centre frequency."));
    grid->addWidget(m_feedbackPan, 1, 3);

    grid->addWidget(new QLabel(tr("Feedback level")), 2, 0);
    m_levelBar = new QProgressBar(group);
    m_levelBar->setRange(-60, 0);
    m_levelBar->setValue(-60);
    m_levelBar->setFormat(tr("%v dBFS"));
    m_levelBar->setAccessibleName(tr("Feedback level"));
    m_levelBar->setAccessibleDescription(
        tr("RMS level of the captured feedback signal. Target minus 30 to minus 12 dBFS."));
    grid->addWidget(m_levelBar, 2, 1, 1, 2);

    m_levelVerdict = new QLabel(tr("No signal"), group);
    m_levelVerdict->setAccessibleName(tr("Feedback level verdict"));
    grid->addWidget(m_levelVerdict, 2, 3);

    auto* levelHelp = new QLabel(
        tr("Adjust the external attenuator until the level sits in the target band. "
           "Too low and the distortion products disappear into the receiver's noise; "
           "too high and the analyzer measures the receiver clipping rather than the "
           "amplifier."));
    levelHelp->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(
        levelHelp, "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
    grid->addWidget(levelHelp, 3, 0, 1, 4);

    m_dummyLoadConfirm = new QCheckBox(
        tr("I confirm a dummy load is connected, or the frequency is verified clear"), group);
    m_dummyLoadConfirm->setAccessibleName(tr("Dummy load confirmation"));
    m_dummyLoadConfirm->setAccessibleDescription(
        tr("Required before every test transmission. This confirmation is never "
           "remembered between runs or between sessions."));
    grid->addWidget(m_dummyLoadConfirm, 4, 0, 1, 4);

    root->addWidget(group);

    connect(m_daxChannel, &QComboBox::currentIndexChanged, this, [this] {
        m_settings.daxChannel = m_daxChannel->currentData().toInt();
        m_settings.save();
        // A channel change invalidates the capture adapter bound to the old one.
        m_capture.reset();
        m_controller.setCaptureSource(nullptr);
        m_levelSeen = false;
        refreshRunEnabled();
    });
    connect(m_feedbackPan, &QComboBox::currentIndexChanged, this,
            &TxLinearityDialog::refreshRunEnabled);
    connect(m_dummyLoadConfirm, &QCheckBox::toggled, this, [this](bool on) {
        if (on) {
            m_controller.safety().confirmDummyLoad();
        } else {
            m_controller.safety().clearDummyLoadConfirmation();
        }
        refreshRunEnabled();
    });
}

void TxLinearityDialog::buildTestGroup(QVBoxLayout* root)
{
    auto* group = new QGroupBox(tr("2 — Test signal"), bodyWidget());
    auto* grid = new QGridLayout(group);
    grid->setSpacing(6);

    grid->addWidget(new QLabel(tr("Tone spacing")), 0, 0);
    m_toneSpacing = new QDoubleSpinBox(group);
    m_toneSpacing->setRange(200.0, 10000.0);
    m_toneSpacing->setSingleStep(100.0);
    m_toneSpacing->setSuffix(tr(" Hz"));
    m_toneSpacing->setValue(m_settings.toneSpacingHz);
    m_toneSpacing->setAccessibleName(tr("Tone spacing"));
    m_toneSpacing->setAccessibleDescription(
        tr("Separation between the two test tones. Order 7 products reach three "
           "spacings beyond each tone."));
    grid->addWidget(m_toneSpacing, 0, 1);

    grid->addWidget(new QLabel(tr("Tune power")), 0, 2);
    m_tunePower = new QSpinBox(group);
    m_tunePower->setRange(1, 100);
    m_tunePower->setSuffix(tr(" W"));
    m_tunePower->setValue(m_settings.tunePowerW);
    m_tunePower->setAccessibleName(tr("Tune power"));
    grid->addWidget(m_tunePower, 0, 3);

    grid->addWidget(new QLabel(tr("Averages")), 1, 0);
    m_averages = new QSpinBox(group);
    m_averages->setRange(1, 64);
    m_averages->setValue(m_settings.averages);
    m_averages->setAccessibleName(tr("Averages"));
    m_averages->setAccessibleDescription(
        tr("Welch averages to collect. More averages lower the noise floor but "
           "lengthen the transmission."));
    grid->addWidget(m_averages, 1, 1);

    grid->addWidget(new QLabel(tr("Transmit timeout")), 1, 2);
    m_timeoutSec = new QSpinBox(group);
    m_timeoutSec->setRange(1, 10);
    m_timeoutSec->setSuffix(tr(" s"));
    m_timeoutSec->setValue(m_settings.timeoutSec);
    m_timeoutSec->setAccessibleName(tr("Transmit timeout"));
    m_timeoutSec->setAccessibleDescription(
        tr("Hard limit on continuous test transmission. Adjustable downward only."));
    grid->addWidget(m_timeoutSec, 1, 3);

    grid->addWidget(new QLabel(tr("Analysis window")), 2, 0);
    m_window = new QComboBox(group);
    for (txlin::WindowKind kind : {txlin::WindowKind::BlackmanHarris7,
                                   txlin::WindowKind::BlackmanHarris4,
                                   txlin::WindowKind::FlatTop, txlin::WindowKind::Hann}) {
        m_window->addItem(QString::fromStdString(txlin::windowKindLabel(kind)),
                          QString::fromStdString(txlin::windowKindKey(kind)));
    }
    m_window->setCurrentIndex(m_window->findData(m_settings.window));
    m_window->setAccessibleName(tr("Analysis window"));
    m_window->setAccessibleDescription(
        tr("Blackman-Harris 7 is the default because its low sidelobes are what make "
           "products below minus 50 dBc visible at all. Hann cannot see them."));
    grid->addWidget(m_window, 2, 1);

    grid->addWidget(new QLabel(tr("Highest order")), 2, 2);
    m_maxOrder = new QComboBox(group);
    m_maxOrder->addItem(tr("IMD3"), 3);
    m_maxOrder->addItem(tr("IMD3 + 5"), 5);
    m_maxOrder->addItem(tr("IMD3 + 5 + 7"), 7);
    m_maxOrder->setCurrentIndex(m_maxOrder->findData(m_settings.maxOrder));
    m_maxOrder->setAccessibleName(tr("Highest intermodulation order"));
    grid->addWidget(m_maxOrder, 2, 3);

    auto* buttons = new QHBoxLayout();
    m_runButton = new QPushButton(tr("Run Measurement"), group);
    m_runButton->setAccessibleName(tr("Run measurement"));
    // The automation bridge must never key a live radio by accident, and this
    // button transmits. Marking it puts it behind AETHER_AUTOMATION_ALLOW_TX
    // like MOX, TUNE and ATU.
    markTxKeying(m_runButton);
    m_stopButton = new QPushButton(tr("Stop"), group);
    m_stopButton->setAccessibleName(tr("Stop measurement"));
    m_stopButton->setEnabled(false);
    m_progressLabel = new QLabel(QString(), group);
    buttons->addWidget(m_runButton);
    buttons->addWidget(m_stopButton);
    buttons->addWidget(m_progressLabel, 1);
    grid->addLayout(buttons, 3, 0, 1, 4);

    m_statusLabel = new QLabel(QString(), group);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setAccessibleName(tr("Analyzer status"));
    grid->addWidget(m_statusLabel, 4, 0, 1, 4);

    root->addWidget(group);

    connect(m_runButton, &QPushButton::clicked, this, &TxLinearityDialog::onStartClicked);
    connect(m_stopButton, &QPushButton::clicked, this, &TxLinearityDialog::onStopClicked);
}

void TxLinearityDialog::buildResultArea(QVBoxLayout* root)
{
    m_plot = new TxLinearitySpectrumView(bodyWidget());
    root->addWidget(m_plot, 1);

    m_readout = new QTableWidget(0, 2, bodyWidget());
    m_readout->setHorizontalHeaderLabels({tr("Measurement"), tr("Value")});
    m_readout->verticalHeader()->setVisible(false);
    m_readout->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_readout->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_readout->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_readout->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_readout->setAlternatingRowColors(true);
    m_readout->setShowGrid(false);
    m_readout->setMinimumHeight(160);
    m_readout->setAccessibleName(tr("Measurement results"));
    ThemeManager::instance().applyStyleSheet(m_readout,
        "QTableWidget {"
        "  background: {{color.background.0}};"
        "  color: {{color.text.primary}};"
        "  border: 1px solid {{color.background.2}};"
        "  font-size: 11px;"
        "}"
        "QHeaderView::section {"
        "  background: {{color.background.1}};"
        "  color: {{color.text.secondary}};"
        "  border: 0px;"
        "  padding: 4px;"
        "}");
    root->addWidget(m_readout);

    auto* exportRow = new QHBoxLayout();
    m_exportCsvButton = new QPushButton(tr("Export CSV…"), bodyWidget());
    m_exportPngButton = new QPushButton(tr("Export PNG…"), bodyWidget());
    m_exportCsvButton->setAccessibleName(tr("Export results as CSV"));
    m_exportPngButton->setAccessibleName(tr("Export plot as PNG"));
    m_exportCsvButton->setEnabled(false);
    m_exportPngButton->setEnabled(false);
    exportRow->addStretch(1);
    exportRow->addWidget(m_exportCsvButton);
    exportRow->addWidget(m_exportPngButton);
    root->addLayout(exportRow);

    connect(m_exportCsvButton, &QPushButton::clicked, this, &TxLinearityDialog::exportCsv);
    connect(m_exportPngButton, &QPushButton::clicked, this, &TxLinearityDialog::exportPng);
}

// ── Wiring ──────────────────────────────────────────────────────────────────

void TxLinearityDialog::wireController()
{
    if (m_model == nullptr) {
        return;
    }

    // Keying goes through TransmitModel's two-tone tune, which is the radio's
    // OWN test generator: the tones are generated in the radio, downstream of
    // nothing this application controls, so they are exactly the stimulus a
    // SmartSDR operator would produce by hand. Nothing here synthesizes audio.
    m_controller.setKeyRequest([this](bool key) {
        TransmitModel& tx = m_model->transmitModel();
        if (key) {
            tx.setTunePower(m_tunePower->value());
            tx.setTuneMode(QStringLiteral("two_tone"));
            tx.startTwoToneTune();
        } else {
            tx.stopTune();
        }
        return true;
    });

    m_controller.setTelemetryProvider([this] {
        txlin::TxLinearityController::TelemetrySnapshot snap;
        MeterModel& meters = m_model->meterModel();
        if (const std::optional<float> swr = meters.swrIfLive(); swr.has_value()) {
            snap.telemetry.swr = double(*swr);
        }
        snap.telemetry.paTempC = double(meters.paTemp());
        snap.telemetry.forwardWatts = double(meters.fwdPower());
        if (meters.hasSupplyVoltage()) {
            snap.telemetry.supplyVolts = double(meters.supplyVolts());
        }
        snap.lastUpdateMs = m_controller.nowMs();
        return snap;
    });

    connect(&m_model->daxIqModel(), &DaxIqModel::iqLevelReady,
            this, &TxLinearityDialog::onLevelUpdate);
    connect(m_model, &RadioModel::panadapterAdded, this,
            &TxLinearityDialog::refreshPanChoices);
    connect(m_model, &RadioModel::panadapterRemoved, this,
            &TxLinearityDialog::refreshPanChoices);

    connect(&m_controller, &txlin::TxLinearityController::resultReady,
            this, &TxLinearityDialog::onResult);
    connect(&m_controller, &txlin::TxLinearityController::aborted,
            this, &TxLinearityDialog::onAborted);
    connect(&m_controller, &txlin::TxLinearityController::refused,
            this, &TxLinearityDialog::onRefused);
    connect(&m_controller, &txlin::TxLinearityController::progress, this,
            [this](int averages, int target) {
                m_progressLabel->setText(tr("Averaging %1 of %2").arg(averages).arg(target));
            });
    connect(&m_controller, &txlin::TxLinearityController::runStateChanged, this,
            [this](bool running) {
                m_stopButton->setEnabled(running);
                if (!running) {
                    // The safety layer clears the confirmation when a run ends;
                    // the checkbox must follow it or the UI would show a
                    // confirmation the interlock no longer holds.
                    m_dummyLoadConfirm->setChecked(false);
                    m_progressLabel->clear();
                }
                refreshRunEnabled();
            });
}

void TxLinearityDialog::refreshPanChoices()
{
    if (m_model == nullptr || m_feedbackPan == nullptr) {
        return;
    }
    const QString previous = m_feedbackPan->currentData().toString();
    m_feedbackPan->clear();
    for (PanadapterModel* pan : m_model->panadapters()) {
        if (pan == nullptr) {
            continue;
        }
        m_feedbackPan->addItem(
            tr("%1 — %2 MHz").arg(pan->panId()).arg(pan->centerMhz(), 0, 'f', 4),
            pan->panId());
    }
    const int restored = m_feedbackPan->findData(previous);
    if (restored >= 0) {
        m_feedbackPan->setCurrentIndex(restored);
    }
    refreshRunEnabled();
}

void TxLinearityDialog::onLevelUpdate(int channel, float rms)
{
    if (m_daxChannel == nullptr || channel != m_daxChannel->currentData().toInt()) {
        return;
    }
    m_levelSeen = true;
    m_levelDbfs = (rms > 0.0f) ? 20.0 * std::log10(double(rms)) : -120.0;
    m_levelBar->setValue(int(std::round(std::clamp(m_levelDbfs, -60.0, 0.0))));

    QString verdict;
    QString colorToken;
    if (m_levelDbfs < kLevelTooLowDbfs) {
        verdict = tr("Too low");
        colorToken = QStringLiteral("color.accent.danger");
    } else if (m_levelDbfs < kLevelGoodLowDbfs) {
        verdict = tr("Low");
        colorToken = QStringLiteral("color.accent.warning");
    } else if (m_levelDbfs <= kLevelGoodHighDbfs) {
        verdict = tr("Good");
        colorToken = QStringLiteral("color.accent.success");
    } else if (m_levelDbfs <= kLevelTooHighDbfs) {
        verdict = tr("High");
        colorToken = QStringLiteral("color.accent.warning");
    } else {
        verdict = tr("Clipping");
        colorToken = QStringLiteral("color.accent.danger");
    }
    m_levelVerdict->setText(verdict);
    ThemeManager::instance().applyStyleSheet(
        m_levelVerdict, QStringLiteral("QLabel { color: {{%1}}; font-weight: 600; }")
                            .arg(colorToken));
    refreshRunEnabled();
}

void TxLinearityDialog::refreshRunEnabled()
{
    if (m_runButton == nullptr) {
        return;
    }
    const bool running = m_controller.running();
    const bool haveRadio = (m_model != nullptr) && m_model->isConnected();
    const bool havePan = m_feedbackPan != nullptr && m_feedbackPan->count() > 0;
    const bool confirmed = m_dummyLoadConfirm != nullptr && m_dummyLoadConfirm->isChecked();

    m_runButton->setEnabled(!running && haveRadio && havePan && confirmed);
    m_stopButton->setEnabled(running);

    // Setup inputs lock during a run: changing the DAX channel or the tone
    // spacing mid-transmission would invalidate the average that is being
    // accumulated, and the operator would have no way to know.
    const QList<QWidget*> setupInputs{m_daxChannel,  m_feedbackPan, m_toneSpacing,
                                      m_tunePower,   m_averages,    m_timeoutSec,
                                      m_window,      m_maxOrder,    m_dummyLoadConfirm};
    for (QWidget* w : setupInputs) {
        if (w != nullptr) {
            w->setEnabled(!running);
        }
    }
}

// ── Run ─────────────────────────────────────────────────────────────────────

bool TxLinearityDialog::buildRunRequest(txlin::RunRequest& request, QString& reason) const
{
    if (m_model == nullptr) {
        reason = tr("No radio is connected.");
        return false;
    }
    SliceModel* tx = m_model->txSlice();
    if (tx == nullptr) {
        reason = tr("No transmit slice is selected.");
        return false;
    }

    // The two-tone generator sits in the radio and produces its tones either
    // side of the carrier point, so the occupied span is centred on the TX
    // slice's frequency.
    const double centreHz = tx->frequency() * 1e6;
    const double half = m_toneSpacing->value() / 2.0;
    request.tone1Hz = centreHz - half;
    request.tone2Hz = centreHz + half;
    request.maxOrder = m_maxOrder->currentData().toInt();
    request.captureReady = true;

    if (m_bandPlan == nullptr) {
        reason = tr("The band plan is not available, so the test frequency cannot be verified.");
        return false;
    }
    // Search a generous window either side and let the plan decide which
    // contiguous region actually contains the tones. Passing the tone pair
    // itself would return a region clipped to the pair rather than the
    // allocation, and the band-edge check needs the allocation's real edges.
    const double searchLowMhz = (centreHz - 2e6) / 1e6;
    const double searchHighMhz = (centreHz + 2e6) / 1e6;
    for (const BandPlanManager::Region& region :
         m_bandPlan->contiguousRegionsForBand(searchLowMhz, searchHighMhz)) {
        request.allocations.push_back(
            txlin::AllocationRange{region.lowMhz * 1e6, region.highMhz * 1e6});
    }
    return true;
}

void TxLinearityDialog::onStartClicked()
{
    txlin::RunRequest request;
    QString reason;
    if (!buildRunRequest(request, reason)) {
        setStatus(reason, true);
        return;
    }

    // One atomic write of the whole object, not six independent keys.
    m_settings.daxChannel = m_daxChannel->currentData().toInt();
    m_settings.toneSpacingHz = m_toneSpacing->value();
    m_settings.tunePowerW = m_tunePower->value();
    m_settings.averages = m_averages->value();
    m_settings.timeoutSec = m_timeoutSec->value();
    m_settings.window = m_window->currentData().toString();
    m_settings.maxOrder = m_maxOrder->currentData().toInt();
    m_settings.save();

    // Bring up the capture adapter for the selected channel if it is not
    // already bound. Doing this here rather than on every combo change keeps a
    // DAX IQ channel allocated on the radio only while it is actually wanted.
    const int channel = m_daxChannel->currentData().toInt();
    if (m_capture == nullptr) {
        m_capture = std::make_unique<txlin::FlexDaxIqCapture>(channel);
        // The capture adapter holds no model pointer — it asks, and this dialog
        // (which legitimately owns the radio wiring) does. That keeps the
        // engine-boundary ratchet satisfied and, more usefully, leaves the
        // adapter as pure frame assembly for the HPSDR one to mirror.
        DaxIqModel& dax = m_model->daxIqModel();
        connect(m_capture.get(), &txlin::FlexDaxIqCapture::streamStartRequested, &dax,
                [&dax](int ch, int rateHz) {
                    dax.createStream(ch);
                    dax.setSampleRate(ch, rateHz);
                });
        connect(m_capture.get(), &txlin::FlexDaxIqCapture::streamStopRequested, &dax,
                [&dax](int ch) { dax.removeStream(ch); });
        connect(m_capture.get(), &txlin::FlexDaxIqCapture::commandReady, m_model,
                [this](const QString& cmd) { m_model->sendCommand(cmd); });
        connect(&dax, &DaxIqModel::iqSamplesReady, m_capture.get(),
                [this](int ch, const QVector<float>& iq, int rateHz) {
                    m_capture->feedIqSamples(ch, iq, rateHz);
                });
        connect(&dax, &DaxIqModel::streamChanged, m_capture.get(), [this, &dax](int ch) {
            const DaxIqModel::IqStream& stream = dax.stream(ch);
            if (!stream.exists) {
                m_capture->noteStreamRemoved(ch);
            } else if (!stream.rateSettling) {
                m_capture->noteStreamRate(ch, stream.sampleRate);
            }
        });
        m_capture->setRadioDescription(m_model->model());
        m_controller.setCaptureSource(m_capture.get());
    }
    m_capture->setPanId(m_feedbackPan->currentData().toString());

    // The timeout is adjustable downward only; setLimits enforces that and
    // refuses an increase without an override, so a stale persisted value can
    // never quietly loosen the interlock.
    txlin::SafetyLimits limits = m_controller.safety().limits();
    limits.maxTransmitMs = m_timeoutSec->value() * 1000;
    m_controller.safety().setLimits(limits);

    txlin::TxLinearityController::RunConfig config;
    config.capture.sampleRateHz = 192000.0;
    config.capture.frameSamples = 16384;
    config.window = txlin::windowKindFromKey(m_window->currentData().toString().toStdString());
    config.imd.maxOrder = request.maxOrder;
    config.targetAverages = m_averages->value();

    m_plot->clear();
    m_hasResult = false;
    m_exportCsvButton->setEnabled(false);
    m_exportPngButton->setEnabled(false);
    setStatus(tr("Transmitting…"), false);
    m_controller.startRun(request, config);
    refreshRunEnabled();
}

void TxLinearityDialog::onStopClicked()
{
    m_controller.stopRun(txlin::AbortReason::OperatorStop);
    setStatus(tr("Stopped."), false);
}

void TxLinearityDialog::onAborted(txlin::AbortReason reason, const QString& text)
{
    Q_UNUSED(reason);
    setStatus(tr("Measurement aborted: %1").arg(text), true);
}

void TxLinearityDialog::onRefused(const QString& message, const QStringList& warnings)
{
    QString text = message;
    for (const QString& warning : warnings) {
        text += QStringLiteral("\n• ") + warning;
    }
    setStatus(text, true);
}

void TxLinearityDialog::setStatus(const QString& text, bool warning)
{
    m_statusLabel->setText(text);
    ThemeManager::instance().applyStyleSheet(
        m_statusLabel, warning
                           ? "QLabel { color: {{color.accent.warning}}; font-size: 11px; }"
                           : "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
}

void TxLinearityDialog::onResult(const txlin::LinearityResult& result)
{
    m_lastResult = result;
    m_hasResult = result.valid;
    m_plot->setResult(result);
    populateReadout(result);
    m_exportCsvButton->setEnabled(result.valid);
    m_exportPngButton->setEnabled(result.valid);

    if (!result.valid) {
        setStatus(tr("No measurement: %1").arg(QString::fromStdString(result.invalidReason)),
                  true);
        return;
    }

    QStringList caveats;
    if (result.clipped) {
        caveats << tr("the capture clipped — reduce the feedback level and re-run");
    }
    if (std::abs(result.toneImbalanceDb) > 0.5) {
        caveats << tr("the two tones differ by %1 dB, so the symmetric-IMD reading is "
                      "approximate")
                       .arg(QString::number(result.toneImbalanceDb, 'f', 2));
    }
    int noiseLimited = 0;
    for (const txlin::ImdProduct& p : result.products) {
        if (p.limitedByNoiseFloor) {
            ++noiseLimited;
        }
    }
    if (noiseLimited > 0) {
        caveats << tr("%1 product(s) sit at the instrument's own noise floor and are not "
                      "quoted").arg(noiseLimited);
    }

    setStatus(caveats.isEmpty()
                  ? tr("Measurement complete — %1 averages.").arg(result.averages)
                  : tr("Measurement complete — %1.").arg(caveats.join(tr("; "))),
              !caveats.isEmpty());
}

void TxLinearityDialog::populateReadout(const txlin::LinearityResult& result)
{
    m_readout->setRowCount(0);
    if (!result.valid) {
        return;
    }

    const auto addRow = [this](const QString& name, const QString& value) {
        const int row = m_readout->rowCount();
        m_readout->insertRow(row);
        m_readout->setItem(row, 0, new QTableWidgetItem(name));
        m_readout->setItem(row, 1, new QTableWidgetItem(value));
    };

    addRow(tr("Tone spacing"), tr("%1 Hz").arg(QString::number(result.toneSpacingHz, 'f', 1)));
    addRow(tr("Tone imbalance"),
           tr("%1 dB").arg(QString::number(result.toneImbalanceDb, 'f', 2)));
    addRow(tr("Measured PEP"), tr("%1 dBFS").arg(QString::number(result.pepDb, 'f', 2)));
    addRow(tr("Instrument noise floor"),
           tr("%1 dBFS per bin").arg(QString::number(result.noiseFloorDb, 'f', 1)));
    addRow(tr("Averages"), QString::number(result.averages));
    addRow(tr("Resolution"), tr("%1 Hz/bin").arg(QString::number(result.binHz, 'f', 2)));

    for (int order : {3, 5, 7}) {
        for (const txlin::ImdProduct& p : result.products) {
            if (p.order != order) {
                continue;
            }
            addRow(tr("IMD%1 %2").arg(order).arg(p.upper ? tr("upper") : tr("lower")),
                   refText(p.dbcPerTone, p.dbcPep, !p.limitedByNoiseFloor));
        }
    }

    const auto addBand = [&](const QString& name, const std::optional<txlin::BandPower>& band) {
        if (!band.has_value()) {
            // An absent band is stated, not omitted: "the capture was too narrow
            // to integrate this" and "this measured zero" are different facts.
            addRow(name, tr("not measurable in the capture bandwidth"));
            return;
        }
        addRow(name, QStringLiteral("%1   (%2 to %3 Hz)")
                         .arg(refText(band->dbcPerTone, band->dbcPep, true))
                         .arg(QString::number(band->lowHz, 'f', 0))
                         .arg(QString::number(band->highHz, 'f', 0)));
    };
    addBand(tr("Shoulder, lower"), result.shoulderLow);
    addBand(tr("Shoulder, upper"), result.shoulderHigh);
    addBand(tr("ACPR, lower"), result.acprLow);
    addBand(tr("ACPR, upper"), result.acprHigh);

    addRow(tr("Capture clipped"), result.clipped ? tr("YES") : tr("no"));
}

// ── Export ──────────────────────────────────────────────────────────────────

QString TxLinearityDialog::provenanceStamp() const
{
    if (m_model == nullptr) {
        return QString();
    }
    SliceModel* tx = m_model->txSlice();
    const TransmitModel& transmit = m_model->transmitModel();
    return tr("radio=%1, frequency=%2 MHz, mode=%3, tune power=%4 W, APD=%5, window=%6, "
              "averages=%7, capture=%8")
        .arg(m_model->model())
        .arg(tx != nullptr ? QString::number(tx->frequency(), 'f', 6) : tr("unknown"))
        .arg(tx != nullptr ? tx->mode() : tr("unknown"))
        .arg(m_tunePower->value())
        .arg(transmit.apdEnabled() ? tr("on") : tr("off"))
        .arg(m_window->currentText())
        .arg(m_lastResult.averages)
        .arg(m_capture != nullptr ? QString::fromStdString(m_capture->describe())
                                  : tr("unknown"));
}

void TxLinearityDialog::exportCsv()
{
    if (!m_hasResult) {
        return;
    }
    const QString suggested =
        QStringLiteral("tx_linearity_%1.csv")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Measurement"), suggested, tr("CSV files (*.csv)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("TX Linearity Analyzer"),
                             tr("Could not write %1").arg(path));
        return;
    }
    QTextStream out(&file);
    out << "# AetherSDR TX Linearity Analyzer\n";
    out << "# " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
    out << "# " << provenanceStamp() << "\n";
    // Both reference conventions in the header, so a CSV read six months later
    // cannot be misinterpreted.
    out << "Measurement,Value,Unit,Reference\n";
    out << "Tone spacing," << QString::number(m_lastResult.toneSpacingHz, 'f', 2) << ",Hz,\n";
    out << "Tone imbalance," << QString::number(m_lastResult.toneImbalanceDb, 'f', 3) << ",dB,\n";
    out << "Measured PEP," << QString::number(m_lastResult.pepDb, 'f', 3) << ",dBFS,\n";
    out << "Instrument noise floor," << QString::number(m_lastResult.noiseFloorDb, 'f', 2)
        << ",dBFS,per analysis bin\n";
    out << "Averages," << m_lastResult.averages << ",,\n";
    for (const txlin::ImdProduct& p : m_lastResult.products) {
        const QString side = p.upper ? QStringLiteral("upper") : QStringLiteral("lower");
        if (p.limitedByNoiseFloor) {
            out << "IMD" << p.order << " " << side << ",,dBc,below instrument noise floor\n";
            continue;
        }
        out << "IMD" << p.order << " " << side << "," << QString::number(p.dbcPerTone, 'f', 2)
            << ",dBc,one tone\n";
        out << "IMD" << p.order << " " << side << "," << QString::number(p.dbcPep, 'f', 2)
            << ",dBc,PEP\n";
    }
    const auto writeBand = [&out](const char* name, const std::optional<txlin::BandPower>& b) {
        if (!b.has_value()) {
            out << name << ",,dBc,not measurable in the capture bandwidth\n";
            return;
        }
        out << name << "," << QString::number(b->dbcPerTone, 'f', 2) << ",dBc,one tone ("
            << QString::number(b->lowHz, 'f', 0) << " to " << QString::number(b->highHz, 'f', 0)
            << " Hz)\n";
        out << name << "," << QString::number(b->dbcPep, 'f', 2) << ",dBc,PEP\n";
    };
    writeBand("Shoulder lower", m_lastResult.shoulderLow);
    writeBand("Shoulder upper", m_lastResult.shoulderHigh);
    writeBand("ACPR lower", m_lastResult.acprLow);
    writeBand("ACPR upper", m_lastResult.acprHigh);
    out << "Capture clipped," << (m_lastResult.clipped ? "yes" : "no") << ",,\n";

    setStatus(tr("Exported %1").arg(path), false);
}

void TxLinearityDialog::exportPng()
{
    if (!m_hasResult || m_plot == nullptr) {
        return;
    }
    const QString suggested =
        QStringLiteral("tx_linearity_%1.png")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Plot"), suggested, tr("PNG images (*.png)"));
    if (path.isEmpty()) {
        return;
    }
    if (!m_plot->grab().save(path, "PNG")) {
        QMessageBox::warning(this, tr("TX Linearity Analyzer"),
                             tr("Could not write %1").arg(path));
        return;
    }
    setStatus(tr("Exported %1").arg(path), false);
}

}  // namespace AetherSDR
