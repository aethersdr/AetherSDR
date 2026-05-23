#pragma once

#include "CtyDatParser.h"
#include "DxccWorkedStatus.h"

#include <QObject>
#include <QColor>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QFileSystemWatcher>

namespace MasterSDR {

struct QsoRecord;
class AdifParser;

// ---------------------------------------------------------------------------
// DxccColorProvider
//
// Owns the CtyDatParser + DxccWorkedStatus.  The single public entry point is
// colorForSpot() — call it from the GUI thread; it's lock-free read-only after
// importAdifFile() completes.
// ---------------------------------------------------------------------------
class DxccColorProvider : public QObject {
    Q_OBJECT

public:
    explicit DxccColorProvider(QObject* parent = nullptr);
    ~DxccColorProvider() override;

    // Load cty.dat from Qt resource (call once at startup).
    bool loadCtyDat(const QString& resourcePath = ":/cty.dat");

    // Asynchronously parse an ADIF file; emits importFinished() when done.
    void importAdifFile(const QString& path);

    // Enable/disable auto-reload when the ADIF file changes on disk.
    // Pass the same path as importAdifFile(); call with on=false to stop watching.
    void setAutoReload(bool on, const QString& path = {});

    // Synchronous query — safe to call from GUI thread after importFinished().
    QColor colorForSpot(const QString& callsign,
                        double freqMhz,
                        const QString& mode) const;

    DxccStatus statusForSpot(const QString& callsign,
                             double freqMhz,
                             const QString& mode) const;

    bool isEnabled()    const { return m_enabled; }
    void setEnabled(bool on) { m_enabled = on; }

    int  qsoCount()    const { return m_workedStatus.totalQsos(); }
    int  entityCount() const { return m_workedStatus.entityCount(); }

    // Configurable colors (loaded/saved via AppSettings externally)
    QColor colorNewDxcc{0xFF, 0x30, 0x30};   // bright red
    QColor colorNewBand{0xFF, 0x8C, 0x00};   // orange
    QColor colorNewMode{0xFF, 0xD7, 0x00};   // gold
    QColor colorWorked {0x60, 0x60, 0x60};   // dim grey

signals:
    // Emitted just before async parsing begins (used to show "Updating…" in UI).
    void importStarted();
    void importFinished(int qsoCount, int entityCount);

private slots:
    void onParseFinished(QVector<QsoRecord> records);
    void onParseFailed(const QString& path);

private:
    // Band/mode helpers (same logic as AdifParser, but for live spot data)
    static QString freqToBand(double mhz);
    static QString normaliseMode(const QString& mode);

    CtyDatParser    m_ctyParser;
    DxccWorkedStatus m_workedStatus;
    bool             m_enabled{false};

    // Worker thread for async ADIF parsing
    QThread     m_parseThread;
    AdifParser* m_parser{nullptr};

    // Auto-reload on file change (2-second debounce)
    QFileSystemWatcher m_fileWatcher;
    QTimer             m_debounceTimer;
    QString            m_watchedPath;
};

} // namespace MasterSDR
