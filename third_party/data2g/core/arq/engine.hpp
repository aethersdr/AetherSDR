// data2g/arq/engine.py: one station's live stack, clocked by audio samples.
//
// step(x) -> (audio out, PTT) once per block at FS: what was heard goes to
// the streaming receiver (receiver::Receiver), headers and bursts to the ARQ
// session and the gear shifter (and to KISS, when the engine serves it
// too), and whatever the session sends is played out, half duplex. Its
// clock is the sample count, so the same code runs behind a sound card or
// back to back with another Engine through a simulated channel.
//
// Threads. A step has two stages:
// - the receiver stage (search, BUSY: Receiver::feed_deferred) always runs
//   on the thread that calls step();
// - the session stage (receiving a burst, ModemRx with its DD pass, KISS,
//   CQ frames, the session, TX audio, the recorder) runs in step() too by
//   default (sync): step(x) returns this block's output, exactly as Python
//   does, deterministically (parity tests, two engines in a simulation).
// With `worker`, the session stage runs on a worker thread, fed blocks in
// order, so a burst's decode and DD don't hold the next blocks' search and
// BUSY. Every session-stage time is the one sync mode gives (a burst is
// handled at the block it ended in, so reply deadlines don't move); what
// varies, with the decode's wall time, is how far output trails input, as
// Python's backlog does behind a sound card. Not deterministic: it is for
// real time. In worker mode the session stage's state (session(), the
// KissLink, the soft-bit store) is the worker's: reach it only through
// post() and the after_block callback, which run there between blocks.
#pragma once

#include <atomic>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "arq/phy.hpp"
#include "arq/policy.hpp"
#include "arq/session.hpp"
#include "cwid/cwid.hpp"
#include "kisslink/kisslink.hpp"
#include "receiver/noise_profile.hpp"
#include "receiver/receiver.hpp"

namespace data2g::arq {

inline constexpr double MAX_BURST_S = 16.0;  // longest burst accepted from a header
inline constexpr int KISS_FRAME_BYTES = 256;  // ...raised per submode to hold a KISS frame this long (engine.py)
// The headers a station takes: MAX_BURST_S, or room for a KISS_FRAME_BYTES
// frame where that is longer (engine.receive_accept).
modem::Accept receive_accept(double min_header_score = 0.0);
inline constexpr double EXPECT_REPLY_S = 0.5;  // a reply's start past my burst's end (engine.py)
inline constexpr double MISSED_MIN_DB = -18.0;  // a timeout's window under this: no reply at all (engine.py)
inline constexpr double CARRIER_Z = 5.0;  // carrier_hold: on air when the last CARRIER_TAIL_S read this (span_z, engine.py)
inline constexpr double CARRIER_TAIL_S = 1.0;
inline constexpr double CARRIER_STEP_S = 0.25;  // ... re-checked this often
inline constexpr double ENERGY_FLOOR_DB = 3.0;  // a measured burst's energy: at least its snr_est less this (engine.py)
// ID frames (docs/arq.md §7a): in a session, one goes ahead of this station's
// turn at least this often (FCC 97.119: every 10 minutes), and one more once
// the session is over: after its last burst (a DISC_ACK), else ID_GUARD_S
// after it closed, when the peer's own ID (after its DISC_ACK) has been heard
inline constexpr double ID_INTERVAL_S = 600.0;
inline constexpr double ID_GUARD_S = REPLY_START_S;
// CW ID (docs/arq.md §7b; engine.py CW_ID_MODES): Engine::cw_id "off" (ID frames only), "add" (a CW ID with
// each), "only" (a CW ID in their place); a transmission no CW ID covers plans one CW_PLAN_S after it, or after
// the last CW ID when that came under CW_GAP_S before it; it goes at the next chance, held by BUSY until
// CW_FORCE_S past the plan's start
inline constexpr double CW_PLAN_S = 540.0;
inline constexpr double CW_GAP_S = 300.0;
inline constexpr double CW_FORCE_S = 600.0;

// engine.Recorder (--record-dir), in the same formats: events.jsonl (one
// JSON object per line, as json.dumps writes it), rx_NNNNN.npz per burst
// heard (an "audio" float32 array; stored, not deflated), and audio_in.f16:
// everything the receiver was fed, float16 at FS (zeros while transmitting).
//
// async: the files are written by a thread of its own, in order, so a slow disk (an SD card) never delays the
// engine; flush() waits until everything handed over is on disk. Without it each call writes before it returns.
class Recorder {
public:
    Recorder(const std::string& dir, const std::string& call, bool async = false);  // throws std::runtime_error
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    void flush();
    void audio(std::span<const double> x);
    // fields: (key, JSON value) pairs after "kind"
    void event(std::string_view kind, const std::vector<std::pair<std::string, std::string>>& fields);
    std::string rx(double t, std::span<const double> audio, const receiver::Pending& header, bool lost,
                   const std::optional<Measured>& meas, const std::optional<receiver::NoiseSnapshot>& noise = std::nullopt);

private:
    void post(std::function<void()> job, std::size_t bytes);
    void loop();

