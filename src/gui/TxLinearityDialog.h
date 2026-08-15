#pragma once

#include "PersistentDialog.h"
#include "core/TxLinearitySettings.h"
#include "core/txlinearity/TxLinearityController.h"

#include <QString>

#include <memory>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;
class QVBoxLayout;

namespace AetherSDR {

class BandPlanManager;
class RadioModel;
class TxLinearitySpectrumView;
namespace txlin {
class FlexDaxIqCapture;
}

// TX Linearity Analyzer — the instrument window.
//
// Puts numbers on transmitter and amplifier linearity using hardware the
// operator already owns: the radio transmits a two-tone, attenuated TX energy
// is routed back into a receive-only port, and a DAX IQ stream on a panadapter
// watching that port becomes the measurement. Today a Flex owner can turn
// SmartSignal on and look at a picture; this answers "is it actually helping,
// on this band, at this power, into this amplifier" — which otherwise needs a
// spectrum analyzer.
//
// The dialog owns the run: it drives the guided setup, gates the Run button on
// every §8 interlock, and holds the controller and the capture adapter. It does
// NOT do DSP — every number on screen comes from `core/txlinearity`, which
// knows nothing about Qt or FlexRadio and is unit-tested without either.
//
// A NOTE ON THE SETUP FLOW. The brief calls for a wizard. This is a guided
// setup SECTION rather than a modal multi-page QWizard, because a wizard would
// be a new interaction pattern in an application that has none, and because the
// operator needs to watch the live feedback level while adjusting a physical
// attenuator — which a page-at-a-time wizard actively obstructs. The steps, the
// order, and the refusal to run until each one validates are unchanged.
class TxLinearityDialog : public PersistentDialog {
    Q_OBJECT

public:
    TxLinearityDialog(RadioModel* model, BandPlanManager* bandPlan,
                      QWidget* parent = nullptr);
    ~TxLinearityDialog() override;

private:
    void buildSetupGroup(QVBoxLayout* root);
    void buildTestGroup(QVBoxLayout* root);
    void buildResultArea(QVBoxLayout* root);

    void wireController();
    void refreshPanChoices();
    void refreshRunEnabled();
    void onLevelUpdate(int channel, float rms);
    void onStartClicked();
    void onStopClicked();
    void onResult(const txlin::LinearityResult& result);
    void onAborted(txlin::AbortReason reason, const QString& text);
    void onRefused(const QString& message, const QStringList& warnings);
    void populateReadout(const txlin::LinearityResult& result);
    void exportCsv();
    void exportPng();
    void setStatus(const QString& text, bool warning);

    // Build the run request from the current UI state and radio state. Returns
    // false with a reason when the radio cannot currently be measured — no TX
    // slice, no band plan, and so on.
    bool buildRunRequest(txlin::RunRequest& request, QString& reason) const;

    // Everything that stamps an exported file: band, power, mode, APD state,
    // radio model, window, averages. A measurement without this context is
    // an anecdote.
    [[nodiscard]] QString provenanceStamp() const;

    RadioModel* m_model{nullptr};
    BandPlanManager* m_bandPlan{nullptr};

    // The feature's owned configuration object (Principle V) — loaded once,
    // written as a unit. The dummy-load confirmation is deliberately not in it.
    TxLinearitySettings m_settings;

    txlin::TxLinearityController m_controller;
    std::unique_ptr<txlin::FlexDaxIqCapture> m_capture;

    // Setup
    QComboBox* m_daxChannel{nullptr};
    QComboBox* m_feedbackPan{nullptr};
    QProgressBar* m_levelBar{nullptr};
    QLabel* m_levelVerdict{nullptr};
    QCheckBox* m_dummyLoadConfirm{nullptr};

    // Test parameters
    QDoubleSpinBox* m_toneSpacing{nullptr};
    QSpinBox* m_tunePower{nullptr};
    QSpinBox* m_averages{nullptr};
    QSpinBox* m_timeoutSec{nullptr};
    QComboBox* m_window{nullptr};
    QComboBox* m_maxOrder{nullptr};

    QPushButton* m_runButton{nullptr};
    QPushButton* m_stopButton{nullptr};
    QPushButton* m_exportCsvButton{nullptr};
    QPushButton* m_exportPngButton{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_progressLabel{nullptr};

    TxLinearitySpectrumView* m_plot{nullptr};
    QTableWidget* m_readout{nullptr};

    txlin::LinearityResult m_lastResult;
    bool m_hasResult{false};
    // Feedback level in dBFS, updated from the DAX IQ RMS meter. Held here
    // rather than read back off the progress bar so the run gate reasons about
    // the measurement, not about a widget's rounding.
    double m_levelDbfs{-120.0};
    bool m_levelSeen{false};
};

}  // namespace AetherSDR
