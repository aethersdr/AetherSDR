#pragma once

#include <QWidget>
#include <QMap>
#include <QSet>
#include <QSplitter>

namespace AetherSDR {

class BandStackPanel;
class PanFloatingWindow;
class PanadapterApplet;
class SpectrumWidget;

// Vertical stack of N PanadapterApplet instances, each showing an
// independent FFT + waterfall for a different panadapter on the radio.
// Single-pan mode: one applet fills the stack (no visible divider).
class PanadapterStack : public QWidget {
    Q_OBJECT

public:
    explicit PanadapterStack(QWidget* parent = nullptr);

    // Add/remove panadapter displays
    PanadapterApplet* addPanadapter(const QString& panId);
    void removePanadapter(const QString& panId);
    void removeAll();  // remove all applets and reset splitter
    void rekey(const QString& oldId, const QString& newId);

    // Layout: rebuild splitter structure for a given layout ID
    // layoutId: "1", "2v", "2h", "2h1", "12h", "2x2"
    // panIds: the pan IDs to place in order (A, B, C, D)
    void applyLayout(const QString& layoutId, const QStringList& panIds);

    // Accessors
    PanadapterApplet* panadapter(const QString& panId) const;
    SpectrumWidget* spectrum(const QString& panId) const;
    int count() const { return m_pans.size(); }
    QList<PanadapterApplet*> allApplets() const { return m_pans.values(); }

    // Active pan (determines which pan the applet column shows controls for)
    QString activePanId() const { return m_activePanId; }
    PanadapterApplet* activeApplet() const;
    SpectrumWidget* activeSpectrum() const;
    void setActivePan(const QString& panId);
    void setSplitterOrientation(Qt::Orientation o) { m_splitter->setOrientation(o); }
    BandStackPanel* bandStackPanel() const { return m_bandStackPanel; }
    void setBandStackVisible(bool visible);
    void equalizeSizes();
    void rearrangeLayout(const QString& layoutId);

    // Float/dock panadapters
    void floatPanadapter(const QString& panId);
    void dockPanadapter(const QString& panId);
    bool isFloating(const QString& panId) const;

    // Follow the main-window frameless setting for all active floating windows.
    void setFramelessMode(bool on);
    void setShuttingDown(bool on);

    // Persist / restore which pans are currently floating (AppSettings key
    // "FloatingPanIds").  saveFloatingState is called automatically on every
    // float/dock transition and at shutdown; restoreFloatingState is called
    // once after all pans have been added following a radio connect.
    void saveFloatingState() const;
    void restoreFloatingState();

    void prepareShutdown();

signals:
    void activePanChanged(const QString& panId);
    void panFloated(const QString& panId);
    void panDocked(const QString& panId);

private:
    void rebuildDockedSplitter();

    BandStackPanel* m_bandStackPanel{nullptr};
    QSplitter* m_splitter{nullptr};
    QMap<QString, PanadapterApplet*> m_pans;
    QMap<QString, PanFloatingWindow*> m_floatingWindows;
    // Preserve floating state for restored pans that were unavailable this run.
    QSet<QString> m_seenPanIds;
    QString m_activePanId;
    bool m_shutdownPrepared{false};
};

} // namespace AetherSDR
