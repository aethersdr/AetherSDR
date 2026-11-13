#include "arq/engine.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <utility>

#include "codes/codes.hpp"
#include "dsp/dsp.hpp"

namespace data2g::arq {

namespace {

constexpr const char* LOG = "data2g.engine";
constexpr int INFO = 20, WARNING = 30;

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::string spec_name(const receiver::Pending& p) {
    return std::string(p.is_cpm() ? p.cpm().spec->name : p.ofdm().spec->name);
}

Heard heard_of(receiver::Rx&& rx) {
    Heard h;
    if (auto* o = std::get_if<modem::Received>(&rx)) h.ofdm = std::make_shared<const modem::Received>(std::move(*o));
    else h.cpm = std::make_shared<const cpm::Received>(std::move(std::get<cpm::Received>(rx)));
    return h;
}

std::vector<std::string_view> all_grids() {
    std::vector<std::string_view> out;
    for (const auto& g : tables::CPM_GRIDS) out.push_back(g.name);
    return out;
}

namespace {

void put16(std::vector<std::uint8_t>& b, unsigned v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    put16(b, v & 0xFFFF);
    put16(b, v >> 16);
}

std::string json_obj(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string s = "{";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) s += ", ";
        s += json_str(fields[i].first) + ": " + fields[i].second;
    }
    return s + "}";
}

std::string hex(const Bytes& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto c : b) {
        s += d[c >> 4];
        s += d[c & 15];
    }
    return s;
}

}  // namespace

// --- recording -------------------------------------------------------------------------

std::string json_str(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20 || c >= 0x7F) {  // json.dumps' ensure_ascii (callsigns and modes are ASCII)
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else out += static_cast<char>(c);
    }
    return out + "\"";
}

std::string json_num(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
    char buf[64];
    const double a = std::fabs(v);
    // float.__repr__: shortest round trip, positional for 1e-4 <= |v| < 1e16
    const bool fixed = a == 0 || (a >= 1e-4 && a < 1e16);
    auto r = std::to_chars(buf, buf + sizeof buf, v, fixed ? std::chars_format::fixed : std::chars_format::scientific);
    std::string s(buf, r.ptr);
    if (fixed && s.find('.') == std::string::npos) s += ".0";
    return s;
}

std::string json_measured(const Measured& m) {
    std::vector<std::pair<std::string, std::string>> f = {
        {"snr_est", json_num(m.snr_est)}, {"spread_est", json_num(m.spread_est)},
        {"delay_est_ms", json_num(m.delay_est_ms)}, {"headroom", json_num(m.headroom)}, {"frames", json_num(m.frames)}};
    for (std::size_t i = 0; i < CONSTS.size(); ++i) f.emplace_back("mi_" + std::string(CONSTS[i]), json_num(m.mi[i]));
    return json_obj(f);
}

std::string json_noise(const receiver::NoiseSnapshot& n) {
    auto list = [](const std::array<double, 5>& v) {
        std::string s = "[";
        for (std::size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + json_num(v[i]);
        return s + "]";
    };
    return json_obj({{"noise_db", list(n.db)}, {"noise_tail_db", list(n.tail_db)},
                     {"impulses_per_min", json_num(n.impulses_per_min)}, {"noise_blocks", std::to_string(n.blocks)}});
}

std::uint16_t to_half(double x) {
    const std::uint16_t sign = std::signbit(x) ? 0x8000 : 0;
    const double a = std::fabs(x);
    if (std::isnan(a)) return sign | 0x7E00;
    if (a >= 65520.0) return sign | 0x7C00;  // rounds to inf
    if (a < 6.103515625e-05)                 // subnormal: units of 2^-24 (1024 rounds up to the least normal)
        return static_cast<std::uint16_t>(sign | static_cast<int>(std::nearbyint(a * 16777216.0)));
    int e;
    std::frexp(a, &e);  // a = f 2^e, f in [0.5, 1)
    int m = static_cast<int>(std::nearbyint(std::ldexp(a, 11 - e)));  // [1024, 2048], half to even
    int E = e - 1 + 15;
    if (m == 2048) {
        m = 1024;
        ++E;
    }
    if (E >= 31) return sign | 0x7C00;
    return static_cast<std::uint16_t>(sign | E << 10 | (m - 1024));
}

std::vector<std::uint8_t> npz_f32(const std::string& name, std::span<const float> a) {
    std::string hdr = "{'descr': '<f4', 'fortran_order': False, 'shape': (" + std::to_string(a.size()) + ",), }";
    hdr.append(64 - (10 + hdr.size() + 1) % 64, ' ');
    hdr += '\n';
    std::vector<std::uint8_t> npy = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0};
    put16(npy, static_cast<unsigned>(hdr.size()));
    npy.insert(npy.end(), hdr.begin(), hdr.end());
    for (float v : a) {
        std::uint32_t u;
        std::memcpy(&u, &v, 4);
        put32(npy, u);
    }
    // a zip of one stored member, as np.load reads it
    const std::string member = name + ".npy";
    const std::uint32_t crc = codes::crc32(npy);
    const auto size = static_cast<std::uint32_t>(npy.size());
    auto header = [&](std::vector<std::uint8_t>& b, bool central) {
        put32(b, central ? 0x02014B50 : 0x04034B50);
        if (central) put16(b, 20);  // made by
        put16(b, 20);               // needed
        put16(b, 0);                // flags
        put16(b, 0);                // stored
        put16(b, 0);                // time
        put16(b, 0x21);             // 1980-01-01
        put32(b, crc);
        put32(b, size);
        put32(b, size);
        put16(b, static_cast<unsigned>(member.size()));
        put16(b, 0);  // extra
        if (central) {
            put16(b, 0);  // comment
            put16(b, 0);  // disk
            put16(b, 0);  // internal attributes
            put32(b, 0);  // external
            put32(b, 0);  // the local header's offset
        }
        b.insert(b.end(), member.begin(), member.end());
    };
    std::vector<std::uint8_t> out;
    header(out, false);
    out.insert(out.end(), npy.begin(), npy.end());
    const auto cd = static_cast<std::uint32_t>(out.size());
    header(out, true);
    const auto cd_size = static_cast<std::uint32_t>(out.size()) - cd;
    put32(out, 0x06054B50);
    put16(out, 0);
    put16(out, 0);
    put16(out, 1);
    put16(out, 1);
    put32(out, cd_size);
    put32(out, cd);
    put16(out, 0);
    return out;
}

