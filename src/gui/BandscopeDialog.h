#pragma once

#include "gui/PersistentDialog.h"

#include <QMetaObject>
#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;

namespace AetherSDR {

class RadioModel;

// The trace surface: one spectrum, drawn with QPainter.
//
// A PLAIN QWidget AND NOT SpectrumWidget. SpectrumWidget is a QRhiWidget with a
// waterfall, a slice overlay, band annotations, spot markers and a GPU failure
// contract; none of that is wanted here and a second top-level RHI surface is a
// cross-platform question nobody in this lab can answer (there is no Linux or
// Windows machine here). A raster trace of ~1000 bins, redrawn when a frame
// arrives and not on a timer, costs nothing worth measuring.
class BandscopeTrace : public QWidget {
    Q_OBJECT

public:
    explicit BandscopeTrace(QWidget* parent = nullptr);

    // One frame: magnitudes in dBFS, low bin first, spanning DC to
    // sampleRateHz/2. An empty vector clears the surface back to "no frame".
    void setFrame(const QVector<float>& binsDb, double sampleRateHz);
    void clearFrame();

    QSize sizeHint() const override { return {880, 300}; }
    QSize minimumSizeHint() const override { return {320, 140}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void drawGrid(QPainter& p, const QRectF& plot) const;
    void drawTrace(QPainter& p, const QRectF& plot) const;

    QVector<float> m_bins;
    double m_sampleRateHz{0.0};
    // The displayed dB window. Fixed, not auto-ranged: an auto-range would make
    // a quiet band and a band with a broadcast carrier in it look identical,
    // which is the exact comparison this window exists to make possible.
    //
    // The top is 0 dBFS because that is the converter's rail and the gateware's
    // own clip threshold. REACHABLE ONLY BECAUSE THE CALLER CORRECTS THE SCALE:
    // ClientEqFftAnalyzer's own bins are 6.02 dB low (its 2/N normalisation
    // does not remove the Hann window's coherent gain), so BandscopeDialog::
    // onFrame() adds coherentGainCorrectionDb() before the bins arrive here. A
    // caller that forgets is not drawing dBFS. `bandscope_analyzer_test` pins
    // the helper; `bandscope_trace_render_test` pins the production readout.
    //
    // The bottom is -100 dB, which sits just under the analyzer's kFloorDb
    // sentinel once that sentinel has been corrected (-100 + 6.02 = -93.98), so
    // a record with no energy in it draws a flat line near the bottom of the
    // plot rather than off the end of it.
    static constexpr float kTopDb = 0.0f;
    static constexpr float kBottomDb = -100.0f;
};

// The wideband converter view, in its own window.
//
// WHAT IT IS FOR. The operator sees one slice, 48 to 384 kHz wide, and both the
// waterfall and the S-meter are post-DDC. A broadcast station 20 MHz outside
// that slice can drive the converter towards its rail while everything on the
// main display looks calm. This window shows the converter's whole first
// Nyquist zone so that condition is visible instead of inferred.
//
// TWO DESIGN DECISIONS, BOTH DELIBERATE:
//
//   * A SEPARATE WINDOW rather than a mode of the panadapter. The point is
//     seeing both at once — a mode switch hides the comparison the feature
//     exists to show — and a new window changes no existing surface.
//   * ON DEMAND rather than continuous. A continuous consumer would be a second
//     permanent load on the backend's I/O thread, and for the HL2 that thread
//     already carries EP2 pacing, EP6 ingest, WDSP and the panadapter FFT. That
//     cost has NEVER BEEN MEASURED. One frame when the window opens and one per
//     Refresh avoids the question rather than guessing the answer.
//
// IT NAMES NO RADIO FAMILY. The window asks the connected backend for
// RadioCapabilities::widebandConverterView and invokes the namespace and verb
// that record carries. Today exactly one backend answers; this window does not
// know or care which.
//
// WHAT IT DELIBERATELY DOES NOT HAVE: a waterfall, click-to-tune, markers, band
// annotations, or any interaction with the panadapter. Each of those is a
// separate design decision.
class BandscopeDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit BandscopeDialog(RadioModel* model, QWidget* parent = nullptr);
    ~BandscopeDialog() override;

private:
    friend struct BandscopeDialogTestAccess;
    // Ask the backend for one frame. A no-op while one is already outstanding.
    void requestFrame();
    // Tear down the reply connections for the outstanding request, if any, and
    // stop the deadline. Leaves the Refresh button alone — the three callers
    // want three different button states.
    void releaseRequest();
    void onFrame(const QVariant& reply);
    void showStatus(const QString& text);

    RadioModel* m_model{nullptr};
    BandscopeTrace* m_trace{nullptr};
    QLabel* m_status{nullptr};
    QPushButton* m_refresh{nullptr};
    // The deadline on an outstanding request. WITHOUT IT A REQUEST CAN BE
    // ORPHANED AND REFRESH NEVER COMES BACK: RadioModel destroys the backend
    // outright when it is replaced (reconnect, family swap), Qt drops the two
    // lambdas below without telling anyone, and nothing then clears
    // m_requestId — so requestFrame() early-returns for the life of the window
    // with the status stuck on "Waiting for a frame…". The window is closeable
    // and WA_DeleteOnClose makes reopening recover, but the operator should not
    // have to discover that. Not reproduced against a radio; reasoned from
    // RadioModel's teardown path.
    QTimer* m_deadline{nullptr};

    // The id of the outstanding extension request, or 0. Minted here rather
    // than taken from a shared counter because IRadioBackend's contract puts
    // correlation on the caller: "a caller that wants a reply connects to the
    // backend's extensionResult/extensionError and correlates its own id".
    quint64 m_requestId{0};
    // Bound to the CURRENT backend at request time and dropped when the reply
    // arrives. Not held across requests on purpose: the backend object is
    // replaced on every reconnect and on a family swap, and a connection made
    // once in the constructor would be dead after the first of those.
    QMetaObject::Connection m_okConn;
    QMetaObject::Connection m_errConn;
};

} // namespace AetherSDR
