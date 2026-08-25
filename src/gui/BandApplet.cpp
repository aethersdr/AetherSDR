#include "BandApplet.h"

#include "DeclaredBandMenuPolicy.h"
#include "core/ThemeManager.h"
#include "models/BandDefs.h"
#include "models/BandSettings.h"
#include "models/SliceModel.h"

#include <QGridLayout>
#include <QPushButton>

namespace AetherSDR {

namespace {

struct BandGridEntry {
    const char* label;
    const char* bandName;  // key for BandSettings (e.g. "20m")
    double      freqMhz;
    const char* mode;
};

static constexpr BandGridEntry BAND_GRID[] = {
    {"160",  "160m",  1.900,  "LSB"},   // 0
    {"80",   "80m",   3.800,  "LSB"},   // 1
    {"60",   "60m",   5.357,  "USB"},   // 2
    {"40",   "40m",   7.200,  "LSB"},   // 3
    {"30",   "30m",  10.125,  "DIGU"},  // 4
    {"20",   "20m",  14.225,  "USB"},   // 5
    {"17",   "17m",  18.130,  "USB"},   // 6
    {"15",   "15m",  21.300,  "USB"},   // 7
    {"12",   "12m",  24.950,  "USB"},   // 8
    {"10",   "10m",  28.400,  "USB"},   // 9
    {"6",    "6m",   50.150,  "USB"},   // 10
    {"WWV",  "WWV",  10.000,  "AM"},    // 11
    {"GEN",  "GEN",   0.500,  "AM"},    // 12
    {"2200", "2200m", 0.1375, "CW"},    // 13
    {"630",  "630m",  0.475,  "CW"},    // 14
    {"XVTR", "",      0.0,    ""},      // 15
    {"4",    "4m",   70.200,  "USB"},   // 16
    {"2",    "2m",  144.200,  "USB"},   // 17
};

constexpr int kBandIdxXvtr = 15;
constexpr int kBandIdx4m   = 16;
constexpr int kBandIdx2m   = 17;
constexpr int kGridColumns = 6;

const QString kBandAppletStyle = QStringLiteral(
    "QPushButton {"
    "  background: {{color.background.1}};"
    "  border: 1px solid {{color.background.2}};"
    "  border-radius: 3px;"
    "  color: {{color.text.primary}};"
    "  font-size: 10px;"
    "  font-weight: bold;"
    "  padding: 1px 2px;"
    "}"
    "QPushButton:hover {"
    "  background: {{color.background.2}};"
    "}"
    "QPushButton:checked {"
    "  background: {{color.accent.dim}};"
    "  color: {{color.text.primary}};"
    "  border: 1px solid {{color.accent.bright}};"
    "}"
    "QPushButton:disabled {"
    "  color: {{color.text.disabled}};"
    "  border-color: {{color.background.1}};"
    "}"
);

} // namespace

BandApplet::BandApplet(QWidget* parent)
    : QWidget(parent)
{
    m_grid = new QGridLayout(this);
    m_grid->setContentsMargins(4, 4, 4, 4);
    m_grid->setSpacing(2);

    for (int col = 0; col < kGridColumns; ++col) {
        m_grid->setColumnStretch(col, 1);
    }

    ThemeManager::instance().applyStyleSheet(this, kBandAppletStyle);
    rebuildGrid();
}

void BandApplet::setSlice(SliceModel* slice)
{
    if (m_slice == slice) {
        return;
    }
    if (m_slice) {
        disconnect(m_slice, &SliceModel::frequencyChanged, this, &BandApplet::updateActiveBand);
    }
    m_slice = slice;
    if (m_slice) {
        connect(m_slice, &SliceModel::frequencyChanged, this, &BandApplet::updateActiveBand);
    }
    updateActiveBand();
}

void BandApplet::setRadioCapabilities(ModelCapabilities caps)
{
    m_radioCapabilities = caps;
    rebuildGrid();
}

void BandApplet::setXvtrBands(const QVector<XvtrBand>& bands)
{
    m_xvtrBands = bands;
    rebuildGrid();
}

void BandApplet::setDeclaredBands(const QStringList& bands,
                                  const QVector<DeclaredBandRange>& ranges)
{
    m_declaredBands = bands;
    m_declaredBandRanges = ranges;
    rebuildGrid();
}

void BandApplet::setTuningRange(double minMhz, double maxMhz)
{
    m_tuningMinMhz = minMhz;
    m_tuningMaxMhz = maxMhz;
    applyTuningRange();
}

void BandApplet::rebuildGrid()
{
    // Clear previous buttons
    for (const auto& info : m_buttons) {
        if (info.btn) {
            info.btn->deleteLater();
        }
    }
    m_buttons.clear();

    const bool hasDeclared = !m_declaredBands.isEmpty();
    int row = 0;
    int col = 0;

    auto addButton = [&](const QString& label, const QString& bandName,
                         double freqMhz, const QString& mode,
                         const QString& stackKey = QString(),
                         bool isXvtrSetup = false) {
        auto* btn = new QPushButton(label, this);
        btn->setCheckable(true);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setFixedHeight(20);

        BandBtnInfo info;
        info.btn = btn;
        info.bandName = bandName;
        info.freqMhz = freqMhz;
        info.mode = mode;
        info.stackKey = stackKey;
        info.isXvtrSetup = isXvtrSetup;
        m_buttons.append(info);

        connect(btn, &QPushButton::clicked, this, [this, info]() {
            if (info.isXvtrSetup) {
                emit xvtrSetupRequested();
            } else {
                emit bandSelected(info.bandName, info.freqMhz, info.mode, info.stackKey);
            }
            // Keep button check state aligned with actual slice state
            updateActiveBand();
        });

        m_grid->addWidget(btn, row, col);
        col++;
        if (col >= kGridColumns) {
            col = 0;
            row++;
        }
    };

    if (!hasDeclared) {
        // Standard HF bands (160m to 10m)
        for (int i = 0; i <= 9; ++i) {
            addButton(BAND_GRID[i].label, BAND_GRID[i].bandName,
                      BAND_GRID[i].freqMhz, BAND_GRID[i].mode);
        }

        // 6m
        addButton(BAND_GRID[10].label, BAND_GRID[10].bandName,
                  BAND_GRID[10].freqMhz, BAND_GRID[10].mode);

        // Built-in VHF/UHF capabilities
        if (m_radioCapabilities.has4Meters) {
            addButton(BAND_GRID[kBandIdx4m].label, BAND_GRID[kBandIdx4m].bandName,
                      BAND_GRID[kBandIdx4m].freqMhz, BAND_GRID[kBandIdx4m].mode);
        }
        if (m_radioCapabilities.has2Meters) {
            addButton(BAND_GRID[kBandIdx2m].label, BAND_GRID[kBandIdx2m].bandName,
                      BAND_GRID[kBandIdx2m].freqMhz, BAND_GRID[kBandIdx2m].mode);
        }

        // Custom XVTR bands
        for (const auto& xvtr : m_xvtrBands) {
            addButton(xvtr.name, xvtr.name, xvtr.rfFreqMhz, QStringLiteral("USB"), xvtr.stackKey);
        }

        // Utility: WWV, GEN
        addButton(BAND_GRID[11].label, BAND_GRID[11].bandName,
                  BAND_GRID[11].freqMhz, BAND_GRID[11].mode);
        addButton(BAND_GRID[12].label, BAND_GRID[12].bandName,
                  BAND_GRID[12].freqMhz, BAND_GRID[12].mode);

        // 2200, 630, XVTR
        addButton(BAND_GRID[13].label, BAND_GRID[13].bandName,
                  BAND_GRID[13].freqMhz, BAND_GRID[13].mode);
        addButton(BAND_GRID[14].label, BAND_GRID[14].bandName,
                  BAND_GRID[14].freqMhz, BAND_GRID[14].mode);
        addButton(BAND_GRID[kBandIdxXvtr].label, BAND_GRID[kBandIdxXvtr].bandName,
                  BAND_GRID[kBandIdxXvtr].freqMhz, BAND_GRID[kBandIdxXvtr].mode,
                  QString(), /*isXvtrSetup=*/true);
    } else {
        // Radio declared bands
        for (const auto& def : kBands) {
            const QString bandName = QString::fromLatin1(def.name);
            if (!m_declaredBands.contains(bandName))
                continue;
            const QString label = declaredBandButtonLabel(bandName, m_declaredBandRanges);
            const double freqMhz = def.defaultFreqMhz;
            const QString mode = QString::fromLatin1(def.defaultMode);
            addButton(label, bandName, freqMhz, mode, bandName);
        }

        // Custom XVTR bands
        for (const auto& xvtr : m_xvtrBands) {
            addButton(xvtr.name, xvtr.name, xvtr.rfFreqMhz, QStringLiteral("USB"), xvtr.stackKey);
        }

        // Include utilities if available in tuning range
        if (declaredBandMenuIncludesUtility(true, false, BAND_GRID[11].freqMhz,
                                           m_tuningMinMhz, m_tuningMaxMhz)) {
            addButton(BAND_GRID[11].label, BAND_GRID[11].bandName,
                      BAND_GRID[11].freqMhz, BAND_GRID[11].mode);
        }
        if (declaredBandMenuIncludesUtility(true, false, BAND_GRID[12].freqMhz,
                                           m_tuningMinMhz, m_tuningMaxMhz)) {
            addButton(BAND_GRID[12].label, BAND_GRID[12].bandName,
                      BAND_GRID[12].freqMhz, BAND_GRID[12].mode);
        }
        if (declaredBandMenuIncludesUtility(true, false, BAND_GRID[13].freqMhz,
                                           m_tuningMinMhz, m_tuningMaxMhz)) {
            addButton(BAND_GRID[13].label, BAND_GRID[13].bandName,
                      BAND_GRID[13].freqMhz, BAND_GRID[13].mode);
        }
        if (declaredBandMenuIncludesUtility(true, false, BAND_GRID[14].freqMhz,
                                           m_tuningMinMhz, m_tuningMaxMhz)) {
            addButton(BAND_GRID[14].label, BAND_GRID[14].bandName,
                      BAND_GRID[14].freqMhz, BAND_GRID[14].mode);
        }
        addButton(BAND_GRID[kBandIdxXvtr].label, BAND_GRID[kBandIdxXvtr].bandName,
                  BAND_GRID[kBandIdxXvtr].freqMhz, BAND_GRID[kBandIdxXvtr].mode,
                  QString(), /*isXvtrSetup=*/true);
    }

    applyTuningRange();
    updateActiveBand();
}

void BandApplet::applyTuningRange()
{
    if (m_tuningMaxMhz <= m_tuningMinMhz) {
        for (const auto& info : m_buttons) {
            if (info.btn) {
                info.btn->setEnabled(true);
            }
        }
        return;
    }

    for (const auto& info : m_buttons) {
        if (!info.btn) continue;
        if (info.isXvtrSetup) {
            info.btn->setEnabled(true);
            continue;
        }
        const bool inRange = (info.freqMhz >= m_tuningMinMhz && info.freqMhz <= m_tuningMaxMhz);
        info.btn->setEnabled(inRange);
    }
}

void BandApplet::updateActiveBand()
{
    if (!m_slice) {
        for (const auto& info : m_buttons) {
            if (info.btn) {
                QSignalBlocker b(info.btn);
                info.btn->setChecked(false);
            }
        }
        return;
    }

    const double freq = m_slice->frequency();
    QString activeBand = BandSettings::bandForFrequency(freq);

    // Check if frequency matches any custom XVTR band
    for (const auto& xvtr : m_xvtrBands) {
        if (std::abs(freq - xvtr.rfFreqMhz) < 0.5) {
            activeBand = xvtr.name;
            break;
        }
    }

    // Check if matching any declared band ranges
    for (const auto& range : m_declaredBandRanges) {
        if (range.lowHz > 0.0 && range.highHz > 0.0) {
            const double lowMhz = range.lowHz / 1.0e6;
            const double highMhz = range.highHz / 1.0e6;
            if (freq >= lowMhz && freq <= highMhz) {
                activeBand = range.name;
                break;
            }
        }
    }

    // Update checked state on all buttons
    for (const auto& info : m_buttons) {
        if (!info.btn || info.isXvtrSetup) {
            if (info.btn) {
                QSignalBlocker b(info.btn);
                info.btn->setChecked(false);
            }
            continue;
        }

        const bool match = (info.bandName.compare(activeBand, Qt::CaseInsensitive) == 0);
        QSignalBlocker b(info.btn);
        info.btn->setChecked(match);
    }
}

} // namespace AetherSDR