    std::string dir_;
    std::ofstream log_, audio_;
    int n_ = 0;
    bool async_;
    std::mutex m_;
    std::condition_variable wake_, done_;
    std::deque<std::pair<std::function<void()>, std::size_t>> q_;
    std::size_t queued_bytes_ = 0;
    bool busy_ = false, stop_ = false;
    std::thread thread_;
};

// JSON values as Python's json.dumps writes them.
std::string json_str(std::string_view s);
std::string json_num(double v);
std::string json_measured(const Measured& m);  // phy.measure's dict
std::string json_noise(const receiver::NoiseSnapshot& n);  // receiver.NoiseProfile.snapshot()'s dict
// The bytes of a .npz holding one float32 array (np.load reads it).
std::vector<std::uint8_t> npz_f32(const std::string& name, std::span<const float> a);
std::uint16_t to_half(double x);  // numpy's float64 -> float16: round to nearest even

struct EngineConfig {
    double ptt_delay_s = 0.1;
    std::string record_dir;  // empty: no recording
    bool record_async = false;  // the recording written by a thread of its own (Recorder)
    std::optional<std::uint64_t> seed;  // the engine's DefaultRng (nullopt: random)
    double min_header_score = 0.0;
    kisslink::KissLink* kiss = nullptr;  // served too (broadcast over KISS); not owned
    double stats_interval_s = 60.0;
    bool dd = dd_default();
    std::optional<double> dd_budget_s = DD_BUDGET_S;  // nullopt: no limit
    bool worker = false;  // the session stage on a worker thread (see the top)
    // Worker: audio queued for the session stage, at most. Past it blocks
    // are dropped (counted, logged once per episode) and the receiver
    // reset; their time still passes on the session stage, as silence.
    double max_backlog_s = 60.0;
    // The gear shifter's noise rule, its tail weight (GearShifter::noise_rule); 0: off.
    double noise_rule = NOISE_RULE;
};

// How a binding swaps in its own objects; every one optional.
struct EngineHooks {
    std::function<std::shared_ptr<Policy>()> policy;  // default: a GearPolicy per session
    std::shared_ptr<Rng> rng;  // the engine's draws (default: DefaultRng(seed))
    std::function<std::shared_ptr<Rng>(double seed)> session_rng;  // default: DefaultRng
    std::function<std::shared_ptr<Session>(const std::string& call, std::shared_ptr<Policy>, std::shared_ptr<Rng>,
                                           const std::vector<std::string>& aliases, double stats_interval_s)>
        session;
};

// A burst heard (Engine::set_on_burst): what a front end's burst log shows.
struct BurstHeard {
    double t = 0.0;  // engine time, s
    std::string submode;
    int n_cw = 0;
    bool lost = false;  // header heard, burst not (the recorder's "lost")
    std::optional<double> snr_db;  // phy.measure's snr_est; nullopt when lost
    Heard heard;  // the burst itself (empty when lost), for a monitor
    std::shared_ptr<const SlotSoft> soft;  // soft_bits(heard), shared
};

// A burst still arriving (Engine::set_on_preview), for a live constellation: its data symbols
// received so far, each block adding to the last.
struct BurstPreview {
    std::string submode;
    const modem::Spec* ofdm = nullptr;     // OFDM: the spec, and its symbols (modem::preview_frame)
    std::vector<std::complex<double>> iq;
    Mat<double> shares;                    // CPM: each data symbol's tone shares (cpm::shares)
};

// The engine's receive helpers, for a receiver of its own (monitor::Monitor).
Heard heard_of(receiver::Rx&& rx);
std::string spec_name(const receiver::Pending& p);
std::vector<std::string_view> all_grids();  // every CPM grid, as the engine's receiver searches

class Engine {
public:
    struct Out {
        std::vector<double> audio;  // a burst's peak at 1.0
        bool ptt = false;
        // Set on the step a transmission starts: the whole burst's audio (its first block is `audio`'s start),
        // so a front end can play it on a clock of its own instead of block by block from this step's.
        std::shared_ptr<const std::vector<double>> tx_burst;
        bool tx_abort = false;  // abort() was called since the last step: whatever is playing should stop
    };
    struct Tx {
        TxBurstPtr burst;
        std::vector<double> audio;
        std::size_t pos = 0;
    };

