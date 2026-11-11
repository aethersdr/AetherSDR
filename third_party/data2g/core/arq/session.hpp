// data2g/arq/session.py: connect, disconnect and timers around Station
// (docs/arq.md §6, §7). Driven by its caller's clock; nothing sleeps:
//
//     on_header(submode, n_cw, now)   a burst header was decoded
//     on_rx(rx, now)                  a whole burst arrived
//     poll(now) -> TxBurst or null    anything to send now?
//     on_tx_end(burst, now)           that burst finished going out
#pragma once

#include <cstdint>
#include <limits>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "arq/link.hpp"

namespace data2g::arq {

inline constexpr int VERSION = 5;  // session.py VERSION: 4 added T_INLINE, 5 the CW ID ahead of a turn
inline constexpr int CONNECT_TRIES = 5;
inline constexpr int CONNECT_CTL_BYTES = 4 + 2 + Connect::BYTES;  // a CONNECT's Control: core, T_SESS header, body
inline constexpr int DISC_TRIES = 3;
inline constexpr double REPLY_START_S = 2.5;  // data2g/arq/session.py
inline constexpr double REPEAT_GAP_S = 30.0;  // failed connect frames further apart are not one CONNECT's tries
inline constexpr double IDLE_CLOSE_S = 300.0;
inline constexpr double KEEPALIVE_LO_S = 15.0, KEEPALIVE_HI_S = 30.0;
inline constexpr double CHAT_KEEPALIVE_LO_S = KEEPALIVE_LO_S, CHAT_KEEPALIVE_HI_S = KEEPALIVE_HI_S;
inline constexpr bool KEEPALIVE_DOUBLING = false;
inline constexpr double LINK_LOST_S = 90.0;
inline constexpr double WAKE_GUARD_S = 1.0;
inline constexpr double WAKE_JITTER_S = 1.0;
inline constexpr int WAKE_TRIES = 2;
inline constexpr int CHAT_WAKE_TRIES = 6;
inline constexpr double COALESCE_S = 0.1;  // CHAT OFF: a short write to an idle link waits this long for more
// callee: a reply to a burst that brought the host data and ended the
// caller's (T_DRAINED) waits this long for its answer; either station's next
// burst, when CLEANTXBUFFER's limbo has just settled (session.py)
inline constexpr double REPLY_HOLD_S = 0.25;
inline constexpr double REPEAT_MAX_S = 3.0;
// CONNECT_NAK reasons: the caller stops dialing on any of them
inline constexpr int NAK_VERSION = 1, NAK_NOT_LISTENING = 2, NAK_BUSY = 3, NAK_REFUSED = 4;
const char* nak_name(int reason);  // nullptr for an unknown one

// random.Random's two draws the session makes. Not bit-exact with Python's
// by default; a binding passes Python's own generator to keep seeded runs.
class Rng {
public:
    virtual ~Rng() = default;
    virtual std::int64_t randrange(std::int64_t n) = 0;  // [0, n)
    virtual double uniform(double a, double b) = 0;
};

class DefaultRng : public Rng {
public:
    explicit DefaultRng(std::uint64_t seed = std::random_device{}()) : g_(seed) {}
    std::int64_t randrange(std::int64_t n) override { return std::uniform_int_distribution<std::int64_t>(0, n - 1)(g_); }
    double uniform(double a, double b) override { return a + (b - a) * std::uniform_real_distribution<double>(0, 1)(g_); }

private:
    std::mt19937_64 g_;
};

enum class SessionState { IDLE, LISTEN, CONNECTING, CONNECTED, DISCONNECTING, CLOSED };
const char* state_name(SessionState s);  // as session.py's strings

// Nonzero 16-bit key; 0 means no session (connect frames).
int session_key(const std::string& caller, const std::string& callee, std::int64_t nonce);
std::string frame_desc(ByteView body);

// session.py's module constants that a study patches (scripts/idle_study.py's
// variants). Defaults are the constants; a binding copies Python's values in
// when it makes a Session (session.py reads them at each use).
struct SessionTuning {
    int connect_tries = CONNECT_TRIES, disc_tries = DISC_TRIES;
    double reply_start_s = REPLY_START_S, idle_close_s = IDLE_CLOSE_S;
    double keepalive_lo_s = KEEPALIVE_LO_S, keepalive_hi_s = KEEPALIVE_HI_S;
    double chat_keepalive_lo_s = CHAT_KEEPALIVE_LO_S, chat_keepalive_hi_s = CHAT_KEEPALIVE_HI_S;
    bool keepalive_doubling = KEEPALIVE_DOUBLING;
    double link_lost_s = LINK_LOST_S, wake_guard_s = WAKE_GUARD_S, wake_jitter_s = WAKE_JITTER_S;
    int wake_tries = WAKE_TRIES, chat_wake_tries = CHAT_WAKE_TRIES;
    double repeat_max_s = REPEAT_MAX_S;
};

class Session {
public:
    Session(std::string call, std::shared_ptr<Policy> policy, double t_turn = 1.0, std::shared_ptr<Rng> rng = nullptr,
            std::vector<std::string> aliases = {}, double stats_interval_s = 60.0);
    virtual ~Session() = default;

