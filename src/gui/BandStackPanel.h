#pragma once

#include "core/BandStackSettings.h"

#include <QWidget>
#include <QVector>
#include <QPushButton>
#include <QLabel>

class QVBoxLayout;
class QScrollArea;

namespace MasterSDR {

class BandPlanManager;

// Vertical strip (80px) of frequency bookmarks alongside the panadapter.
// Each bookmark shows the frequency, color-coded by band plan segment.
// Click to recall, right-click to delete, "+" to add current position.
// Supports optional band grouping and auto-expiry (#1471).
class BandStackPanel : public QWidget {
    Q_OBJECT

public:
    explicit BandStackPanel(QWidget* parent = nullptr);

    // Load all bookmarks for a radio, looking up colors from band plan
    void loadBookmarks(const QString& radioSerial, const BandPlanManager* bpm);

    // Clear all bookmarks (on disconnect)
    void clear();

    // Look up band plan segment color for a frequency
    static QColor colorForFrequency(double freqMhz, const BandPlanManager* bpm);

    // Grouping and auto-expiry UI state
    void setGrouped(bool grouped);
    void setAutoExpiryMinutes(int minutes);
    void setAutoSaveDwellSeconds(int seconds);

signals:
    void addRequested();
    void recallRequested(const BandStackEntry& entry);
    void removeRequested(int index);
    void clearAllRequested();
    void clearBandRequested(double lowMhz, double highMhz);
    void groupByBandChanged(bool grouped);
    void autoExpiryChanged(int minutes);
    void autoSaveDwellChanged(int seconds);

private:
    void rebuildLayout();
    QLabel* createBandHeader(const QString& bandName, double lowMhz, double highMhz);
    static QString bandNameForFrequency(double freqMhz);

    struct Bookmark {
        BandStackEntry entry;
        QColor color;
        int settingsIndex{-1};   // index in BandStackSettings storage
        QPushButton* button{nullptr};
    };

    QVector<Bookmark> m_bookmarks;
    QVector<QLabel*> m_bandHeaders;
    QVBoxLayout* m_buttonLayout{nullptr};
    QScrollArea* m_scrollArea{nullptr};
    QPushButton* m_addButton{nullptr};
    QPushButton* m_clearAllButton{nullptr};
    QPushButton* m_settingsButton{nullptr};
    bool m_grouped{false};
    int m_autoExpiryMinutes{0};
    int m_autoSaveDwellSeconds{0};
};

} // namespace MasterSDR
