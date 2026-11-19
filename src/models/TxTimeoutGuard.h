#pragma once

#include <QJsonObject>
#include <QString>
#include <QtGlobal>

#include <functional>
#include <map>

namespace AetherSDR {

// The host transmit time-out (RFC #7021), stored per radio as the
// "TxTimeout" feature document.
struct TxTimeoutSettings {
    static constexpr int kSchemaVersion = 1;
    static constexpr int kDefaultTimeoutS = 180;
    static constexpr int kMinimumTimeoutS = 15;
    static constexpr int kMaximumTimeoutS = 1800;
    static constexpr int kDefaultWarningS = 10;
    static constexpr int kMaximumWarningS = 60;

    bool enabled{true};
    int timeoutS{kDefaultTimeoutS};
    int warningS{kDefaultWarningS}; // 0 = no warning

    // Defaults for a radio with no stored document: never shorter than the
    // radio's own interlock timeout, so the host timer is the second layer.
    [[nodiscard]] static TxTimeoutSettings defaultsFor(int radioTimeoutMs);
    [[nodiscard]] static TxTimeoutSettings fromJson(const QJsonObject& doc,
                                                    const TxTimeoutSettings& fallback);
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] TxTimeoutSettings clamped() const;
    bool operator==(const TxTimeoutSettings&) const = default;
};

// Arms on the radio's keyed edge, whatever keyed it, and reports when the
// over has run too long. It has no key-on path: the owner stops TX on Expired.
// After an expiry it latches until every input that held a key has released
// and the radio is idle; the owner refuses every key-down while latched().
class TxTimeoutGuard {
public:
    using Clock = std::function<qint64()>;
    enum class Event { None, Warning, Expired };
    static constexpr int kMaximumHolders = 32;

    explicit TxTimeoutGuard(Clock clock);

    void setClock(Clock clock) { m_clock = std::move(clock); }
    void setSettings(const TxTimeoutSettings& settings);
    [[nodiscard]] const TxTimeoutSettings& settings() const { return m_settings; }

    void setTransmitting(bool on, const QString& source);
    [[nodiscard]] Event poll();

    [[nodiscard]] bool armed() const { return m_armed; }
    [[nodiscard]] bool transmitting() const { return m_transmitting; }
    [[nodiscard]] QString source() const { return m_source; }
    [[nodiscard]] qint64 remainingMs() const;
    [[nodiscard]] int lastExpiredAfterS() const { return m_lastExpiredAfterS; }

    // An input that holds a key. `alive` reports whether its lifetime still
    // exists; a dead holder counts as released. False when the table is full
    // of live holders: the owner must refuse that key-down (fail closed).
    [[nodiscard]] bool noteHold(quintptr key, std::function<bool()> alive);
    // Whether noteHold(key) would record it; prunes dead holders to make room.
    [[nodiscard]] bool canHold(quintptr key);
    void noteRelease(quintptr key);
    // Inputs the owner knows hold nothing (no live intent, radio idle).
    void forgetHolders();
    [[nodiscard]] int holderCount() const { return static_cast<int>(m_holders.size()); }

    [[nodiscard]] bool latched() const { return m_latched; }
    // Prunes dead holders and clears the latch once nothing holds and the
    // radio is idle. Returns true when the latch changed.
    bool refreshLatch();
    void reset();

private:
    Clock m_clock;
    TxTimeoutSettings m_settings;
    bool m_transmitting{false};
    bool m_armed{false};
    bool m_warned{false};
    bool m_latched{false};
    qint64 m_startedMs{0};
    int m_lastExpiredAfterS{0};
    QString m_source;
    std::map<quintptr, std::function<bool()>> m_holders;
};

} // namespace AetherSDR
