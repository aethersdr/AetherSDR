#pragma once

#include <QJsonObject>
#include <QObject>
#include <QMap>
#include <QString>
#include <array>

namespace AetherSDR {

// 8-band graphic equalizer model for TX and RX.
//
// Status arrives via "eq txsc mode=1 63Hz=0 125Hz=5 ..." after "sub eq all".
// Commands use "eq TX|RX sc mode=True|False" and "eq TX|RX sc <band>Hz=<val>".
//
// On a radio whose EQ runs on this host there is no status to mirror, so the
// state is the client's to keep: State round-trips through the "EqApplet"
// settings object and comes back through restoreState(), which sends nothing.
class EqualizerModel : public QObject {
    Q_OBJECT

public:
    // Band indices (matching SmartSDR EQ panel order)
    enum Band { B63, B125, B250, B500, B1k, B2k, B4k, B8k, BandCount };

    // Slider travel, and the bound a restored band is clamped to.
    static constexpr int kMinBandDb = -10;
    static constexpr int kMaxBandDb = 10;

    // Everything the operator can set: both enables and all sixteen bands.
    struct State {
        bool txEnabled{false};
        bool rxEnabled{false};
        std::array<int, BandCount> txBands{};  // dB
        std::array<int, BandCount> rxBands{};

        bool operator==(const State&) const = default;
        bool anyEnabled() const { return txEnabled || rxEnabled; }
        bool anyBandNonZero() const;
    };

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

    // ── Client-side persistence ──────────────────────────────────────────────
    State state() const;
    // Silent: adopts `s` and emits tx/rxStateChanged for a side that changed,
    // never commandReady or operatorEdited. A restored value is not a command.
    void restoreState(const State& s);

    // The "graphicEq" member of the EqApplet settings object. Reading tolerates
    // anything: a missing or malformed member yields defaults, bands are clamped.
    static State stateFromJson(const QJsonObject& eqAppletCfg);
    static void stateToJson(const State& s, QJsonObject& eqAppletCfg);

    // AppSettings "EqApplet". Saving keeps the object's other members and writes
    // nothing, returning false, when the stored state already equals `s`.
    static State loadPersistedState();
    static bool savePersistedState(const State& s);

    // Band name for protocol commands (e.g. "63", "1000")
    static QString bandKey(Band b);
    // Short display label (e.g. "63", "1k")
    static QString bandLabel(Band b);

signals:
    void txStateChanged();
    void rxStateChanged();
    void commandReady(const QString& cmd);
    // One of the four setters above ran: the operator, a controller or a
    // shortcut moved the EQ. Status parsing and restoreState() do not emit it.
    void operatorEdited();

private:
    void applyEqStatus(const QMap<QString, QString>& kvs, bool isTx);

    bool m_txEnabled{false};
    bool m_rxEnabled{false};
    std::array<int, BandCount> m_txBands{};  // dB, initialized to 0
    std::array<int, BandCount> m_rxBands{};
};

} // namespace AetherSDR