    explicit Engine(std::string call, EngineConfig cfg = {}, EngineHooks hooks = {});
    virtual ~Engine();  // stop()s
    static Out silence(std::size_t samples) {
        Out o;
        o.audio.assign(samples, 0.0);
        return o;
    }
    // Joins the worker after the block in hand; no step() after it. A
    // subclass overriding a seam calls it from its own destructor, before
    // its part is gone.
    void stop();
    void flush_recorder() {
        if (rec_) rec_->flush();
    }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // -- audio side (the engine thread)
    Out step(std::span<const double> x);
    bool busy() const { return busy_now_; }  // a burst arriving (the receiver stage's, latest)
    bool channel_busy() const { return channel_busy_now_; }  // the host's BUSY ON/OFF
    // the session peer's last burst it claimed: snr_est (the host's SN); nullopt before one
    std::optional<double> session_snr() const { return session_snr_; }
    // Samples dropped because the worker fell max_backlog_s behind (any thread).
    std::uint64_t decode_dropped() const { return dropped_.load(std::memory_order_relaxed); }

    // -- the session stage: the engine thread in sync mode; in worker mode
    // through post() / after_block only
    void post(std::function<void()> f);  // runs before the next block's session stage
    void set_after_block(std::function<void(bool ptt)> f) { after_block_ = std::move(f); }
    // Every burst heard, on the session stage. Set it before the first step().
    void set_on_burst(std::function<void(const BurstHeard&)> f) { on_burst_ = std::move(f); }
    // Every burst we transmit (ARQ, ID, KISS), on the session stage as it is keyed, in the order sent:
    // a monitor's view of our own side. Set it before the first step().
    void set_on_send(std::function<void(const TxBurstPtr&)> f) { on_send_ = std::move(f); }
    // After a heard (not lost) burst's on_burst, once it has been read: its slots decoded, and tried but not.
    void set_on_decoded(std::function<void(int decoded, int failed)> f) { on_decoded_ = std::move(f); }
    // Called on the receiving thread (step's) as a burst arrives, while set_preview(true): a few
    // FFTs per block. Set before the engine runs.
    void set_on_preview(std::function<void(const BurstPreview&)> f) { on_preview_ = std::move(f); }
    void set_preview(bool on) { preview_on_ = on; }

