// data2g/receiver.py: the live receive front end. Receiver: feed() audio
// at FS as it arrives, get events back: a burst's header as it is found
// (preamble search over the StreamDetectors' statistics, a CPM early lock,
// or the header copy of a burst whose head faded), BUSY while it arrives,
// then the whole burst's receive. feed_deferred() leaves each receive as a
// DecodeRequest, so arq::Engine can run decode() on its worker. Also the
// offline search (search_span, receive_any). NoiseProfile is
// noise_profile.hpp.
//
// Positions in events are stream sample indices (samples fed since reset).
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "audio/filters.hpp"
#include "cpm/cpm.hpp"
#include "modem/modem.hpp"
#include "waveform/sync.hpp"

namespace data2g::receiver {

inline constexpr double SILENCE_RMS = 1e-4;  // -80 dBFS: digital silence, no search

// Samples holding a whole preamble and header (every copy) of any of these.
std::int64_t search_span(std::span<const std::string_view> bands, std::span<const std::string_view> cpm_grids = {});

// What a burst's receive gives: OFDM up to soft bits, or CPM's soft bits.
using Rx = std::variant<modem::Received, cpm::Received>;

// receiver.receive_any: one burst starting within `lead` samples of y's start,
// OFDM first; nullopt if none. cpm_grids nullopt: every grid.
std::optional<Rx> receive_any(std::span<const double> y, std::int64_t lead = 0,
                              std::optional<std::vector<std::string_view>> cpm_grids = std::nullopt);

// A burst being received: find_burst's / find_copy's lock or a CPM early
// lock. A CPM lock's header_end stays the buffer index it was found at (as
// Python's dict(p, start=..., end=...) leaves it).
struct Pending {
    std::variant<modem::Lock, cpm::Lock> lock;

    bool is_cpm() const { return lock.index() == 1; }
    const modem::Lock& ofdm() const { return std::get<0>(lock); }
    const cpm::Lock& cpm() const { return std::get<1>(lock); }
    std::int64_t start() const;
    std::int64_t end() const;
    double score() const;
    int n_cw() const;  // CPM: slots, control included
    bool copy() const { return !is_cpm() && ofdm().copy.has_value(); }
    void shift(std::int64_t d);  // every position (but a CPM header_end) by d
};

struct HeaderEvent {
    Pending header;
    std::int64_t stream_end = 0;  // the stream index of the audio fed so far (the caller's now)
};
struct BurstEvent {
    Pending header;
    std::optional<Rx> rx;  // nullopt: lost
    std::vector<double> audio;  // the segment received
    double receive_ms = 0;      // wall time modem::receive took (sync check, header, channel estimate), for the log
};
using Event = std::variant<HeaderEvent, BurstEvent>;

// A complete burst's audio and how to receive it: what the receiver hands
// to a decode step (in line from feed(); feed_deferred() hands it back for
// arq::Engine to decode on its worker). Nothing the receiver does after a
// burst reads its rx (supersede and trim read only the audio), so decode()
// can run elsewhere and its BurstEvent be posted later, ordered by
// header.start().
struct DecodeRequest {
    Pending header;              // stream indices
    std::vector<double> seg;     // the audio
    std::int64_t at = 0;         // seg[0]'s stream index
    std::int64_t head = 0;       // OFDM, no copy: the preamble and header lie in seg[:head]
    std::int64_t audio_lo = 0, audio_hi = 0;  // the event's audio: seg[audio_lo:audio_hi]
    std::shared_ptr<cpm::Stream> stream;      // CPM, streamed: its receive is stream->finish()
};
BurstEvent decode(DecodeRequest req, const modem::Accept& accept);

// A pending CPM lock's audio since the last, for its stream (cpm::Stream): the
// work a decode step does while the burst arrives, in order before the
// burst's DecodeRequest. Empty audio: the stream was just made.
struct StreamFeed {
    std::shared_ptr<cpm::Stream> stream;
    std::vector<double> audio;
};
void run(StreamFeed& f);

// The streaming receiver: feed() audio at FS as it arrives, get events
// back, in order: a HeaderEvent as a burst starts, then its BurstEvent.
// busy(): a burst is arriving (our own replies wait for it);
// channel_busy(): the host's BUSY (the pending burst's pilots or in-band
// energy say it is really there).
class Receiver {
public:
    static constexpr int HOP = config::FS / 4;
    static constexpr int MARGIN = 320;  // audio kept either side of an OFDM burst's span (receiver.Receiver.MARGIN)
    static constexpr double ON_AIR_DB = 3.0;
    static constexpr std::size_t FLOOR_BLOCKS = 1200;
    static constexpr int REVISIT = 2 * HOP;
    static constexpr int CPM_TAIL = config::FS / 20;  // kept before a CPM burst's end (receiver.Receiver.CPM_TAIL)
    static constexpr double SUPERSEDE_MARGIN = 0.05;
    static constexpr double SUSPECT_SCORE = 0.36;
    // receiver.Receiver.RELEASE_S: a suspect OFDM lock (a header copy under COPY_COMMIT_SCORE) whose pilots have been measurably absent this long in
    // a row (pilot checks, one a HOP) is let go: reported lost, the search goes on past it.
    // 0: never. A member so a study can set it (scripts/false_lock_study.py).
    double release_s = 3.0;

    explicit Receiver(modem::Accept accept, std::vector<std::string_view> cpm_grids = {}, bool blank = true);

