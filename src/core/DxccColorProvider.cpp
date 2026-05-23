#include "DxccColorProvider.h"
#include "AdifParser.h"

#include <QFileInfo>
#include <QMetaObject>

namespace MasterSDR {

DxccColorProvider::DxccColorProvider(QObject* parent)
    : QObject(parent)
{
    m_parser = new AdifParser;
    m_parser->setCtyParser(&m_ctyParser);
    m_parser->moveToThread(&m_parseThread);
    connect(m_parser, &AdifParser::finished,
            this,     &DxccColorProvider::onParseFinished,
            Qt::QueuedConnection);
    connect(m_parser, &AdifParser::openFailed,
            this,     &DxccColorProvider::onParseFailed,
            Qt::QueuedConnection);
    m_parseThread.start();

    // Debounce timer: fires 2 seconds after the last file-changed notification.
    // Re-adding the path here (rather than in fileChanged) ensures we register
    // the *new* inode when a logger atomically replaces the file via rename —
    // by the time the 2-second window expires the replacement file is present.
    m_debounceTimer.setSingleShot(true);
    m_debounceTimer.setInterval(2000);
    connect(&m_debounceTimer, &QTimer::timeout, this, [this]() {
        if (m_watchedPath.isEmpty()) return;
        // If the file no longer exists (deleted, not just locked), don't attempt
        // a parse — the directory watcher will re-arm us when it reappears.
        if (!QFileInfo::exists(m_watchedPath)) return;
        // Re-arm: atomic file replacement (temp→final rename) changes the inode
        // so the watcher loses the file.  Add the path again before importing
        // so subsequent changes to the newly-written file are also caught.
        if (!m_fileWatcher.files().contains(m_watchedPath))
            m_fileWatcher.addPath(m_watchedPath);
        importAdifFile(m_watchedPath);
    });

    connect(&m_fileWatcher, &QFileSystemWatcher::fileChanged,
            this, [this](const QString&) {
        // Just (re)start the debounce window.  Path re-registration happens in
        // the timeout handler once the replacement file is guaranteed present.
        m_debounceTimer.start();
    });

    // Also watch the parent directory so we catch the case where the file is
    // deleted and a new file is later dropped/exported to the same path.
    // QFileSystemWatcher can only watch files that exist — once the file is
    // gone and addPath() fails in the debounce handler, the file watcher goes
    // dark.  The directory watcher stays alive regardless, and fires whenever
    // anything in the folder changes (create, rename, delete).  We check
    // whether our target file has (re)appeared and re-arm the file watcher.
    connect(&m_fileWatcher, &QFileSystemWatcher::directoryChanged,
            this, [this](const QString&) {
        if (m_watchedPath.isEmpty()) return;
        if (QFileInfo::exists(m_watchedPath) &&
            !m_fileWatcher.files().contains(m_watchedPath)) {
            // File has reappeared — start watching it again and reload.
            m_fileWatcher.addPath(m_watchedPath);
            m_debounceTimer.start();
        }
    });
}

DxccColorProvider::~DxccColorProvider()
{
    m_parseThread.quit();
    m_parseThread.wait();
    delete m_parser;
}

bool DxccColorProvider::loadCtyDat(const QString& resourcePath)
{
    return m_ctyParser.loadFromResource(resourcePath);
}

void DxccColorProvider::importAdifFile(const QString& path)
{
    emit importStarted();
    QMetaObject::invokeMethod(m_parser, "parseFileAsync",
                              Qt::QueuedConnection,
                              Q_ARG(QString, path));
}

void DxccColorProvider::setAutoReload(bool on, const QString& path)
{
    // Remove any existing watched file and directory paths
    if (!m_fileWatcher.files().isEmpty())
        m_fileWatcher.removePaths(m_fileWatcher.files());
    if (!m_fileWatcher.directories().isEmpty())
        m_fileWatcher.removePaths(m_fileWatcher.directories());
    m_debounceTimer.stop();

    if (on && !path.isEmpty()) {
        m_watchedPath = path;
        m_fileWatcher.addPath(path);  // watch the file (exists now)
        // Watch the parent directory so we catch delete-then-recreate:
        // if the file is removed and a new one dropped in later, the directory
        // watcher fires and re-arms the file watcher when the file reappears.
        const QString dir = QFileInfo(path).absolutePath();
        m_fileWatcher.addPath(dir);
    } else {
        m_watchedPath.clear();
    }
}

void DxccColorProvider::onParseFinished(QVector<QsoRecord> records)
{
    // DXCC prefixes are now pre-resolved on the worker thread by
    // parseFileAsync(), so we only need the cheap hash-insert pass.
    m_workedStatus.load(records);
    emit importFinished(m_workedStatus.totalQsos(), m_workedStatus.entityCount());
}

void DxccColorProvider::onParseFailed(const QString& /*path*/)
{
    // The file could not be opened — either locked by an external logger or
    // deleted between the existence check and the open attempt.
    //
    // Either way, keep the existing worked status intact.
    //
    // Only re-arm the debounce timer if the file still exists (locked case):
    // we'll retry in 2 s and should succeed once the lock is released.
    // If the file is gone (deleted case), don't loop — the directory watcher
    // will fire when the file reappears and re-arm us at that point.
    if (!m_watchedPath.isEmpty() && QFileInfo::exists(m_watchedPath))
        m_debounceTimer.start();

    // Emit importFinished so any "Updating…" UI label clears to the current counts.
    emit importFinished(m_workedStatus.totalQsos(), m_workedStatus.entityCount());
}

// ---------------------------------------------------------------------------
// Spot queries
// ---------------------------------------------------------------------------

QString DxccColorProvider::freqToBand(double mhz)
{
    if (mhz >= 1.8   && mhz < 2.0)   return "160m";
    if (mhz >= 3.5   && mhz < 4.0)   return "80m";
    if (mhz >= 5.0   && mhz < 5.6)   return "60m";
    if (mhz >= 7.0   && mhz < 7.3)   return "40m";
    if (mhz >= 10.1  && mhz < 10.15) return "30m";
    if (mhz >= 14.0  && mhz < 14.35) return "20m";
    if (mhz >= 18.068&& mhz < 18.168)return "17m";
    if (mhz >= 21.0  && mhz < 21.45) return "15m";
    if (mhz >= 24.89 && mhz < 24.99) return "12m";
    if (mhz >= 28.0  && mhz < 29.7)  return "10m";
    if (mhz >= 50.0  && mhz < 54.0)  return "6m";
    if (mhz >= 70.0  && mhz < 70.5)  return "4m";
    if (mhz >= 144.0 && mhz < 148.0) return "2m";
    if (mhz >= 430.0 && mhz < 440.0) return "70cm";
    return {};
}

// Infer a mode group from frequency using IARU Region 1/2 band-plan segments.
// Used when a spot carries no explicit mode field.
static QString inferModeFromFreq(double mhz)
{
    // Each entry: { lo, hi, modeGroup }
    static const struct { double lo, hi; const char* mg; } segs[] = {
        // 160m
        {1.800, 1.838, "CW"},    {1.838, 1.840, "DATA"}, {1.840, 2.000, "PHONE"},
        // 80m
        {3.500, 3.570, "CW"},    {3.570, 3.600, "DATA"}, {3.600, 4.000, "PHONE"},
        // 60m  (data/phone only)
        {5.000, 5.060, "DATA"},  {5.060, 5.600, "PHONE"},
        // 40m
        {7.000, 7.040, "CW"},    {7.040, 7.100, "DATA"}, {7.100, 7.300, "PHONE"},
        // 30m  (CW + data only)
        {10.100,10.130,"CW"},    {10.130,10.150,"DATA"},
        // 20m
        {14.000,14.070,"CW"},    {14.070,14.112,"DATA"}, {14.112,14.350,"PHONE"},
        // 17m
        {18.068,18.095,"CW"},    {18.095,18.110,"DATA"}, {18.110,18.168,"PHONE"},
        // 15m
        {21.000,21.070,"CW"},    {21.070,21.150,"DATA"}, {21.150,21.450,"PHONE"},
        // 12m
        {24.890,24.915,"CW"},    {24.915,24.930,"DATA"}, {24.930,24.990,"PHONE"},
        // 10m
        {28.000,28.070,"CW"},    {28.070,28.300,"DATA"}, {28.300,29.700,"PHONE"},
        // 6m
        {50.000,50.100,"CW"},    {50.100,50.400,"DATA"}, {50.400,54.000,"PHONE"},
        // 4m
        {70.000,70.100,"CW"},    {70.100,70.200,"DATA"}, {70.200,70.500,"PHONE"},
        // 2m
        {144.000,144.060,"CW"},  {144.060,144.175,"DATA"},{144.175,148.000,"PHONE"},
        // 70cm
        {430.000,430.150,"CW"},  {430.150,430.600,"DATA"},{430.600,440.000,"PHONE"},
    };
    for (const auto& s : segs)
        if (mhz >= s.lo && mhz < s.hi) return s.mg;
    return "PHONE";  // default for anything outside defined segments
}

QString DxccColorProvider::normaliseMode(const QString& mode)
{
    const QString m = mode.toUpper();
    if (m == "CW")   return "CW";
    if (m == "SSB" || m == "USB" || m == "LSB" || m == "AM" || m == "FM" || m == "PHONE")
        return "PHONE";
    // FT8, RTTY, PSK, etc. -> DATA
    return "DATA";
}

DxccStatus DxccColorProvider::statusForSpot(const QString& callsign,
                                            double freqMhz,
                                            const QString& mode) const
{
    const QString prefix = m_ctyParser.resolvePrimaryPrefix(callsign);
    if (prefix.isEmpty()) return DxccStatus::Unknown;

    const QString band = freqToBand(freqMhz);
    if (band.isEmpty()) return DxccStatus::Unknown;

    // If the spot carries no mode, infer it from frequency using the IARU band plan.
    // Defaulting unknown modes to DATA would mask phone/CW needs for active FT8 operators.
    const QString mg = mode.trimmed().isEmpty() ? inferModeFromFreq(freqMhz) : normaliseMode(mode);
    return m_workedStatus.query(prefix, band, mg);
}

QColor DxccColorProvider::colorForSpot(const QString& callsign,
                                       double freqMhz,
                                       const QString& mode) const
{
    switch (statusForSpot(callsign, freqMhz, mode)) {
        case DxccStatus::NewDxcc:  return colorNewDxcc;
        case DxccStatus::NewBand:  return colorNewBand;
        case DxccStatus::NewMode:  return colorNewMode;
        case DxccStatus::Worked:   return colorWorked;
        default:                   return {};  // Unknown — use default spot colour
    }
}

} // namespace MasterSDR