    const std::string& call() const { return call_; }
    const std::vector<std::string>& aliases() const { return aliases_; }
    // Session::accept_connect, for every session from the next on (the host's ACCEPT)
    std::function<bool(const std::string&, const std::string&)> accept_connect;
    // ... set for the session now as well
    void set_accept_connect(std::function<bool(const std::string&, const std::string&)> f) {
        accept_connect = f;
        session_->accept_connect = std::move(f);
    }
    Session& session() { return *session_; }
    const std::shared_ptr<Session>& session_ptr() const { return session_; }
    double now() const { return static_cast<double>(n_) / config::FS; }
    std::int64_t n() const { return n_; }
    void set_n(std::int64_t n) { n_ = n; }
    void listen(bool on = true);
    void set_call(const std::string& call, const std::vector<std::string>& aliases = {});
    void disconnect() { session_->disconnect(); }  // gracefully: the peer is told, what's queued still goes
    void abort();
    void connect(const std::string& peer, int cap);  // throws std::runtime_error while a session is under way
    void set_chat(bool on);
    // engine.py's: a connected session with no data this long disconnects; this session and the next.
    // nullopt: the session's own tuning (IDLE_CLOSE_S).
    void set_idle_close(std::optional<double> s);
    double idle_close_s() const { return session_->tune.idle_close_s; }
    // Host notifications since the last call, as engine.py's events() words them: "CONNECTED peer",
    // "DISCONNECTED why", "CQFRAME call cap", "ID call key".
    std::vector<std::string> events();
    void send_cq(const std::string& call, int cap);  // throws std::runtime_error, std::invalid_argument (call)
    // A control-only burst anyone can read (frame type SESSION, mask 0), in the cap's connect mode.
    TxBurstPtr open_frame(int cap, int ext, const Bytes& body) const;
    double id_interval_s = ID_INTERVAL_S;
    std::string cw_id = "off";  // "off", "add", "only" (CW_PLAN_S)
    double cw_wpm = cwid::WPM;
    std::optional<double> next_event() { return session_->next_event(); }
    const std::optional<Tx>& tx() const { return tx_; }
    // The burst on air's submode (the session's, when an ID rides with it); nullptr: nothing on air.
    const std::string* tx_submode() const { return tx_ ? &tx_->burst->submode : nullptr; }
    std::int64_t ptt_delay() const { return ptt_delay_; }  // samples of silence before each burst's audio
    const std::deque<TxBurstPtr>& extra() const { return extra_; }
    // (port, frame)s heard for KISS clients since the last call
    std::vector<std::pair<int, Bytes>> take_kiss_rx() { return std::exchange(kiss_rx_, {}); }
    kisslink::KissLink* kiss() const { return cfg_.kiss; }
    const modem::Accept& accept() const { return accept_; }
    SoftStore& store() { return store_; }
    const EngineConfig& config() const { return cfg_; }

protected:
    // Seams for a binding (a Python receiver, a patched phy.tx_audio) and
    // for tests (a slow decode).
    virtual std::vector<receiver::Receiver::Item> receiver_feed(std::span<const double> x) {
        return receiver_.feed_deferred(x);
    }
    virtual bool receiver_busy() { return receiver_.busy(); }
    virtual bool receiver_channel_busy() { return receiver_.channel_busy(); }
    virtual void receiver_reset() { receiver_.reset(); }
    virtual void receiver_expect(const std::vector<std::string_view>& grids, std::int64_t lo, std::int64_t hi, double cfo) {
        receiver_.expect(grids, lo, hi, cfo);
    }
    virtual std::vector<double> tx_audio(const TxBurst& b) { return arq::tx_audio(b); }
    virtual void hear_burst(receiver::BurstEvent& ev, double t);

private:
    struct Block {
        std::vector<double> x;
        std::vector<receiver::Receiver::Item> items;
        bool busy = false;
        bool cw = false;  // a CW ID heard (cwid::CwSense) with no burst locked
        std::uint64_t gen = 0, seq = 0;
        std::int64_t gap = 0;  // samples dropped just before this block
    };
    struct Done {
        Out out;
        bool sound = false;  // anything played (else silence that can be dropped to catch up)
    };

    void new_session();
    bool idle() const;
    Done process(Block& b);  // the session stage
    Done process_safe(Block& b);  // process(); a throw drops the session and plays silence
    void hear(std::vector<receiver::Receiver::Item>& items, double t);
    TxBurstPtr kiss_burst(std::int64_t k, bool busy);
    bool cq(ModemRx& rx);
    TxBurstPtr id_frame(const Session& s) const;
    void id_check(double t);
    std::vector<TxBurstPtr> with_id(const TxBurstPtr& burst, double t);
    std::vector<TxBurstPtr> ids(const Session& s, bool ahead) const;  // its ID as cw_id has it
    TxBurstPtr cw_burst(const std::string& call) const;
    std::vector<TxBurstPtr> cw_ahead(std::vector<TxBurstPtr> bursts, double t) const;  // outside a session
    bool cw_ready(double t) const;  // the planned CW ID is due
    bool cw_due(double t, bool ok) const;  // ...alone, outside a session: `ok` (free, no BUSY), or CW_FORCE_S
    void cw_plan(const std::vector<TxBurstPtr>& bursts, const std::vector<std::size_t>& lens, double t);
    void cw_hold(double t);       // a CW ID heard: what follows it on the same PTT is the peer's
    // Bursts back to back on one PTT; `main` is the one the session is told has gone.
    void start_tx(const std::vector<TxBurstPtr>& bursts, double t, const TxBurstPtr& main);
    void request_reset();
    void apply_reset();
    void run_posted();
    void work();
    Out next_out(std::size_t k);  // under mu_

