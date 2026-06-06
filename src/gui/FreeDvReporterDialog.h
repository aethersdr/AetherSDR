#pragma once

#ifdef HAVE_WEBSOCKETS

#include "PersistentDialog.h"
#include "FreeDvReporterModel.h"
#include "core/FreeDvClient.h"

#include <QPointer>
#include <QSortFilterProxyModel>

class QTableView;
class QCheckBox;
class QRadioButton;
class QPushButton;
class QButtonGroup;

namespace AetherSDR {

class SliceModel;

class FreeDvReporterDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit FreeDvReporterDialog(QWidget* parent = nullptr);

    // Called from MainWindow when the active slice changes.
    void setActiveSlice(SliceModel* slice);

public slots:
    void onStationsCleared();
    void onStationUpdated(const QString& sid, const AetherSDR::FreeDvClient::StationInfo& info);
    void onStationRemoved(const QString& sid);
    void setMyGrid(const QString& grid);

private slots:
    void onSliceFrequencyChanged(double mhz);
    void applyBandFilter(int bandIndex);
    void applyFreqFilter(double mhz);
    void onTrackToggled(bool checked);
    void onBandModeToggled(bool checked);
    void onFreqModeToggled(bool checked);

private:
    void buildBody();
    void persistSettings() const;
    void restoreSettings();

public:
    // Total number of band filter buttons (9 named bands + "All").
    // The "All" button is at index BandCount-1.
    static constexpr int BandCount = 10;

private:

    FreeDvReporterModel*     m_model{nullptr};
    QSortFilterProxyModel*   m_proxy{nullptr};

    QTableView*    m_table{nullptr};
    QCheckBox*     m_trackCheck{nullptr};
    QRadioButton*  m_bandRadio{nullptr};
    QRadioButton*  m_freqRadio{nullptr};
    QButtonGroup*  m_bandGroup{nullptr};
    QVector<QPushButton*> m_bandBtns;

    QPointer<SliceModel> m_slice;
    QMetaObject::Connection m_sliceFreqConn;

    int    m_activeBandIndex{BandCount - 1};  // default to "All"
    double m_activeFreqHz{0.0};
};

} // namespace AetherSDR

#endif // HAVE_WEBSOCKETS
