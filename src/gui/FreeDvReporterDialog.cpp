#ifdef HAVE_WEBSOCKETS

#include "FreeDvReporterDialog.h"
#include "FreeDvReporterModel.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "models/SliceModel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QTableView>
#include <QHeaderView>
#include <QCheckBox>
#include <QRadioButton>
#include <QPushButton>
#include <QButtonGroup>
#include <QJsonArray>
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

    // bands: empty vector = "All"; non-empty = OR of all ranges
    void setBandFilters(const QVector<QPair<double,double>>& bands)
    {
        m_mode  = Band;
        m_bands = bands;
        invalidateFilter();
    }

    void setFreqFilter(double hz)
    {
        m_mode   = Freq;
        m_freqHz = hz;
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override
    {
        const QModelIndex mhzIdx = sourceModel()->index(
            sourceRow, FreeDvReporterModel::MHz, sourceParent);
        const double mhz = sourceModel()->data(mhzIdx, Qt::UserRole).toDouble();

        if (m_mode == Band) {
            if (m_bands.isEmpty()) return true;   // "All"
            for (const auto& [lo, hi] : m_bands)
                if (mhz >= lo && mhz <= hi) return true;
            return false;
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
    FilterMode                    m_mode{Band};
    QVector<QPair<double,double>> m_bands;   // empty = "All"
    double                        m_freqHz{0.0};
};

// ── Band table ─────────────────────────────────────────────────────────────

namespace {
struct Band { const char* label; double low; double high; };
constexpr Band kBands[FreeDvReporterDialog::BandCount] = {
    {"160m",   1.8,    2.0   },
    {"80m",    3.5,    4.0   },
    {"40m",    7.0,    7.3   },
    {"30m",   10.1,   10.2   },
    {"20m",   14.0,   14.35  },
    {"17m",   18.0,   18.2   },
    {"15m",   21.0,   21.45  },
    {"12m",   24.8,   25.0   },
    {"10m",   28.0,   29.8   },
    {"6m+",   50.0, 1300.0   },  // 6m through 23cm; mirrors FreeDV-GUI "Other"
    {"All",    0.0,    0.0   },  // always last — index BandCount-1 = 10
};
} // namespace

// ── Constructor ────────────────────────────────────────────────────────────

FreeDvReporterDialog::FreeDvReporterDialog(QWidget* parent)
    : PersistentDialog("FreeDV Reporter", "FreeDvReporterGeometry", parent)
{
    setMinimumSize(780, 350);
    theme::setContainer(this, QStringLiteral("reporter"));
    buildBody();
    restoreSettings();
    m_initializing = false;
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
    m_bandRadio->setToolTip("Ctrl+click band buttons to select multiple bands");
    ThemeManager::instance().applyStyleSheet(m_bandRadio,
        "QRadioButton { color: {{color.text.primary}}; }");
    bottom->addWidget(m_bandRadio);

    m_freqRadio = new QRadioButton("Freq");
    ThemeManager::instance().applyStyleSheet(m_freqRadio,
        "QRadioButton { color: {{color.text.primary}}; }");
    bottom->addWidget(m_freqRadio);

    m_bandGroup = new QButtonGroup(this);
    m_bandGroup->setExclusive(false);

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
            const bool isAllBtn = (idx == BandCount - 1);
            const bool ctrl =
                QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier)
#ifdef Q_OS_MACOS
                || QGuiApplication::keyboardModifiers().testFlag(Qt::MetaModifier)
#endif
                ;

            if (isAllBtn || !ctrl) {
                // Plain click on any button, or Ctrl+click on "All": single-select
                m_activeBandIndices.clear();
                if (!isAllBtn)
                    m_activeBandIndices.insert(idx);
            } else {
                // Ctrl+click on a named band: toggle in/out of active set
                if (m_activeBandIndices.contains(idx))
                    m_activeBandIndices.remove(idx);
                else
                    m_activeBandIndices.insert(idx);
                // Empty set falls through to "All" mode in applyBandFilter()
            }
            applyBandFilter();
        });
    }
    // Default: "All" mode — empty set; last button checked
    m_bandBtns.last()->setChecked(true);

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

    // Apply the default "All" filter (empty set = All)
    applyBandFilter();
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

    // Always update band button highlight regardless of filter mode.
    // In Band mode this also drives the proxy filter; in Freq mode it is
    // visual-only (the proxy filters by exact Hz via applyFreqFilter below).
    m_activeBandIndices.clear();
    for (int i = 0; i < BandCount - 1; ++i) {   // -1 excludes "All"
        if (mhz >= kBands[i].low && mhz <= kBands[i].high) {
            m_activeBandIndices.insert(i);
            break;
        }
    }
    // No band matched → set stays empty → "All" buttons highlight

    if (m_bandRadio->isChecked()) {
        applyBandFilter();          // applies band filter + syncs buttons + saves
    } else {
        syncButtonStates();         // visual only; proxy updated below
        applyFreqFilter(mhz * 1e6);
    }
}