    std::string call;
    std::shared_ptr<Policy> policy;
    double t_turn;
    std::shared_ptr<Rng> rng;
    SessionState state = SessionState::IDLE;
    std::string peer;
    int cap = 2;
    std::shared_ptr<Station> station;
    std::vector<std::string> events;  // host notifications, oldest first
    std::string close_reason;
    bool chat = false;
    std::vector<std::string> aliases;
    // before a CONNECT is answered: false refuses it (host ACCEPT)
    std::function<bool(const std::string& caller, const std::string& callee)> accept_connect;
    double stats_interval_s;
    SessionTuning tune;

    std::int64_t nonce = 0;
    TxBurstPtr out;
    TxBurstPtr nak;  // a refusal in out: not the session's burst (no reply timer, no wake)
    std::string tx_mode;  // the mode of my last burst sent (expecting)
    // The modes my peer's next burst likely comes in while one is due (session.py's expecting, §3a).
    std::vector<std::string> expecting() const;
    std::optional<double> due, deadline, build_at;
    std::optional<double> coalesce_until;  // write(): an idle poll or wake held for more data
    std::optional<std::string> answer_mode;
    int tries = 0;
    double last_heard = 0, last_data = 0, idle_wait = 0;
    double quiet_from = 0, wake_wait = 0;
    int wakes = 0;
    bool want_disc = false, sent_disc_ack = false;
    std::string disc_why = "CLIENT";  // who wanted the DISC: CLIENT or IDLE
    Bytes pending_write;
    double now_ = 0, stats_since = 0, connected_at = 0;
    std::map<std::string, std::int64_t> stats_prev;

    // host side
    void set_chat(bool on);
    void listen() { state = SessionState::LISTEN; }
    void connect(const std::string& peer, int cap, double now);
    void disconnect();
    void write(ByteView data);
    Bytes read();
    // bytes the host wrote that are in no codeword yet (unsent), and that the
    // peer has not acked, sent or not (unacked); writes waiting for the session count as both
    struct Queued {
        std::int64_t unsent = 0, unacked = 0;
    };
    // Host bytes. `vara`: without those written before the last CLEANTXBUFFER (VARA drops them all, on air or not)
    Queued queued_bytes(bool vara = false) const;

    // clock side
    TxBurstPtr poll(double now);
    std::optional<double> next_event();
    // rx is my peer's: connected (or disconnecting) and its first control
    // codeword decodes under the session key (the station's own decode of it
    // is then a memo hit)
    bool claims(RxBurst& rx) const;
    // rx, whose first codeword converged (the caller checks), is another station's: connected, and it is
    // neither my peer's burst nor a session frame
    bool foreign(RxBurst& rx);
    // connected with nothing in flight or queued, either way: free air for a broadcast burst
    bool idle_connected() const {
        return state == SessionState::CONNECTED && station && !out && !deadline && !want_disc
               && pending_write.empty() && !station->tx.pending();
    }
    void on_tx_end(const TxBurstPtr& burst, double now);
    void on_header(const std::string& submode, int n_cw, double now);
    // a CW ID on air (cwid::CwSense): a reply deadline waits reply_start_s past it, a wake counts silence from it
    void on_cw(double now);
    void on_rx(RxBurst& rx, double now);

    bool master() const;  // the caller: the station that retries
    std::optional<double> wake_time();
    bool disc(double now);  // queue a DISC if asked for and all our data is acked
    std::map<std::string, std::int64_t> stats();
    // session.py's _on_timeout; virtual so a binding can honour an instance
    // override (scripts/linksim.py counts timeouts by wrapping it)
    virtual void on_timeout(double now);

protected:
    // A binding makes its own Station subclass here.
    virtual std::shared_ptr<Station> make_station(int direction, bool master, int key);

private:
    struct Frame {
        int key;
        Bytes body;
        std::string mode;
    };
    void queue(TxBurstPtr burst, double when);
    void heard(double now);
    void close(const std::string& why, TxBurstPtr final_burst = nullptr, double now = 0.0);
    void connected(double now);
    void log_stats(const char* label, double now, double since, const std::map<std::string, std::int64_t>& prev);
    TxBurstPtr session_burst(int direction, int key, const Bytes& body, std::optional<std::string> mode = std::nullopt);
    TxBurstPtr connect_burst();
    TxBurstPtr disc_burst();
    TxBurstPtr accept_burst();
    void refuse(const Connect& c, const std::string& mode, int reason, double now);
    bool compact(const std::string& mode);  // a CONNECT in `mode` goes compact (frames pack_connect)
    std::optional<Frame> session_frame(RxBurst& rx);
    std::optional<Frame> primed_frame(RxBurst& rx);  // a connect frame that failed blind (session.py's, §3a)
    std::optional<Frame> primed_body(Bytes body, RxBurst& rx, const char* how = "primed");
    std::optional<Frame> repeated_frame(RxBurst& rx, double now);  // one that failed, combined with the failed ones before it
    double repeat_at = -1e300;  // the last failed connect frame kept for combining
    void on_session_frame(const Frame& f, double now);
    void flush_writes();
};

}  // namespace data2g::arq
