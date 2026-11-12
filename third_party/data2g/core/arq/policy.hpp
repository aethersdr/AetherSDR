// data2g/arq/policy.py: the gear shifter (docs/arq.md §8), one per station.
// GearShifter is the model; GearPolicy (below) is its link.Policy, the
// methods policy.py's GearShifter has for the link and the session.
// policy.py's study toggles are Python only: here their defaults (no
// dropped modes, BIAS_MAX 6; noise_rule and cpm_floor_on are fields).
#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "arq/link.hpp"
#include "arq/modes.hpp"
#include "arq/predictor.hpp"
#include "arq/session.hpp"  // LINK_LOST_S

namespace data2g::arq {

inline constexpr std::array<double, 4> SIZE_S = {1.0, 3.0, 6.0, 12.0};
inline constexpr double TURN_S = 1.3;
inline constexpr double TIMEOUT_S = 1.0 + 1.0 + 1.5;
inline constexpr double PREV_MAX_S = 30.0;
inline constexpr double BIAS_STEP = 1.0, BIAS_MAX = 6.0;
inline constexpr double DUP_BELOW = 0.9;
inline constexpr double CPM_SIZE_SCALE = 4.0;
inline constexpr double CPM_MAX_S = 24.0;  // ... but at most this long
inline constexpr double T_RECOVER_S = TIMEOUT_S + 2 * 5.0;  // a timeout, a robust poll and its reply
inline constexpr double LOST_LINK_COST_S = 300.0;
inline constexpr double REPLY_HOLD_MARGIN_S = 0.5;
inline constexpr double NOISE_RULE = 1.0;  // the noise rule's tail weight (policy.py NOISE_RULE)
// the data ladder (policy.py): after LADDER_AFTER data bursts lost in a row,
// data only in modes LADDER_STEP_DB more robust on every channel
inline constexpr int LADDER_AFTER = 2;
inline constexpr double LADDER_STEP_DB = 3.0;
inline constexpr std::size_t ENERGY_HIST = 4;  // the energy inputs' window: the peer's last bursts
// The CPM floor (policy.py CPM_FLOOR, default snr:-4:1): data in CPM modes only
// while the median snr_est of the peer's last CPM_FLOOR_HIST bursts is under
// CPM_FLOOR_SNR_DB and their median spread_est at least CPM_FLOOR_SPREAD_HZ.
inline constexpr std::size_t CPM_FLOOR_HIST = 3;
inline constexpr double CPM_FLOOR_SNR_DB = -4.0;
inline constexpr double CPM_FLOOR_SPREAD_HZ = 1.0;
inline constexpr double UNMEASURED_SPREAD_HZ = 2.0;  // equalizer::DEFAULT_SPREAD_HZ (policy.py)
// per mode, its 10% codeword failure SNR on awgn, mpg, mpp, mpd; throws for an unknown mode
const std::array<double, 4>& mode_thresholds(std::string_view submode);
inline constexpr int CTL_BYTES = 12;
inline constexpr std::string_view ALT_POLL = "n4-ack-8f";  // escalation 2's mode
inline constexpr int ROBUST_ESCALATION = 4;  // from here on ROBUST_CONNECT
inline constexpr std::string_view ROBUST_CONNECT = "fsk16r25-r1/2";

std::string_view fallback(int cap);
std::string_view connect_mode(int cap, int tries = 0);
int ctl_slots(const Mode& m);
int slots_for(const Mode& m, double seconds, bool data = true, bool dup = false);

// What the shifter reads from its link.Station (policy.py StationView, which
// also carries the timeouts its Python-only link history reads).
struct StationView {
    int cap = 2;
    bool pending = true;  // tx.pending()
    std::optional<int> peer_recommend, peer_reply_recommend;
    int peer_size_hint = 1;
    bool peer_wants_dup = false;
    bool chat = false;     // station.chat or station.peer_chat
    std::int64_t peer_queued = 0;
    std::int64_t held = 0;         // codewords held beyond the cumulative ACK: len(rx.buf)
    int misses = 0;
    int esc_floor = 0;
    bool last_rx_data = true;  // the peer's last burst carried data (expected_reply)
};

struct GearRecommendation {  // link.hpp has Recommendation (the Policy's)
    int data, hint, reply;
};

class GearShifter {
public:
    struct Heard {
        Measured m;
        std::string band;
        double at = 0.0;
    };
    struct LogEntry {
        std::string data;
        int hint;
        std::string reply;
    };
    using Map = std::map<std::string, double, std::less<>>;