Recorder::Recorder(const std::string& dir, const std::string& call, bool async) : dir_(dir), async_(async) {
    std::filesystem::create_directories(dir_);
    log_.open(dir_ + "/events.jsonl", std::ios::app);
    audio_.open(dir_ + "/audio_in.f16", std::ios::app | std::ios::binary);
    if (!log_ || !audio_) throw std::runtime_error("cannot record to " + dir_);
    if (async_) thread_ = std::thread(&Recorder::loop, this);
    const double wall = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    event("start", {{"call", json_str(call)}, {"wall", json_num(wall)}, {"fs", std::to_string(config::FS)}});
}

Recorder::~Recorder() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard lock(m_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();  // writes what is queued first
}

// A job for the writer thread (or run now): in order, and no more than QUEUE_MAX bytes waiting, so a disk that
// stalls for good ends in the engine waiting rather than in the machine's memory.
void Recorder::post(std::function<void()> job, std::size_t bytes) {
    if (!async_) {
        job();
        return;
    }
    constexpr std::size_t QUEUE_MAX = 64u << 20;
    std::unique_lock lock(m_);
    done_.wait(lock, [&] { return queued_bytes_ < QUEUE_MAX; });
    queued_bytes_ += bytes;
    q_.emplace_back(std::move(job), bytes);
    wake_.notify_one();
}

void Recorder::loop() {
    std::unique_lock lock(m_);
    for (;;) {
        wake_.wait(lock, [&] { return stop_ || !q_.empty(); });
        if (q_.empty()) return;  // stopped, and nothing left
        auto [job, bytes] = std::move(q_.front());
        q_.pop_front();
        busy_ = true;
        lock.unlock();
        try {
            job();
        } catch (...) {  // a full disk must not end the engine's thread
        }
        lock.lock();
        busy_ = false;
        queued_bytes_ -= bytes;
        done_.notify_all();
    }
}

void Recorder::flush() {
    if (!async_) return;
    std::unique_lock lock(m_);
    done_.wait(lock, [&] { return q_.empty() && !busy_; });
}

void Recorder::audio(std::span<const double> x) {
    std::vector<std::uint16_t> h(x.size());
    std::transform(x.begin(), x.end(), h.begin(), to_half);  // little-endian hosts, as numpy writes it
    const std::size_t bytes = h.size() * 2;
    post(
        [this, h = std::move(h)] {
            audio_.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size() * 2));
            audio_.flush();  // whole on disk at every block: readable while live, nothing lost on a crash
        },
        bytes);
}

void Recorder::event(std::string_view kind, const std::vector<std::pair<std::string, std::string>>& fields) {
    std::vector<std::pair<std::string, std::string>> f = {{"kind", json_str(kind)}};
    f.insert(f.end(), fields.begin(), fields.end());
    std::string line = json_obj(f) + '\n';
    const std::size_t bytes = line.size();
    post([this, line = std::move(line)] { log_ << line << std::flush; }, bytes);  // line-buffered, as Python opens it
}

std::string Recorder::rx(double t, std::span<const double> audio, const receiver::Pending& header, bool lost,
                         const std::optional<Measured>& meas, const std::optional<receiver::NoiseSnapshot>& noise) {
    char name[32];
    std::snprintf(name, sizeof name, "rx_%05d.npz", n_++);
    std::vector<float> a(audio.begin(), audio.end());
    const std::size_t bytes = a.size() * 4;
    // the file, then its line in the log: both in the writer's hands, in that order
    post(
        [this, path = dir_ + "/" + name, a = std::move(a)] {
            const auto npz = npz_f32("audio", a);
            std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(npz.data()),
                                                        static_cast<std::streamsize>(npz.size()));
        },
        bytes);
    event("rx", {{"t", json_num(t)},
                 {"file", json_str(name)},
                 {"submode", json_str(spec_name(header))},
                 {"n_cw", std::to_string(header.n_cw())},
                 {"score", json_num(header.score())},
                 {"lost", lost ? "true" : "false"},
                 {"meas", meas ? json_measured(*meas) : "null"},
                 {"noise", noise ? json_noise(*noise) : "null"}});
    return name;
}

// --- engine ----------------------------------------------------------------------------

modem::Accept receive_accept(double min_header_score) {
    auto a = modem::Accept::of({}, MAX_BURST_S, min_header_score);
    for (auto& [s, cw] : a.max_cw) {
        if (s->name.find("ack") != std::string_view::npos) continue;
        // a full control, each codeword twice (DUP), and the frame framed as a burst's first (a zero before it)
        const int pb = codes::payload_bytes(*s);
        const int need = 2 * MAX_CTL + static_cast<int>((1 + kisslink::framed_len(KISS_FRAME_BYTES) + pb - 1) / pb);
        cw = std::max(cw, std::min(need, config::max_codewords(s->sync_band)));
    }
    return a;
}

Engine::Engine(std::string call, EngineConfig cfg, EngineHooks hooks)
    : call_(upper(std::move(call))),
      cfg_(std::move(cfg)),
      hooks_(std::move(hooks)),
      rng_(hooks_.rng ? hooks_.rng
                      : std::make_shared<DefaultRng>(cfg_.seed ? *cfg_.seed : std::random_device{}())),
      accept_(receive_accept(cfg_.min_header_score)),
      receiver_(accept_, all_grids()),
      ptt_delay_(static_cast<std::int64_t>(cfg_.ptt_delay_s * config::FS)) {
    if (!cfg_.record_dir.empty()) rec_.emplace(cfg_.record_dir, call_, cfg_.record_async);
    new_session();
    if (cfg_.worker) worker_ = std::thread(&Engine::work, this);
}