void FreeDvReporterDialog::applyBandFilter()
{
    auto* p = static_cast<FreeDvReporterProxy*>(m_proxy);
    if (m_activeBandIndices.isEmpty()) {
        p->setBandFilters({});   // "All"
    } else {
        QVector<QPair<double,double>> ranges;
        ranges.reserve(m_activeBandIndices.size());
        for (int idx : std::as_const(m_activeBandIndices)) {
            if (idx >= 0 && idx < BandCount - 1)
                ranges.append({kBands[idx].low, kBands[idx].high});
        }
        p->setBandFilters(ranges);
    }
    syncButtonStates();
    persistSettings();
}

void FreeDvReporterDialog::syncButtonStates()
{
    const bool allMode = m_activeBandIndices.isEmpty();
    for (int i = 0; i < BandCount; ++i) {
        const bool isAllBtn = (i == BandCount - 1);
        m_bandBtns[i]->setChecked(isAllBtn ? allMode : m_activeBandIndices.contains(i));
    }
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
    applyBandFilter();
    if (m_trackCheck->isChecked() && m_slice)
        onSliceFrequencyChanged(m_slice->frequency());
    // applyBandFilter() (called above, and inside onSliceFrequencyChanged)
    // already persists — no redundant save needed here
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
    if (m_initializing) return;

    QJsonObject obj;
    obj["track"]       = m_trackCheck->isChecked();
    obj["bandMode"]    = m_bandRadio->isChecked();
    obj["bandVersion"] = 1;

    QJsonArray indices;
    for (int idx : std::as_const(m_activeBandIndices))
        indices.append(idx);
    obj["bandIndices"] = indices;

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

    m_activeBandIndices.clear();

    if (obj.contains("bandVersion")) {
        // New format (v1+): array of selected band indices
        const QJsonArray arr = obj.value("bandIndices").toArray();
        for (const auto& v : arr) {
            const int idx = v.toInt(-1);
            if (idx >= 0 && idx < BandCount - 1)   // ignore "All" pseudo-index if stored
                m_activeBandIndices.insert(idx);
        }
    } else {
        // Old format (10-band, single scalar bandIndex).
        // Indices 0–8 (160m–10m) map 1:1 to the new layout.
        // Old index 9 was "All" → empty set → "All" mode.
        const int oldIdx = obj.value("bandIndex").toInt(BandCount - 1);
        if (oldIdx >= 0 && oldIdx <= 8)
            m_activeBandIndices.insert(oldIdx);
    }

    m_trackCheck->setChecked(track);
    if (bandMode) m_bandRadio->setChecked(true);
    else          m_freqRadio->setChecked(true);

    applyBandFilter();   // applies filter + syncs button checked states
}

} // namespace AetherSDR

#endif // HAVE_WEBSOCKETS
