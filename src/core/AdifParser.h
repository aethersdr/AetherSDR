#pragma once

#include <QString>
#include <QVector>
#include <QObject>

namespace AetherSDR {

class CtyDatParser;  // forward declaration for optional DXCC resolution

struct QsoRecord {
    QString callsign;
    QString band;       // "80m", "40m", etc. (normalised)
    QString modeGroup;  // "CW", "PHONE", "DATA"
    QString dxccPrefix; // filled by DxccColorProvider after callsign resolution
};

// ---------------------------------------------------------------------------
// AdifParser
//
// Parses .adi / .adif log files into QsoRecord vectors.
// Can be used synchronously (parseFile) or from a worker thread.
// ---------------------------------------------------------------------------
class AdifParser : public QObject {
    Q_OBJECT

public:
    explicit AdifParser(QObject* parent = nullptr) : QObject(parent) {}

    // Synchronous — returns records directly.
    static QVector<QsoRecord> parseFile(const QString& path);

    // Async — call this slot (e.g. via QMetaObject::invokeMethod on a worker
    // thread).  Emits finished() when done.
    Q_INVOKABLE void parseFileAsync(const QString& path);

    // Set the CtyDatParser used for DXCC prefix resolution on the worker
    // thread.  Must be called before parseFileAsync() and must outlive this
    // object.  The parser is read-only after loadCtyDat() so cross-thread
    // access is safe.
    void setCtyParser(const CtyDatParser* ctyParser) { m_ctyParser = ctyParser; }

signals:
    void finished(QVector<QsoRecord> records);
    // Emitted when the file cannot be opened after all retry attempts
    // (e.g. locked by an external logger).  The existing worked status
    // should be preserved and the caller may schedule a later retry.
    void openFailed(QString path);

private:
    static QVector<QsoRecord> parse(const QByteArray& data);
    static QString normaliseMode(const QString& mode, const QString& submode);
    static QString freqToBand(double mhz);

    const CtyDatParser* m_ctyParser{nullptr};
};

} // namespace AetherSDR
