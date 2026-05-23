#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <array>

namespace MasterSDR {

// 8-band graphic equalizer model for TX and RX.
//
// Status arrives via "eq txsc mode=1 63Hz=0 125Hz=5 ..." after "sub eq all".
// Commands use "eq TX|RX sc mode=True|False" and "eq TX|RX sc <band>Hz=<val>".
class EqualizerModel : public QObject {
    Q_OBJECT

public:
    // Band indices (matching SmartSDR EQ panel order)
    enum Band { B63, B125, B250, B500, B1k, B2k, B4k, B8k, BandCount };

    explicit EqualizerModel(QObject* parent = nullptr);

    // ── Getters ──────────────────────────────────────────────────────────────
    bool txEnabled() const { return m_txEnabled; }
    bool rxEnabled() const { return m_rxEnabled; }
    int  txBand(Band b) const { return m_txBands[b]; }
    int  rxBand(Band b) const { return m_rxBands[b]; }

    // ── Status parsing (called from RadioModel) ──────────────────────────────
    void applyTxEqStatus(const QMap<QString, QString>& kvs);
    void applyRxEqStatus(const QMap<QString, QString>& kvs);

    // ── Commands (emit commandReady) ─────────────────────────────────────────
    void setTxEnabled(bool on);
    void setRxEnabled(bool on);
    void setTxBand(Band b, int dB);
    void setRxBand(Band b, int dB);

    // Band name for protocol commands (e.g. "63", "1000")
    static QString bandKey(Band b);
    // Short display label (e.g. "63", "1k")
    static QString bandLabel(Band b);

signals:
    void txStateChanged();
    void rxStateChanged();
    void commandReady(const QString& cmd);

private:
    void applyEqStatus(const QMap<QString, QString>& kvs, bool isTx);

    bool m_txEnabled{false};
    bool m_rxEnabled{false};
    std::array<int, BandCount> m_txBands{};  // dB, initialized to 0
    std::array<int, BandCount> m_rxBands{};
};

} // namespace MasterSDR