Engine::~Engine() { stop(); }

void Engine::stop() {
    {
        std::lock_guard lock(mu_);
        stop_ = true;
    }
    wake_.notify_all();
    done_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool Engine::idle() const {
    const auto s = session_->state;
    return s == SessionState::IDLE || s == SessionState::LISTEN || s == SessionState::CLOSED;
}

void Engine::new_session() {
    std::shared_ptr<Policy> policy;
    if (hooks_.policy) {
        policy = hooks_.policy();
    } else {
        auto gear = std::make_shared<GearPolicy>();
        gear->shifter.noise_rule = cfg_.noise_rule > 0 ? std::optional(cfg_.noise_rule) : std::nullopt;
        policy = std::move(gear);
    }
    const double seed = rng_->uniform(0.0, 1.0);  // random.Random(self.rng.random())
    std::shared_ptr<Rng> rng;
    if (hooks_.session_rng) rng = hooks_.session_rng(seed);
    else {
        std::uint64_t bits;
        std::memcpy(&bits, &seed, sizeof bits);
        rng = std::make_shared<DefaultRng>(bits);
    }
    session_ = hooks_.session ? hooks_.session(call_, std::move(policy), std::move(rng), aliases_, cfg_.stats_interval_s)
                              : std::make_shared<Session>(call_, std::move(policy), 1.0, std::move(rng), aliases_,
                                                          cfg_.stats_interval_s);
    session_->set_chat(chat_);
    if (idle_close_) session_->tune.idle_close_s = *idle_close_;
    session_->accept_connect = accept_connect;
    store_.clear();
    session_snr_.reset();
}

void Engine::listen(bool on) {
    if (idle()) {
        new_session();
        if (on) session_->listen();
    }
}

void Engine::set_call(const std::string& call, const std::vector<std::string>& aliases) {
    pack_call(call);  // invalid_argument for a call that won't pack, before anything changes
    for (const auto& a : aliases) pack_call(a);
    call_ = upper(call);
    aliases_.clear();
    for (const auto& a : aliases) aliases_.push_back(upper(a));
    if (idle()) listen(session_->state == SessionState::LISTEN);
}

void Engine::abort() {
    abort_pending_ = true;
    tx_.reset();
    transmitting_ = false;
    request_reset();
    new_session();
}

void Engine::connect(const std::string& peer, int cap) {
    if (!idle()) throw std::runtime_error(std::string("session ") + state_name(session_->state));
    pack_call(peer);  // invalid_argument before any state changes
    new_session();
    session_->connect(peer, cap, now());
}

void Engine::set_idle_close(std::optional<double> s) {
    session_->tune.idle_close_s = s.value_or(IDLE_CLOSE_S);
    idle_close_ = s;
}

void Engine::set_chat(bool on) {
    chat_ = on;
    session_->set_chat(on);
}

std::vector<std::string> Engine::events() {
    std::vector<std::string> ev = std::move(events_);
    events_.clear();
    ev.insert(ev.end(), session_->events.begin(), session_->events.end());
    session_->events.clear();
    return ev;
}

void Engine::send_cq(const std::string& call, int cap) {
    if (!idle()) throw std::runtime_error(std::string("session ") + state_name(session_->state));
    extra_.push_back(open_frame(cap, T_CQ, pack_cq(call, cap)));
}

TxBurstPtr Engine::open_frame(int cap, int ext, const Bytes& body) const {
    const std::string mode = session_->policy->connect_mode(cap, 0);
    Control ctl{Core{.ftype = SESSION}, {{ext, body}}};
    const auto payloads = ctl.pack(session_->policy->payload_bytes(mode));
    auto b = std::make_shared<TxBurst>();
    b->submode = mode;
    b->cap = cap;
    for (std::size_t i = 0; i < payloads.size(); ++i) b->slots.push_back({ctl_mask(0, static_cast<int>(i), 0), 0, payloads[i]});
    return b;
}

TxBurstPtr Engine::id_frame(const Session& s) const {
    log_write(LOG, INFO, format("TX ID %s", s.call.c_str()));
    return open_frame(s.cap, T_ID, pack_id(s.call, s.station->key));
}

// Track the session ID frames are owed for; once it closes, its last ID is pending.
void Engine::id_check(double t) {
    if (!session_->station) return;
    if (session_ != id_for_) {
        id_for_ = session_;
        id_due_ = t + id_interval_s;
    }
    if (session_->state == SessionState::CLOSED && id_due_) {
        id_due_.reset();
        id_pending_ = session_;
        id_pending_t_ = hold_ = t + ID_GUARD_S;
    }
}

// Session s's ID as cw_id has it: an ID frame, a CW ID, or both (ahead of a turn: the CW first).
std::vector<TxBurstPtr> Engine::ids(const Session& s, bool ahead) const {
    std::vector<TxBurstPtr> out;
    if (cw_id != "only") out.push_back(id_frame(s));
    if (cw_id != "off") out.insert(ahead ? out.begin() : out.end(), cw_burst(s.call));
    return out;
}

TxBurstPtr Engine::cw_burst(const std::string& call) const {
    log_write(LOG, INFO, format("TX CW ID %s", call.c_str()));
    auto b = std::make_shared<TxBurst>();
    b->submode = "cw";
    b->cw_text = cwid::id_text(call);
    b->cw_wpm = cw_wpm;
    return b;
}

// A session's burst -> the bursts to send back to back: an ID first when one
// is due, or the pending last ID after a closed session's final burst (its DISC_ACK).
std::vector<TxBurstPtr> Engine::with_id(const TxBurstPtr& burst, double t) {
    if (session_->state == SessionState::CLOSED && id_pending_) {
        id_pending_.reset();
        auto out = ids(*session_, false);
        out.insert(out.begin(), burst);
        return out;
    }
    if (session_ == id_for_ && id_due_ && t >= *id_due_) {
        id_due_ = t + id_interval_s;
        auto out = ids(*session_, true);
        out.push_back(burst);
        return out;
    }
    return {burst};
}

bool Engine::cw_ready(double t) const { return cw_id != "off" && cw_at_ && t >= *cw_at_; }

// Outside a session: the planned CW ID ahead of `bursts` once due.
std::vector<TxBurstPtr> Engine::cw_ahead(std::vector<TxBurstPtr> bursts, double t) const {
    if (cw_ready(t)) bursts.insert(bursts.begin(), cw_burst(call_));
    return bursts;
}

bool Engine::cw_due(double t, bool ok) const {
    return cw_ready(t) && idle() && !id_pending_ && (ok || t >= *cw_at_ - CW_PLAN_S + CW_FORCE_S);
}

// CW_PLAN_S: a CW ID covers all before it, and plans the next one when bursts follow it; else the first burst no
// ID covers plans one.
void Engine::cw_plan(const std::vector<TxBurstPtr>& bursts, const std::vector<std::size_t>& lens, double t) {
    if (cw_id == "off") return;
    double start = t + static_cast<double>(ptt_delay_) / config::FS;
    for (std::size_t j = 0; j < bursts.size(); ++j) {
        if (bursts[j]->submode == "cw") {
            cw_last_ = start;
            cw_at_.reset();
        } else if (!cw_at_) {
            const bool fresh = !cw_last_ || start - *cw_last_ >= CW_GAP_S;
            cw_at_ = (fresh ? start : *cw_last_) + CW_PLAN_S;
        }
        start += static_cast<double>(lens[j]) / config::FS;
    }
}

void Engine::cw_hold(double t) {
    if (!cw_heard_) log_write(LOG, INFO, "CW ID heard: holding");
    session_->on_cw(t);
    cw_until_ = t + ID_GUARD_S;
    if (id_pending_) id_pending_t_ = std::max(id_pending_t_, cw_until_);
}

void Engine::post(std::function<void()> f) {
    {
        std::lock_guard lock(mu_);
        posted_.push_back(std::move(f));
    }
    wake_.notify_one();
}

void Engine::run_posted() {
    std::deque<std::function<void()>> todo;
    {
        std::lock_guard lock(mu_);
        todo.swap(posted_);
    }
    for (auto& f : todo) f();
}

// --- the receiver stage ----------------------------------------------------------------

void Engine::apply_reset() {
    receiver_reset();
    cw_.reset();
    gen_ = want_gen_;
    std::optional<Expect> e;
    {
        std::lock_guard lock(mu_);
        e = std::exchange(expect_next_, std::nullopt);
    }
    if (e && e->gen == gen_) receiver_expect(e->grids, e->lo, e->hi, e->cfo);
    busy_now_ = receiver_busy();
    channel_busy_now_ = receiver_channel_busy();
}

void Engine::expect_reply() {
    // the receiver's expected slot for the peer's reply: the CPM grids my session expects it in, where its
    // replies have started (stream indices from the reset at my burst's end), its CFO (engine.py)
    const auto w = slot_.window(0.0);
    std::vector<std::string_view> grids;
    for (const auto& m : session_->expecting()) {
        const Mode* md = mode(m);
        if (!md || !md->is_cpm()) continue;
        const std::string_view g = cpm::grid_of(*md->cpm).name;
        if (std::find(grids.begin(), grids.end(), g) == grids.end()) grids.push_back(g);
    }
    if (!w || grids.empty() || !prime_default()) return;
    std::lock_guard lock(mu_);
    expect_next_ = Expect{want_gen_ + 1, std::move(grids),
                          std::max<std::int64_t>(0, static_cast<std::int64_t>(std::nearbyint(w->first * config::FS))),
                          static_cast<std::int64_t>(std::nearbyint(w->second * config::FS)), *slot_.cfo};
}

void Engine::request_reset() {
    ++want_gen_;
    if (!cfg_.worker) apply_reset();  // one thread: at once, as Python does
}

Engine::Out Engine::step(std::span<const double> x) {
    if (cfg_.worker && static_cast<double>(queued_.load()) >= cfg_.max_backlog_s * config::FS) {
        // the worker fell behind: drop rather than queue without limit
        if (!gap_) apply_reset();  // nothing is fed across the hole: the receiver resyncs after it
        gap_ += static_cast<std::int64_t>(x.size());
        dropped_ += x.size();
        std::lock_guard lock(mu_);
        return next_out(x.size());
    }
    if (gen_ != want_gen_) apply_reset();
    Block b;
    if (gap_) {
        log_write(LOG, WARNING, format("decode worker fell %.0f s behind: %.1f s of audio dropped, receiver reset",
                                       cfg_.max_backlog_s, static_cast<double>(gap_) / config::FS));
        b.gap = std::exchange(gap_, 0);
    }
    b.x.assign(x.begin(), x.end());
    b.gen = gen_;
    b.seq = seq_++;
    b.cw = !transmitting_ && cw_.feed(x);
    if (!transmitting_ && preview_on_ && on_preview_) {
        // a burst can end and decode inside one block (Receiver::MARGIN is under one): the
        // preview reads up to its end first, so it holds every data symbol
        const auto& p = receiver_.pending();
        const auto k = p ? static_cast<std::size_t>(std::clamp<std::int64_t>(
                               p->end() - receiver_.stream_end(), 0, static_cast<std::int64_t>(x.size())))
                         : std::size_t{0};
        if (k > 0 && k < x.size()) {
            b.items = receiver_feed(x.first(k));
            preview();
        }
        auto more = receiver_feed(x.subspan(k < x.size() ? k : 0));
        b.items.insert(b.items.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
        preview();
    } else if (!transmitting_) {
        b.items = receiver_feed(x);  // our own transmission is not heard
    }
    // a suspect OFDM lock with a CW ID keyed through it: dropped (engine.py _cw_false_lock)
    if (const auto& p = receiver_.pending(); p && !p->is_cpm() &&
        p->score() < receiver::Receiver::SUSPECT_SCORE && p->start() <= cw_.hits_from()) {
        log_write(LOG, INFO, format("RX %s header (score %.2f) under a CW ID: dropped", spec_name(*p).c_str(), p->score()));
        receiver_.drop();
    }
    b.busy = receiver_busy();
    busy_now_ = b.busy;
    channel_busy_now_ = receiver_channel_busy() || b.cw;
    if (!cfg_.worker) {
        run_posted();
        Done d = process_safe(b);
        if (after_block_) after_block_(d.out.ptt);
        return std::move(d.out);
    }
    const bool slow = std::any_of(b.items.begin(), b.items.end(),
                                  [](const auto& it) { return !std::holds_alternative<receiver::HeaderEvent>(it); });
    const std::uint64_t seq = b.seq;
    std::unique_lock lock(mu_);
    const bool lagging = slow_ > 0;  // a burst ahead is being received: don't wait for it
    if (slow) ++slow_;
    queued_ += b.x.size();
    in_.push_back(std::move(b));
    wake_.notify_one();
    if (!lagging && !slow) done_cv_.wait(lock, [&] { return n_done_ > seq || stop_; });
    return next_out(x.size());
}

// out_ needs no bound of its own: a step queues at most one block and takes
// one out when there is one, so out_ only grows as in_ drains, and the two
// together hold in_'s bound plus a block or two.
Engine::Out Engine::next_out(std::size_t k) {
    // catching up: silence the session stage made while it lagged is dropped
    while (out_.size() > 1 && !out_.front().sound) out_.pop_front();
    if (out_.empty()) return silence(k);
    Out o = std::move(out_.front().out);
    out_.pop_front();
    return o;
}

void Engine::work() {
    std::unique_lock lock(mu_);
    while (true) {
        wake_.wait(lock, [&] { return stop_ || !in_.empty() || !posted_.empty(); });
        if (stop_) return;
        if (in_.empty()) {
            lock.unlock();
            run_posted();
            lock.lock();
            continue;
        }
        Block b = std::move(in_.front());
        in_.pop_front();
        queued_ -= b.x.size();
        const bool slow = std::any_of(b.items.begin(), b.items.end(),
                                      [](const auto& it) { return !std::holds_alternative<receiver::HeaderEvent>(it); });
        lock.unlock();
        run_posted();
        Done d = process_safe(b);
        if (after_block_) after_block_(d.out.ptt);
        lock.lock();
        if (slow) --slow_;
        out_.push_back(std::move(d));
        ++n_done_;
        done_cv_.notify_all();
    }
}

// --- the session stage -----------------------------------------------------------------

// A throw here would end the worker thread (std::terminate) or the station's engine loop with
// the radio possibly keyed. Drop the session and the transmission instead, and keep listening.
Engine::Done Engine::process_safe(Block& b) {
    try {
        return process(b);
    } catch (const std::exception& e) {
        log_write(LOG, 40, format("session stage: %s: session dropped", e.what()));
        abort();
        return {silence(b.x.size()), false};
    }
}

Engine::Done Engine::process(Block& b) {
    if (b.gap) {  // dropped blocks before this one: their time passes, recorded as silence
        if (rec_)
            for (std::int64_t left = b.gap; left > 0; left -= config::FS)
                rec_->audio(std::vector<double>(static_cast<std::size_t>(std::min<std::int64_t>(left, config::FS)), 0.0));
        n_ += b.gap;
    }
    const auto k = static_cast<std::int64_t>(b.x.size());
    const double t = now() + static_cast<double>(k) / config::FS;  // the block's end: when anything in it is known
    Done d;
    d.out.audio.assign(static_cast<std::size_t>(k), 0.0);
    if (rec_) rec_->audio(tx_ ? d.out.audio : b.x);
    if (!tx_) {
        noise_.feed(b.x, now());
        const bool fresh = b.gen == want_gen_;  // else fed before a reset this stage asked for
        if (fresh) hear(b.items, t);
        const bool cw = fresh && b.cw;
        if (cw) cw_hold(t);
        cw_heard_ = cw;
        id_check(t);
        const bool busy = (fresh && b.busy) || cw;
        std::vector<TxBurstPtr> bursts;
        TxBurstPtr main;
        const bool held = t < hold_ && session_->state != SessionState::CLOSED;  // a closed session's DISC_ACK goes
        const bool free = t >= hold_ && t >= cw_until_;  // for anything outside a session
        if (!busy && !held) {  // a burst still arriving holds any reply (half duplex)
            carrier_hold(t);
            auto burst = session_->poll(t);
            missed_energy();
            id_check(t);
            if (burst) {
                bursts = with_id(burst, t);
                main = burst;
            } else if (id_pending_ && t >= id_pending_t_) {
                bursts = ids(*id_pending_, false);
                id_pending_.reset();
            } else if (!extra_.empty() && free) {
                main = extra_.front();
                extra_.pop_front();
                bursts = cw_ahead({main}, t);
            }
        }
        if (bursts.empty() && cfg_.kiss && free && (idle() || session_->idle_connected())) {  // KISS between ARQ sessions, or while one is idle
            if (auto kb = kiss_burst(k, busy)) {
                bursts = cw_ahead({kb}, t);
                main = kb;
            }
            tx_kiss_ = !bursts.empty();
        } else kiss_busy_ = 0;
        if (bursts.empty() && cw_due(t, free && !busy && !held)) bursts = {cw_burst(call_)};
        if (!bursts.empty()) start_tx(bursts, t, main ? main : bursts.front());
    }
    if (tx_) {
        if (tx_->pos == 0) d.out.tx_burst = std::make_shared<const std::vector<double>>(tx_->audio);
        const auto n = std::min<std::size_t>(static_cast<std::size_t>(k), tx_->audio.size() - tx_->pos);
        std::copy_n(tx_->audio.begin() + static_cast<std::ptrdiff_t>(tx_->pos), n, d.out.audio.begin());
        tx_->pos += n;
        d.sound = true;
        if (tx_->pos >= tx_->audio.size()) {
            const TxBurstPtr sent = tx_->burst;
            tx_.reset();
            noise_.mark(now(), now() + static_cast<double>(n) / config::FS + receiver::NoiseProfile::RECOVER_S);
            tx_end_ = now() + static_cast<double>(n) / config::FS;
            if (!tx_kiss_) {
                session_->on_tx_end(sent, now() + static_cast<double>(n) / config::FS);  // a broadcast burst is not the session's: no reply timer
                expect_reply();  // with the reset below
            }
            request_reset();  // our own transmission was not heard
            tx_kiss_ = false;
            if (cfg_.kiss) cfg_.kiss->on_sent(sent);  // a broadcast burst's frames are acked
        }
    }
    n_ += k;
    d.out.tx_abort = abort_pending_.exchange(false);
    d.out.ptt = tx_.has_value();
    transmitting_ = d.out.ptt;
    return d;
}

// The pending burst's symbols that have arrived since the last block, added to preview_ and
// handed on. A new lock (a new burst, or a superseding one) starts over.
void Engine::preview() {
    const auto& p = receiver_.pending();
    if (!p) return;
    if (preview_.start != p->start()) {
        preview_ = {};
        preview_.start = p->start();
        preview_.out.submode = spec_name(*p);
    }
    const std::int64_t end = receiver_.stream_end();
    auto& out = preview_.out;
    bool added = false;
    if (!p->is_cpm()) {
        const auto& l = p->ofdm();
        const int n_f = l.n_cw * l.spec->frames_per_cw;
        const auto kc = modem::copy_frame(l.spec->sync_band, n_f);
        const int n_air = n_f + kc.has_value();
        out.ofdm = l.spec;
        for (; preview_.done < n_air; ++preview_.done) {
            const std::int64_t at = l.p0 + static_cast<std::int64_t>(preview_.done) * config::FRAME_SAMPLES;
            if (at + modem::PREVIEW_SAMPLES > end) break;
            if (preview_.done == kc) continue;  // the header's copy, not data
            const auto x = receiver_.audio(at, modem::PREVIEW_SAMPLES);
            if (x.empty()) continue;
            const auto y = modem::preview_frame(x, l.cfo, *l.spec, n_f);
            out.iq.insert(out.iq.end(), y.begin(), y.end());
            added = true;
        }
    } else {
        const auto& l = p->cpm();
        const auto& g = cpm::grid_of(*l.spec);
        const auto L = cpm::layout(g, cpm::stream_symbols(g, l.n_data, l.dup));
        const auto T = static_cast<std::int64_t>(g.T);
        const int have = static_cast<int>(std::min<std::int64_t>(L.n, std::max<std::int64_t>(0, (end - l.start) / T)));
        if (have > preview_.done) {
            const auto x = receiver_.audio(l.start + preview_.done * T, (have - preview_.done) * T);
            if (!x.empty()) {
                const auto E = cpm::shares(cpm::energies(g, x, 0, have - preview_.done, l.cfo));
                if (out.shares.cols == 0) out.shares.cols = E.cols;
                for (int r = preview_.done; r < have; ++r)
                    if (std::binary_search(L.data_rows.begin(), L.data_rows.end(), r)) {
                        const auto* row = E[static_cast<std::size_t>(r - preview_.done)];
                        out.shares.data.insert(out.shares.data.end(), row, row + E.cols);
                        ++out.shares.rows;
                        added = true;
                    }
            }
            preview_.done = have;
        }
    }
    if (added) on_preview_(out);
}

void Engine::hear(std::vector<receiver::Receiver::Item>& items, double t) {
    for (auto& it : items) {
        if (auto* h = std::get_if<receiver::HeaderEvent>(&it)) {
            session_->on_header(spec_name(h->header), h->header.n_cw(), t);
            // the burst's span: its lead-in (the header's start, stream_end
            // samples ago being now, t) to its end (engine.py)
            const Mode* m = mode(spec_name(h->header));
            const double start =
                t - static_cast<double>(h->stream_end - h->header.start() + config::LEADIN_SAMPLES) / config::FS;
            noise_.mark(start, start + (m ? burst_seconds(*m, h->header.n_cw()) : MAX_BURST_S));
            span_ = m ? std::optional(Span{start, start + burst_seconds(*m, h->header.n_cw()), std::string(m->name)})
                      : std::nullopt;
            continue;
        }
        heard_span_ = std::move(span_);
        span_.reset();
        receiver::BurstEvent ev = std::holds_alternative<receiver::DecodeRequest>(it)
                                 ? receiver::decode(std::move(std::get<receiver::DecodeRequest>(it)), accept_)
                                 : std::move(std::get<receiver::BurstEvent>(it));
        hear_burst(ev, t);
    }
}

void Engine::energy(double start, double end, const std::string& mode, double t, std::optional<double> min_db,
                    std::optional<double> floor_db) {
    // floor_db: the burst's pilot snr_est, measured: the reading is at least
    // that less ENERGY_FLOOR_DB (receiver AGC lifts the gaps' noise under a strong signal, engine.py)
    std::optional<double> e;
    if (const auto snr = span_snr({start, end, mode})) e = *snr + peak_db(mode, rx_cap());
    if (e && floor_db) e = std::max(*e, *floor_db - ENERGY_FLOOR_DB);  // a reading floored, never one made up
    if (e && (!min_db || *e >= *min_db)) session_->policy->observe_energy(*e, t);
}

std::optional<double> Engine::span_snr(const Span& s) {
    const auto [lo, hi] = band_span_hz(mode_at(s.mode).band);
    return noise_.span_snr_db(noise_.span_spectra(s.start, s.end), lo, hi);
}

int Engine::rx_cap() const {
    // the session's (before a CONNECT is answered, my own), or between sessions the KISS link's
    if (cfg_.kiss && idle()) return cfg_.kiss->cap;
    return session_->cap;
}

void Engine::carrier_hold(double t) {
    // a timeout due, no header heard: while the last CARRIER_TAIL_S read as something on air, hold it, up to
    // the end of the reply I asked for (engine.py _carrier_hold)
    auto& s = *session_;
    const auto exp = s.policy->expected_reply();
    if (!s.deadline || t < *s.deadline || !tx_end_ || !exp) return;
    const double start = *tx_end_ + EXPECT_REPLY_S;
    const double until = start + exp->second + s.t_turn;  // the reply's end, as on_header waits for one heard
    if (until <= t) return;
    const auto [lo, hi] = band_span_hz(mode_at(exp->first).band);
    const auto z = noise_.span_z(noise_.span_spectra(std::max(start, t - CARRIER_TAIL_S), t), lo, hi);
    if (z && *z >= CARRIER_Z) {
        if (carrier_for_ != tx_end_) {
            carrier_for_ = tx_end_;
            log_write(LOG, INFO, format("no reply header, but energy on air: holding the timeout (up to %.1f s)", until - t));
        }
        s.deadline = std::min(until, t + CARRIER_STEP_S);
    }
}

void Engine::missed_energy() {
    const auto& st = session_->station;
    const std::int64_t n = st ? st->stats["timeouts"] : 0;
    if (n <= timeouts_) {
        timeouts_ = n;
        return;
    }
    timeouts_ = n;
    const auto exp = session_->policy->expected_reply();
    if (!tx_end_ || !exp) return;
    const double start = *tx_end_ + EXPECT_REPLY_S;
    energy(start, start + exp->second, exp->first, now(), MISSED_MIN_DB);
}

void Engine::hear_burst(receiver::BurstEvent& ev, double t) {
    // DEBUG: what this burst cost to decode, stage by stage, from the first look at it to the session having
    // read it (soft bits, DD, LDPC, and the session's own work), for finding what is slow on a slow machine.
    using Clock = std::chrono::steady_clock;
    struct Timing {
        receiver::BurstEvent& ev;
        Clock::time_point t0 = Clock::now();
        std::vector<std::pair<const char*, Clock::time_point>> marks{};  // each stage's name and when it ended
        const ModemRx* rx = nullptr;
        void mark(const char* stage) { marks.emplace_back(stage, Clock::now()); }
        ~Timing() {
            if (!log_enabled(LOG, 10)) return;
            const auto ms = [](Clock::time_point a, Clock::time_point b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            const auto t1 = Clock::now();
            std::string stages = format("receive %.0f", ev.receive_ms);  // before t0: receiver::decode's, ahead of this
            auto prev = t0;
            for (const auto& [name, at] : marks) {
                stages += format(", %s %.0f", name, ms(prev, at));
                prev = at;
            }
            stages += format(", rest %.0f ms", ms(prev, t1));
            log_write(LOG, 10,
                      format("RX decode %s x%d: %.0f ms (%s)%s", spec_name(ev.header).c_str(), ev.header.n_cw(), ev.receive_ms + ms(t0, t1),
                             stages.c_str(),
                             rx && rx->dd_refines()
                                 ? format(", DD: %d re-estimates%s", rx->dd_refines(), rx->dd_spent() ? ", budget spent" : "").c_str()
                                 : ""));
        }
    } timing{ev};
    std::optional<Heard> r;
    std::optional<Measured> meas;
    std::shared_ptr<const SlotSoft> soft;
    if (ev.rx) {
        r = heard_of(std::move(*ev.rx));
        meas = measure(*r);
        soft = soft_bits(*r);
    }
    const std::optional<Span> span = std::move(heard_span_);
    heard_span_.reset();
    // heard (decoded or not) in a session: the peer's, most likely
    if (span && (session_->state == SessionState::CONNECTED || session_->state == SessionState::DISCONNECTING))
        energy(span->start, span->end, span->mode, t, std::nullopt, meas ? std::optional(meas->snr_est) : std::nullopt);
    timing.mark("soft bits");
    if (rec_) rec_->rx(t, ev.audio, ev.header, !r, meas, noise_.snapshot());
    timing.mark("recorder");
    if (on_burst_)
        on_burst_({t, spec_name(ev.header), ev.header.n_cw(), !r, meas ? std::optional(meas->snr_est) : std::nullopt,
                   r ? *r : Heard{}, soft});
    timing.mark("hook");
    if (!r) {
        if (log_enabled(LOG, INFO))
            log_write(LOG, INFO, format("RX %s x%d: header heard (score %.2f), burst lost", spec_name(ev.header).c_str(),
                                        ev.header.n_cw(), ev.header.score()));
        if (cfg_.kiss && idle())  // maybe an open port's
            cfg_.kiss->missed(spec_name(ev.header), ev.header.n_cw(), span ? span_snr(*span) : std::nullopt);
        return;
    }
    ModemRx rx(*r, &store_, cfg_.dd_budget_s, soft, cfg_.dd);
    timing.rx = &rx;
    struct Tally {  // whichever return below: what decoded, for on_decoded
        const ModemRx& rx;
        const std::function<void(int, int)>& f;
        ~Tally() {
            if (f) f(rx.n_decoded(), rx.n_failed());
        }
    } tally{rx, on_decoded_};
    // in a session, its peer's bursts are the likely ones: the session claims
    // the burst before KISS tries its keys on it (asked with or without KISS:
    // its decode comes first either way)
    const bool ours = session_->claims(rx);
    if (ours) {
        session_snr_ = meas->snr_est;
        // its start from my burst's end (the receiver was reset there): the expected slot learns
        if (r->cpm) slot_.heard(static_cast<double>(ev.header.start()) / config::FS, r->cpm->cfo);
    }
    if (cfg_.kiss && !ours) {
        if (auto frames = cfg_.kiss->on_burst(*r, rx)) {  // a broadcast burst: not the session's
            kiss_rx_.insert(kiss_rx_.end(), std::make_move_iterator(frames->begin()), std::make_move_iterator(frames->end()));
            return;
        }
    }
    if (cq(rx)) return;
    // a whole codeword under another key: its measurements and its loss are not my peer's
    if (!ours && rx.converged(0, false) && session_->foreign(rx)) {
        log_write(LOG, INFO, format("RX %s x%d: another station's", rx.submode().c_str(), rx.n_cw()));
        return;
    }
    timing.mark("probe");
    // the noise profile too (the gear shifter's noise rule reads it)
    Measured m = *meas;
    if (const auto s = noise_.snapshot()) m.noise = NoiseLevels{s->db, s->tail_db, s->impulses_per_min};
    session_->policy->observe(m, rx.submode(), t);
    timing.mark("observe");
    session_->on_rx(rx, t);
}

// The next KISS burst if it may go now: after waiting on BUSY, p-persistent
// slots; on a free channel at once; BUSY holds it, but not past
// busy_limit_s of it unbroken.
TxBurstPtr Engine::kiss_burst(std::int64_t k, bool busy) {
    auto& link = *cfg_.kiss;
    if (link.queue.empty()) {
        kiss_busy_ = 0;
        kiss_deferred_ = false;
        return nullptr;
    }
    if (busy) {
        kiss_busy_ += k;
        kiss_deferred_ = true;
        if (static_cast<double>(kiss_busy_) < link.busy_limit_s * config::FS) return nullptr;
        log_write(LOG, WARNING, format("KISS: BUSY for %.0f s, sending anyway", static_cast<double>(kiss_busy_) / config::FS));
    } else {
        kiss_busy_ = 0;
        if (kiss_deferred_) {
            if (n_ < kiss_slot_) return nullptr;
            if (rng_->uniform(0.0, 1.0) >= (link.persist + 1) / 256.0) {
                kiss_slot_ = n_ + static_cast<std::int64_t>(link.slot_s * config::FS);
                return nullptr;
            }
        }
    }
    kiss_busy_ = 0;
    kiss_deferred_ = false;
    return link.next_burst();
}

// A CQ frame (notified: CQFRAME call cap) or an ID frame (ID call key)? Then
// nothing else to do. A malformed one (docs/arq.md §4) is dropped as if it
// had not decoded.
bool Engine::cq(ModemRx& rx) {
    const auto first = rx.decode(0, ctl_mask(0, 0, 0), 0, nullptr);
    if (!first) return false;
    const Core core = Core::unpack(*first);
    if (core.ftype != SESSION || core.n_ctl > rx.n_cw()) return false;
    std::vector<Bytes> payloads = {*first};
    for (int i = 1; i < core.n_ctl; ++i) {
        auto p = rx.decode(i, ctl_mask(0, i, 0), 0, nullptr);
        if (!p) return false;
        payloads.push_back(std::move(*p));
    }
    try {
        const auto ext = Control::unpack(payloads).ext;
        if (const auto it = ext.find(T_ID); it != ext.end()) {
            const auto [call, key] = unpack_id(it->second);
            if (log_enabled(LOG, INFO)) log_write(LOG, INFO, format("RX ID %s (session %04x)", call.c_str(), key));
            events_.push_back("ID " + call + " " + std::to_string(key));
            return true;
        }
        const auto it = ext.find(T_CQ);
        if (it == ext.end()) return false;
        const auto [call, cap] = unpack_cq(it->second);
        events_.push_back("CQFRAME " + call + " " + std::to_string(cap));
        return true;
    } catch (const std::invalid_argument& e) {
        log_write(LOG, WARNING, format("RX malformed CQ/ID frame (%s): dropped", e.what()));
        return false;
    }
}

void Engine::start_tx(const std::vector<TxBurstPtr>& bursts, double t, const TxBurstPtr& main) {
    Tx tx{main, std::vector<double>(static_cast<std::size_t>(ptt_delay_), 0.0), 0};
    std::vector<std::size_t> lens;
    for (const auto& b : bursts) {
        if (b->submode == "cw") {  // built at the bursts' peak; not the tx_audio seam's (a Python phy.tx_audio)
            const auto x = cwid::audio(b->cw_text, b->cw_wpm);
            tx.audio.insert(tx.audio.end(), x.begin(), x.end());
            lens.push_back(x.size());
            continue;
        }
        const auto x = tx_audio(*b);
        // envelope peak at full scale: the modem's unit-RMS audio peaks at 2-3, and a sound
        // card clips at 1; the envelope, not the samples: w48's crests fall up to 1.8 dB
        // between samples (engine.py)
        double peak = 0.0;
        for (const dsp::cdouble& z : dsp::hilbert(x)) peak = std::max(peak, std::abs(z));
        for (double v : x) tx.audio.push_back(v / peak);
        lens.push_back(x.size());
        if (on_send_) on_send_(b);
    }
    cw_plan(bursts, lens, t);
    if (rec_) {
        // one event per burst, at its own start: the first's seconds include
        // the PTT delay, as a lone burst's always did (scripts/replay.py)
        for (std::size_t j = 0; j < bursts.size(); ++j) {
            const auto& burst = bursts[j];
            std::string slots = "[";
            for (std::size_t i = 0; i < burst->slots.size(); ++i) {
                const auto& s = burst->slots[i];
                if (i) slots += ", ";
                slots += json_obj({{"mask", "[" + std::to_string(s.mask_id.key) + ", " + std::to_string(s.mask_id.direction) +
                                                ", " + std::to_string(s.mask_id.seq) + "]"},
                                   {"rv", std::to_string(s.rv)},
                                   {"payload", json_str(hex(s.payload))}});
            }
            const double seconds =
                static_cast<double>(lens[j] + (j == 0 ? static_cast<std::size_t>(ptt_delay_) : 0)) / config::FS;
            rec_->event("tx", {{"t", json_num(t)},
                               {"submode", json_str(burst->submode)},
                               {"burst_seq", std::to_string(burst->burst_seq)},
                               {"slots", slots + "]"},
                               {"seconds", json_num(seconds)}});
            t += seconds;
        }
    }
    tx_ = std::move(tx);
}

}  // namespace data2g::arq
