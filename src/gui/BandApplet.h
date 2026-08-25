#pragma once

#include "core/backends/RadioCapabilities.h"
#include "models/ModelCapabilities.h"
#include "models/DeclaredBands.h"

#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QGridLayout;
class QPushButton;

namespace AetherSDR {

class SliceModel;

// BandApplet — right-side sidebar applet for quick band switching.
// Mirrors the band selection functionality of SpectrumOverlayMenu's Band popup,
// including HF, VHF/UHF capability bands, custom XVTR bands, and utility bands.
// Highlights the active band button matching the current slice's RX frequency.
class BandApplet : public QWidget {
    Q_OBJECT

public:
    struct XvtrBand {
        QString name;
        double  rfFreqMhz{0.0};
        QString stackKey;
    };

    explicit BandApplet(QWidget* parent = nullptr);

    void setSlice(SliceModel* slice);
    void setRadioCapabilities(ModelCapabilities caps);
    void setXvtrBands(const QVector<XvtrBand>& bands);
    void setDeclaredBands(const QStringList& bands,
                          const QVector<DeclaredBandRange>& ranges = {});
    void setTuningRange(double minMhz, double maxMhz);

signals:
    void bandSelected(const QString& bandName, double freqMhz, const QString& mode,
                      const QString& stackKeyHint = QString());
    void xvtrSetupRequested();

private:
    void rebuildGrid();
    void updateActiveBand();
    void applyTuningRange();

    struct BandBtnInfo {
        QPushButton* btn{nullptr};
        QString      bandName;
        double       freqMhz{0.0};
        QString      mode;
        QString      stackKey;
        bool         isXvtrSetup{false};
    };

    QGridLayout*          m_grid{nullptr};
    QPointer<SliceModel>  m_slice;
    ModelCapabilities     m_radioCapabilities;
    QVector<XvtrBand>     m_xvtrBands;
    QStringList           m_declaredBands;
    QVector<DeclaredBandRange> m_declaredBandRanges;
    double                m_tuningMinMhz{0.0};
    double                m_tuningMaxMhz{0.0};
    QVector<BandBtnInfo>  m_buttons;
};

} // namespace AetherSDR
