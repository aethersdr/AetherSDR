#ifdef HAVE_WEBSOCKETS

#include "FreeDvReporterDialog.h"
#include "FreeDvReporterModel.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "models/SliceModel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QTableView>
#include <QHeaderView>
#include <QCheckBox>
#include <QRadioButton>
#include <QPushButton>
#include <QButtonGroup>
#include <QJsonObject>
#include <QJsonDocument>
#include <cmath>

namespace AetherSDR {

// ── Proxy ──────────────────────────────────────────────────────────────────

class FreeDvReporterProxy : public QSortFilterProxyModel {
public:
    enum FilterMode { Band, Freq };

    explicit FreeDvReporterProxy(QObject* parent = nullptr)
        : QSortFilterProxyModel(parent)
    {
        setDynamicSortFilter(true);
        setSortRole(Qt::UserRole);
    }

    void setBandFilter(double low, double high)
    {
        m_mode = Band;
        m_low  = low;
        m_high = high;
        invalidateFilter();
    }

    void setFreqFilter(double hz)
    {
        m_mode   = Freq;
        m_freqHz = hz;
        invalidateFilter();
    }

    void clearFilter()
    {
        m_mode = Band;
        m_low  = 0.0;
        m_high = 0.0;
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override
    {
        const QModelIndex mhzIdx = sourceModel()->index(
            sourceRow, FreeDvReporterModel::MHz, sourceParent);
        const double mhz = sourceModel()->data(mhzIdx, Qt::UserRole).toDouble();

        if (m_mode == Band) {
            if (m_low <= 0.0 && m_high <= 0.0) return true;  // "All"
            return mhz >= m_low && mhz <= m_high;
        } else {
            // Freq mode: match exact Hz (llround comparison)
            const long long stationHz = llround(mhz * 1e6);
            return stationHz == llround(m_freqHz);
        }
    }

    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override
    {
        const int col = left.column();
        if (col == FreeDvReporterModel::Km
         || col == FreeDvReporterModel::Hdg
         || col == FreeDvReporterModel::MHz
         || col == FreeDvReporterModel::Snr) {
            const double l = sourceModel()->data(left,  Qt::UserRole).toDouble();
            const double r = sourceModel()->data(right, Qt::UserRole).toDouble();
            return l < r;
        }
        return QSortFilterProxyModel::lessThan(left, right);
    }

private:
    FilterMode m_mode{Band};
    double     m_low{0.0};
    double     m_high{0.0};
    double     m_freqHz{0.0};
};

// ── Band table ─────────────────────────────────────────────────────────────

namespace {
struct Band { const char* label; double low; double high; };
constexpr Band kBands[FreeDvReporterDialog::BandCount] = {
    {"160m",  1.8,   2.0  },
    {"80m",   3.5,   4.0  },
    {"40m",   7.0,   7.3  },
    {"30m",  10.1,  10.2  },
    {"20m",  14.0,  14.35 },
    {"17m",  18.0,  18.2  },
    {"15m",  21.0,  21.45 },
    {"12m",  24.8,  25.0  },
    {"10m",  28.0,  29.8  },
    {"All",   0.0,   0.0  },  // always last — index BandCount-1
};
} // namespace

// ── Constructor ────────────────────────────────────────────────────────────

FreeDvReporterDialog::FreeDvReporterDialog(QWidget* parent)
    : PersistentDialog("FreeDV Reporter", "FreeDvReporterGeometry", parent)
{
    setMinimumSize(700, 350);
    theme::setContainer(this, QStringLiteral("reporter"));
    buildBody();
    restoreSettings();
}

void FreeDvReporterDialog::buildBody()
{
    m_model = new FreeDvReporterModel(this);
    auto* proxy = new FreeDvReporterProxy(this);
    proxy->setSourceModel(m_model);
    m_proxy = proxy;

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(4);

    // ── Table ──────────────────────────────────────────────────────────────
    m_table = new QTableView;
    m_table->setModel(m_proxy);
    m_table->setSortingEnabled(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSortIndicatorShown(true);
    m_table->setShowGrid(true);
    m_table->sortByColumn(FreeDvReporterModel::MHz, Qt::AscendingOrder);

    ThemeManager::instance().applyStyleSheet(m_table,
        "QTableView {"
        "  background-color: {{color.background.0}};"
        "  color: {{color.text.primary}};"
        "  gridline-color: {{color.background.2}};"
        "  selection-background-color: {{color.background.2}};"
        "  selection-color: {{color.text.primary}};"
        "  border: 1px solid {{color.background.2}};"
        "}"
        "QHeaderView::section {"
        "  background-color: {{color.background.2}};"
        "  color: {{color.text.primary}};"
        "  border: 1px solid {{color.background.0}};"
        "  padding: 2px 4px;"
        "}"
    );
    root->addWidget(m_table, 1);

    // ── Separator ──────────────────────────────────────────────────────────
    auto* sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    ThemeManager::instance().applyStyleSheet(sep,
        "color: {{color.border.subtle}};");
    root->addWidget(sep);

    // ── Bottom controls ────────────────────────────────────────────────────
    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(6);

    m_trackCheck = new QCheckBox("Track");
    ThemeManager::instance().applyStyleSheet(m_trackCheck,
        "QCheckBox { color: {{color.text.primary}}; }");
    bottom->addWidget(m_trackCheck);

    m_bandRadio = new QRadioButton("Band");
    m_bandRadio->setChecked(true);
    ThemeManager::instance().applyStyleSheet(m_bandRadio,
        "QRadioButton { color: {{color.text.primary}}; }");
    bottom->addWidget(m_bandRadio);

    m_freqRadio = new QRadioButton("Freq");
    ThemeManager::instance().applyStyleSheet(m_freqRadio,
        "QRadioButton { color: {{color.text.primary}}; }");
    bottom->addWidget(m_freqRadio);

    m_bandGroup = new QButtonGroup(this);
    m_bandGroup->setExclusive(true);

    // Band buttons share a single registered template string; ThemeManager
    // re-resolves on theme change for each registered widget.
    const QString bandBtnStyle =
        "QPushButton {"
        "  background-color: {{color.background.2}};"
        "  color: {{color.text.primary}};"
        "  border: 1px solid {{color.background.2}};"
        "  padding: 2px 6px;"
        "  min-width: 36px;"
        "}"
        "QPushButton:checked {"
        "  background-color: {{color.accent}};"
        "  color: {{color.background.0}};"
        "}"
        "QPushButton:hover {"
        "  background-color: {{color.background.2}};"
        "}";

    for (int i = 0; i < BandCount; ++i) {
        auto* btn = new QPushButton(kBands[i].label);
        btn->setCheckable(true);
        ThemeManager::instance().applyStyleSheet(btn, bandBtnStyle);
        m_bandGroup->addButton(btn, i);
        m_bandBtns.append(btn);
        bottom->addWidget(btn);
        const int idx = i;
        connect(btn, &QPushButton::clicked, this, [this, idx] {
            applyBandFilter(idx);
        });
    }
    // Check "All" by default (last button, index BandCount-1 = 9 → All)
    m_bandBtns.last()->setChecked(true);
    m_activeBandIndex = BandCount - 1;  // "All" is the last entry

    bottom->addStretch();

    auto* closeBtn = new QPushButton("Close");
    ThemeManager::instance().applyStyleSheet(closeBtn,
        "QPushButton {"
        "  background-color: {{color.background.2}};"
        "  color: {{color.text.primary}};"
        "  border: 1px solid {{color.background.2}};"
        "  padding: 2px 10px;"
        "}"
        "QPushButton:hover { background-color: {{color.background.2}}; }"
    );
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::hide);
    bottom->addWidget(closeBtn);

    root->addLayout(bottom);

    // ── Signal wiring ──────────────────────────────────────────────────────
    connect(m_trackCheck, &QCheckBox::toggled,   this, &FreeDvReporterDialog::onTrackToggled);
    connect(m_bandRadio,  &QRadioButton::toggled, this, &FreeDvReporterDialog::onBandModeToggled);
    connect(m_freqRadio,  &QRadioButton::toggled, this, &FreeDvReporterDialog::onFreqModeToggled);

    // Apply the default "All" filter
    applyBandFilter(BandCount - 1);
}

// ── Public interface ───────────────────────────────────────────────────────

void FreeDvReporterDialog::setActiveSlice(SliceModel* slice)
{
    if (m_sliceFreqConn)
        disconnect(m_sliceFreqConn);

    m_slice = slice;
    if (!slice) return;

    m_sliceFreqConn = connect(slice, &SliceModel::frequencyChanged,
                              this,  &FreeDvReporterDialog::onSliceFrequencyChanged);

    // Immediately apply current frequency to tracking
    if (m_trackCheck->isChecked())
        onSliceFrequencyChanged(slice->frequency());
}

void FreeDvReporterDialog::onStationsCleared()
{
    m_model->onStationsCleared();
}

void FreeDvReporterDialog::onStationUpdated(const QString& sid,
                                             const FreeDvClient::StationInfo& info)
{
    m_model->onStationUpdated(sid, info);
}

void FreeDvReporterDialog::onStationRemoved(const QString& sid)
{
    m_model->onStationRemoved(sid);
}

void FreeDvReporterDialog::setMyGrid(const QString& grid)
{
    m_model->setMyGrid(grid);
}

// ── Slot implementations ───────────────────────────────────────────────────

void FreeDvReporterDialog::onSliceFrequencyChanged(double mhz)
{
    if (!m_trackCheck->isChecked()) return;

    if (m_bandRadio->isChecked()) {
        // Find which band this frequency falls in
        for (int i = 0; i < BandCount - 1; ++i) {  // -1 to exclude "All"
            if (mhz >= kBands[i].low && mhz <= kBands[i].high) {
                m_bandBtns[i]->setChecked(true);
                applyBandFilter(i);
                return;
            }
        }
        // Frequency not in any known band — show All
        m_bandBtns[BandCount - 1]->setChecked(true);
        applyBandFilter(BandCount - 1);
    } else {
        applyFreqFilter(mhz * 1e6);
    }
}

void FreeDvReporterDialog::applyBandFilter(int bandIndex)
{
    m_activeBandIndex = bandIndex;
    auto* p = static_cast<FreeDvReporterProxy*>(m_proxy);
    if (bandIndex < 0 || bandIndex >= BandCount) {
        p->clearFilter();
        return;
    }
    p->setBandFilter(kBands[bandIndex].low, kBands[bandIndex].high);
    persistSettings();
}

void FreeDvReporterDialog::applyFreqFilter(double hz)
{
    m_activeFreqHz = hz;
    static_cast<FreeDvReporterProxy*>(m_proxy)->setFreqFilter(hz);
}

void FreeDvReporterDialog::onTrackToggled(bool checked)
{
    if (checked && m_slice)
        onSliceFrequencyChanged(m_slice->frequency());
    persistSettings();
}

void FreeDvReporterDialog::onBandModeToggled(bool checked)
{
    if (!checked) return;
    applyBandFilter(m_activeBandIndex);
    if (m_trackCheck->isChecked() && m_slice)
        onSliceFrequencyChanged(m_slice->frequency());
    persistSettings();
}

void FreeDvReporterDialog::onFreqModeToggled(bool checked)
{
    if (!checked) return;
    if (m_trackCheck->isChecked() && m_slice)
        applyFreqFilter(m_slice->frequency() * 1e6);
    persistSettings();
}

// ── Settings persistence (Principle V) ────────────────────────────────────

void FreeDvReporterDialog::persistSettings() const
{
    QJsonObject obj;
    obj["track"]     = m_trackCheck->isChecked();
    obj["bandMode"]  = m_bandRadio->isChecked();
    obj["bandIndex"] = m_activeBandIndex;
    AppSettings::instance().setValue(
        "FreeDvReporter",
        QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    AppSettings::instance().save();
}

void FreeDvReporterDialog::restoreSettings()
{
    const QString raw = AppSettings::instance().value("FreeDvReporter", "").toString();
    if (raw.isEmpty()) return;

    const QJsonObject obj = QJsonDocument::fromJson(raw.toUtf8()).object();
    if (obj.isEmpty()) return;

    const bool track    = obj.value("track").toBool(false);
    const bool bandMode = obj.value("bandMode").toBool(true);
    const int  bandIdx  = obj.value("bandIndex").toInt(BandCount - 1);

    m_trackCheck->setChecked(track);

    if (bandMode) {
        m_bandRadio->setChecked(true);
    } else {
        m_freqRadio->setChecked(true);
    }

    if (bandIdx >= 0 && bandIdx < BandCount) {
        m_bandBtns[bandIdx]->setChecked(true);
        applyBandFilter(bandIdx);
    }
}

} // namespace AetherSDR

#endif // HAVE_WEBSOCKETS
