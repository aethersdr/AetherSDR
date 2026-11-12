// A promiscuous receiver: every burst heard as a packet dump, whoever it is
// for (data2g-monitor, the GUI's Monitor window). Nothing here transmits.
//
// Everything on air is plain (docs/arq.md §2): the scrambler is unkeyed and
// a mask only decides whose CRC check passes. So a burst is read under every
// mask it might carry, in order: a broadcast group's (the control names it,
// docs/broadcast.md §2), mask 0 (CONNECT, CONNECT_ACK, CQ and ID frames),
// then each session key learned from a CONNECT or an ID frame, both
// directions. A connected-mode burst under a known key is followed as its
// receiver would: resends mapped from the other side's last ACK, each
// direction's codewords reassembled in seq order and inflated, so the dump
// shows the host bytes. A burst no mask fits is dumped raw, unverified.
//
// A station that monitors while it works also shows the monitor its own
// bursts as it sends them (sent()), read from their payloads rather than
// heard: our side of a session is followed like any other.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "arq/engine.hpp"

namespace data2g::monitor {

using arq::Bytes;
using arq::ByteView;

enum class Format { TEXT, HEX };

struct Payload {
    std::string label;
    Bytes bytes;
    std::vector<bool> unknown;  // per byte: not known (copied from history never heard); empty: all known
};

// One burst: its description, then its payloads.
struct Dump {
    std::string head;  // lines, each ending in '\n'
    std::vector<Payload> payloads;
    bool tx = false;  // ours, sent (Monitor::sent), not heard
};

// Non-printables as <AB>, unknown bytes as <??>; a line break follows each <0A>.
std::string text_dump(ByteView b, const std::vector<bool>& unknown = {});
// As xxd: 16 bytes a line, offset, hex in pairs, then the bytes with dots
// for non-printables. Unknown bytes: ?? and ?.
std::string hex_dump(ByteView b, const std::vector<bool>& unknown = {});
// The dump, each payload line indented.
std::string render(const Dump& d, Format f);

class Monitor {
public:
    // A burst the engine heard (BurstHeard::heard set unless lost). `when`
    // starts the first line: the caller's clock.
    Dump burst(const arq::BurstHeard& b, const std::string& when);
    // A burst we transmitted (Engine::set_on_send), as it goes out.
    Dump sent(const arq::TxBurst& b, const std::string& when);

    // Its own receiver, for the CLI: audio at FS -> the bursts it completes,
    // as (engine time s, dump). when(t) stamps each.
    std::vector<Dump> feed(std::span<const double> x, const std::function<std::string(double t)>& when);

private:
    struct Pair {
        std::string caller, callee;  // direction 0's and 1's station; empty: not known
        std::string id;              // heard in an ID frame for this key, side unknown
        std::uint64_t seen = 0;      // when last heard (seen_)
    };
    // One direction of a session: what its station has said it holds, and
    // the stream it sends, as reassembled here.
    struct Side {
        std::map<int, std::pair<int, std::set<int>>> snaps;  // its burst seq -> (cum, received above it), 7-bit
        arq::RxSide rx;  // cum, the codewords held above it, comp_seqs, plain (bytes delivered)
        bool started = false;
        bool fresh = false;  // its CONNECT was heard: the stream starts empty, history and framing known
        int epoch = 0;
        bool epoch_known = false;
        std::map<std::int64_t, arq::SoftEntry> soft;  // resends' soft bits, combined (IR) until one decodes
        // The deflate history as the sender has it (at most HIST), and which
        // of its bytes were never heard. After a gap it is HIST long, and
        // zdict_sure says whether the sender's is too (else ZDICT's place in
        // the window is unknown as well).
        Bytes hist;
        std::vector<bool> unk;
        bool zdict_sure = true;
        // Record framing: framed, with `left` bytes of the current record to
        // come; unframed after a gap, until zero padding ends a codeword.
        bool framed = true;
        int left = 0;
        Payload out;  // delivered, not yet in a dump
        bool raw = false;  // out holds bytes shown unframed
    };

    std::string who(int key, int direction) const;
    void learn(int key, const std::string& caller = {}, const std::string& callee = {}, const std::string& id = {});
    void read(arq::PlainRx& rx, Dump& d);
    bool broadcast(arq::PlainRx& rx, Dump& d);
    bool keyed(arq::PlainRx& rx, Dump& d);
    void session_frame(const Bytes& body, int key, int direction, Dump& d);
    void arq_burst(arq::PlainRx& rx, const arq::Control& ctl, int key, int direction, int dup, Dump& d);
    void start(Side& s, std::int64_t cum);
    void forget(Side& s);  // history and framing lost: a gap, or a stream joined late
    void take(Side& s, const Bytes& plain, const std::vector<bool>& unknown);  // a codeword's stream bytes, in order
    void deliver(Side& s, int key, int direction, std::int64_t seq, Bytes p, bool comp, Dump& d);
    void skip(Side& s, int key, int direction, std::int64_t to, Dump& d);  // jump the stream to `to` over what wasn't heard
    void flush(Side& s, int key, int direction, Dump& d);                  // its delivered bytes -> a payload

    std::map<int, Pair> keys_;                            // session key -> its stations
    std::map<int, std::pair<std::string, std::string>> connects_;  // CONNECT nonce -> (caller, callee)
    std::map<std::pair<int, int>, Side> sides_;           // (key, direction)
    std::uint64_t seen_ = 0;
    std::unique_ptr<receiver::Receiver> receiver_;        // feed()'s, made on first use
};

}  // namespace data2g::monitor