    std::optional<double> noise_rule = NOISE_RULE;  // nullopt: off (DATA2G_NOISE_RULE=off)
    double gap_s = 2.5;
    bool use_cpm = true;
    double min_success = 0.0;
    std::optional<Measured> measured;  // the peer's last burst
    std::string measured_band = "w";
    double measured_at = 0.0;
    std::optional<std::string> heard;  // the submode of the peer's last burst
    int data_lost = 0;  // bursts lost in a row in the data mode I recommended (LADDER_AFTER)
    std::optional<std::array<double, 4>> ceiling;  // the data ladder: per channel, the highest threshold data may have
    std::optional<std::array<double, 4>> ladder_top;  // the thresholds of the mode whose losses started it
    bool ladder_left = false;  // just climbed off the ladder: one data burst lost puts it back
    std::optional<Heard> prev;  // the peer burst before the last
    Map bias, bias_burst;
    bool want_dup = false;
    std::map<std::string, std::pair<double, double>, std::less<>> predicted;
    bool peer_had_data = true;
    std::vector<LogEntry> log;
    std::vector<double> spreads;  // the peer's last bursts' spread_est (tables::OUTCOME_GATE_HIST)
    // the peer's last measured (not defaulted) spread_est: a defaulted reading
    // takes it instead (policy.py SPREAD_CARRY)
    std::optional<double> last_spread;
    std::vector<double> energies;  // the peer's last ENERGY_HIST bursts' energy SNR, dB (policy.py)
    std::vector<double> snrs;  // the peer's last CPM_FLOOR_HIST bursts' snr_est (the CPM floor)
    bool cpm_floor_on = true;  // DATA2G_CPM_FLOOR='' (Python, studies) turns it off

    std::pair<std::string_view, int> choose(const StationView& st, int escalation) const;
    int next_capacity(const StationView& st) const;
    void observe(const Measured& m, std::string_view submode, double now);
    // usable: nullopt (KISS: no control) = any codeword decoded.
    void outcome(std::string_view submode, int decoded, int sent, std::optional<bool> usable = std::nullopt);
    GearRecommendation recommend(const StationView& st);
    // the gate's model applies (policy.py GearShifter.gate)
    // 0 the installed model, 1 the gate's, 2 the fast region's
    int gate() const;
    // the CPM floor holds (policy.py GearShifter.cpm_floor)
    bool cpm_floor() const;
    void observe_energy(double snr_db, double now);
    // the energy inputs (mean dB in power, fill, 1); nullopt before any
    std::optional<std::array<double, 3>> energy_features() const;
    // the burst I asked the peer for last: its mode and seconds on air
    std::optional<std::pair<std::string, double>> expected_reply() const;
    // seconds past t_turn to wait for a reply to a burst in `submode`
    double reply_hold(const StationView& st, std::string_view submode) const;
};

StationView view(const Station& st);

// The engine's default policy: a GearShifter as link.Policy, the methods
// policy.py's GearShifter has for the link and the session.
class GearPolicy : public Policy {
public:
    GearShifter shifter;

    std::pair<std::string, int> choose(Station& st, int escalation) override;
    int payload_bytes(const std::string& submode) override;
    int ctl_payload_bytes(const std::string& submode) override;
    int max_ctl(const std::string& submode) override;
    std::optional<Recommendation> recommend(Station& st) override;
    bool want_dup() override { return shifter.want_dup; }
    int rv_cycle(const std::string& submode) override;
    bool has_outcome() override { return true; }
    void outcome(const std::string& submode, int decoded, int sent, bool usable) override {
        shifter.outcome(submode, decoded, sent, usable);
    }
    std::string mode_name(int rec) override;
    bool has_airtime() override { return true; }
    std::optional<double> snr_est() override;
    std::string measured_extra() override;
    double airtime(const std::string& submode, int n_cw, bool dup) override;
    std::string connect_mode(int cap, int tries) override;
    std::optional<double> reply_hold(Station& st, const TxBurst& burst) override {
        return shifter.reply_hold(view(st), burst.submode);
    }
    void observe(const Measured& m, const std::string& submode, double now) override { shifter.observe(m, submode, now); }
    void observe_energy(double snr_db, double now) override { shifter.observe_energy(snr_db, now); }
    std::optional<std::pair<std::string, double>> expected_reply() const override { return shifter.expected_reply(); }
    std::optional<int> next_capacity(const Station& st) const override { return shifter.next_capacity(view(st)); }
};

}  // namespace data2g::arq
