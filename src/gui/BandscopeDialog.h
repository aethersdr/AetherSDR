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
    // Fixed dB window, not auto-ranged, so a quiet band and one with a broadcast
    // carrier look different. Top 0 dBFS = converter rail / gateware clip, reachable
    // only because onFrame() adds coherentGainCorrectionDb() (the analyzer's bins
    // are 6.02 dB low); pinned by bandscope_analyzer_test and
    // bandscope_trace_render_test. Bottom -100 dB sits just under the corrected
    // kFloorDb sentinel (-93.98), so an empty record draws near the bottom.
    static constexpr float kTopDb = 0.0f;
    static constexpr float kBottomDb = -100.0f;
};

// Wideband converter view: the converter's whole first Nyquist zone, so an
// out-of-slice signal driving the ADC towards its rail is visible (the
// waterfall and S-meter are post-DDC). A separate window, so both are visible
// at once; on demand (one frame on open and per Refresh) because a continuous
// consumer would load the backend I/O thread (on HL2: EP2 pacing, EP6, WDSP,
// pan FFT) at an unmeasured cost. Names no radio family: it invokes whatever
// RadioCapabilities::widebandConverterView advertises. No waterfall,
// click-to-tune, markers or pan interaction by design.
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