    std::vector<Event> feed(std::span<const double> x);
    // feed() with each burst to decode left as its DecodeRequest, in order.
    using Item = std::variant<HeaderEvent, BurstEvent, DecodeRequest, StreamFeed>;
    std::vector<Item> feed_deferred(std::span<const double> x);
    // Stream CPM locks (cpm::Stream: refine and data slots' passes while the burst arrives); `pass`
    // (arq::cpm_pass's, the engine's) may be empty: refine alone. Off by default (as Python's receiver).
    void stream_cpm(cpm::PassFn pass) {
        stream_on_ = true;
        stream_pass_ = std::move(pass);
    }
    void reset();
    // The expected slot (receiver.Receiver.expect, docs/arq.md §3a): a burst of one of these CPM grids should
    // start between stream indices lo and hi, its CFO near cfo; one grid per search hop, in order, looked at
    // once (cpm::prime_lock) when the audio holds every second header copy of a start there and nothing has
    // locked. The window's audio is kept until the last is looked at.
    void expect(std::vector<std::string_view> grids, std::int64_t lo, std::int64_t hi, double cfo);
    std::size_t n_expected() const { return expected_ ? expected_->grids.size() : 0; }  // grids still queued
    // Forget the pending burst: its header was no burst's (a CW ID keyed over its preamble, arq::Engine)
    void drop() {
        last_start_ = pending_->start();
        pending_.reset();
        stream_.reset();
        pilots_ok_ = confirmed_ = false;
        dark_ = 0;
        looked_ = false;
    }

    bool busy() const { return pending_.has_value(); }
    bool channel_busy() const { return pending_ && (pilots_ok_ || on_air()); }
    bool on_air() const;
    const std::optional<Pending>& pending() const { return pending_; }
    // The (blanked) audio fed so far: its end's stream index, and n samples from stream index
    // `from` (empty unless all of them are still held).
    std::int64_t stream_end() const { return off_ + len(); }
    std::span<const double> audio(std::int64_t from, std::int64_t n) const {
        if (from < off_ || n < 0 || from + n > stream_end()) return {};
        return std::span<const double>(buf_).subspan(static_cast<std::size_t>(from - off_), static_cast<std::size_t>(n));
    }
    const modem::Accept& accept() const { return accept_; }
    std::int64_t n_blanked() const { return blanker_ ? blanker_->n_blanked : 0; }

private:
    struct Detector {
        std::string_view band;
        waveform::StreamDetector d;
    };
    struct Stats {  // per band, the statistic for buf[w0:]'s starts
        std::vector<Mat<double>> mats;
        std::vector<modem::BandStat> v;
    };

    void check_pilots();
    void trim(std::int64_t n);
    Stats stats(std::int64_t w0 = 0);
    void searched();
    bool supersede(std::vector<Item>& out, bool whole = false);
    bool release(std::vector<Item>& out);
    bool unconfirmed(std::vector<Item>& out);
    std::optional<modem::Lock> find_copy() const;
    std::optional<cpm::Lock> find_cpm();
    std::optional<cpm::Lock> primed();  // expect()'s lock in its next grid, buffer indices, once its window is complete
    std::int64_t decided(std::string_view key) const;
    std::int64_t decided_mid(const cpm::Grid& g) const;
    std::int64_t len() const { return static_cast<std::int64_t>(buf_.size()); }

    modem::Accept accept_;
    std::vector<std::string_view> bands_;
    std::vector<const cpm::Grid*> grids_;
    std::int64_t min_search_, keep_, hold_;
    std::vector<Detector> detectors_;
    std::optional<audio::Blanker> blanker_;

    std::vector<double> buf_;
    std::int64_t off_ = 0;  // stream index of buf_[0]
    std::optional<Pending> pending_;
    bool stream_on_ = false;
    cpm::PassFn stream_pass_;
    std::shared_ptr<cpm::Stream> stream_;  // the pending CPM lock's
    bool pilots_ok_ = false, confirmed_ = false;
    int dark_ = 0;  // the pending lock's latest pilot checks in a row with its pilots measurably absent
    bool looked_ = false;  // the pending CPM lock's second look is done (unconfirmed)
    std::deque<double> powers_;  // in-band power of 0.1 s blocks
    double ps_ = 0.0;
    int pn_ = 0;
    std::int64_t fresh_ = HOP, last_start_ = -1;
    std::map<std::string_view, std::int64_t> decided_;
    std::map<std::string_view, std::int64_t> decided_mid_;  // per CPM grid, its mid block's (Python's "<grid>/mid")
    // CPM fronts whose first header copy fell short (cpm::find's candidates,
    // stream indices): read again with both copies once the second is in
    struct CpmWait {
        const cpm::Grid* g;
        cpm::Lock cand;
        std::int64_t first;  // its earliest tile start: kept until read
    };
    std::vector<CpmWait> cpm_wait_;
    struct Expected {
        std::vector<const cpm::Grid*> grids;
        std::int64_t lo, hi;
        double cfo;
    };
    std::optional<Expected> expected_;
};

// receiver.ReplySlot: where my peer's next burst should start (past my burst's end: the median of the last
// GAPS heard) and its CFO (its last burst's).
class ReplySlot {
public:
    static constexpr double TOL_S = 0.15;
    static constexpr std::size_t GAPS = 8;
    static constexpr double MAX_GAP_S = 3.0;
    static constexpr double NOD_HALF_S = 0.6;  // nod_window's cap either side of the median
    void heard(std::optional<double> gap_s, std::optional<double> cfo);
    // (earliest, latest) start of the reply to my burst that ended at tx_end; nullopt before anything was learned
    std::optional<std::pair<double, double>> window(double tx_end) const;
    // window() for a nod (receiver.ReplySlot.nod_window): the gaps' spread +-TOL_S, at most NOD_HALF_S either
    // side of the median (a peer's TX playout lag can vary by a second; a nod has no blind fallback)
    std::optional<std::pair<double, double>> nod_window(double tx_end) const;
    std::optional<double> cfo;

private:
    std::deque<double> gaps_;
};


}  // namespace data2g::receiver