    std::string call_;
    std::vector<std::string> aliases_;
    EngineConfig cfg_;
    EngineHooks hooks_;
    std::shared_ptr<Rng> rng_;
    modem::Accept accept_;
    receiver::Receiver receiver_;
    receiver::NoiseProfile noise_;  // the passband's noise between bursts (recorded with each burst heard)
    std::int64_t ptt_delay_;
    std::optional<Recorder> rec_;
    std::int64_t n_ = 0;
    std::optional<Tx> tx_;
    std::shared_ptr<Session> session_;
    SoftStore store_;
    bool chat_ = false;
    std::optional<double> idle_close_;  // set_idle_close
    std::deque<TxBurstPtr> extra_;  // bursts outside any session (CQ frames)
    std::vector<std::string> events_;  // host notifications from outside the session
    std::vector<std::pair<int, Bytes>> kiss_rx_;
    bool tx_kiss_ = false;  // the burst on air is a KISS one
    std::int64_t kiss_busy_ = 0;  // samples of unbroken BUSY a queued KISS burst has waited
    bool kiss_deferred_ = false;  // ... and it has waited on BUSY
    std::int64_t kiss_slot_ = 0;  // next p-persistence slot, samples
    std::shared_ptr<Session> id_for_;      // the session ID frames are being sent for
    std::optional<double> id_due_;         // its next ID (nullopt: closed, its last ID pending or sent)
    std::shared_ptr<Session> id_pending_;  // its last ID, after it closed, from id_pending_t_
    double id_pending_t_ = 0.0;
    double hold_ = 0.0;  // nothing new goes before this (the peer's last ID may follow its DISC_ACK)
    cwid::CwSense cw_;  // the receiver stage's
    bool cw_heard_ = false;  // the last block's (logged once per ID)
    double cw_until_ = 0.0;  // nothing outside a session goes before this: ID_GUARD_S past a CW ID heard
    std::optional<double> cw_at_;  // the planned CW ID (CW_PLAN_S)
    std::optional<double> cw_last_;  // my last CW ID's start
    // the energy inputs (engine.py _energy, _missed_energy)
    struct Span {
        double start, end;
        std::string mode;
    };
    std::optional<Span> span_;  // the burst being heard
    std::optional<Span> heard_span_;  // ... being decoded: its energy once measured (hear_burst)
    std::optional<double> session_snr_;
    std::optional<double> tx_end_;  // when my last burst ended
    // the receiver's expected slot (engine.py's slot, _expect_reply; docs/arq.md §3a): the session stage
    // learns it, and leaves the window for the receiver stage to set with the reset it asks for
    receiver::ReplySlot slot_;
    struct Expect {
        std::uint64_t gen;
        std::vector<std::string_view> grids;
        std::int64_t lo, hi;
        double cfo;
    };
    std::optional<Expect> expect_next_;  // under mu_
    void expect_reply();
    std::int64_t timeouts_ = 0;  // the station's timeouts already reported
    void energy(double start, double end, const std::string& mode, double t, std::optional<double> min_db = std::nullopt,
                std::optional<double> floor_db = std::nullopt);
    void missed_energy();
    void carrier_hold(double t);  // a timeout held while a reply whose header I missed is on air
    std::optional<double> carrier_for_;  // the burst end (tx_end_) a carrier hold was logged for
    std::optional<double> span_snr(const Span& s);  // its in-band SNR from power alone (snr_est's scale)
    int rx_cap() const;  // the bandwidth cap a heard burst's TX filter was most likely for
    std::function<void(bool)> after_block_;
    std::function<void(const BurstHeard&)> on_burst_;
    std::function<void(const TxBurstPtr&)> on_send_;
    std::function<void(int, int)> on_decoded_;
    std::function<void(const BurstPreview&)> on_preview_;
    std::atomic<bool> preview_on_{false};
    // the receiving thread's: the burst being previewed, keyed by its lock's start
    struct Preview {
        std::int64_t start = -1;
        int done = 0;  // on-air frames (OFDM) or stream symbols (CPM) read
        BurstPreview out;
    };
    Preview preview_;
    void preview();

    // between the stages
    std::atomic<bool> transmitting_{false}, busy_now_{false}, channel_busy_now_{false}, abort_pending_{false};
    std::atomic<std::uint64_t> want_gen_{0};  // the session stage asks for a receiver reset
    std::uint64_t gen_ = 0, seq_ = 0;          // the receiver stage's
    std::int64_t gap_ = 0;                     // the receiver stage's: samples dropped since the last block queued
    std::atomic<std::size_t> queued_{0};       // samples in in_
    std::atomic<std::uint64_t> dropped_{0};
    std::mutex mu_;
    std::condition_variable wake_, done_cv_;
    std::deque<Block> in_;
    std::deque<Done> out_;
    std::deque<std::function<void()>> posted_;
    std::uint64_t n_done_ = 0;
    int slow_ = 0;  // blocks with a burst queued or in the session stage
    bool stop_ = false;
    std::thread worker_;
};

}  // namespace data2g::arq
