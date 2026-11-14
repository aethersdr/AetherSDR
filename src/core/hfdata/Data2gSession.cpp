#include "core/hfdata/Data2gSession.h"

#include "core/hfdata/Data2gMonitor.h"

#include "arq/engine.hpp"
#include "arq/session.hpp"
#include "audio/filters.hpp"
#include "config/config.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <iterator>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>

namespace AetherSDR::hfdata {

namespace {

struct ConnectRequest {
    std::string peer;
    Data2gBandwidth bandwidth;
};
struct DisconnectRequest {};
struct AbortRequest {};
struct SendRequest {
    std::vector<std::uint8_t> bytes;
};
using Request = std::variant<ConnectRequest, DisconnectRequest, AbortRequest, SendRequest>;

int capOf(Data2gBandwidth bandwidth)
{
    switch (bandwidth) {
    case Data2gBandwidth::Hz500:
        return 0;
    case Data2gBandwidth::Hz1200:
        return 1;
    case Data2gBandwidth::Hz2400:
        return 2;
    }
    return 0;
}

Data2gLinkState linkStateOf(data2g::arq::SessionState state)
{
    using data2g::arq::SessionState;
    switch (state) {
    case SessionState::CONNECTING:
        return Data2gLinkState::Connecting;
    case SessionState::CONNECTED:
        return Data2gLinkState::Connected;
    case SessionState::DISCONNECTING:
        return Data2gLinkState::Disconnecting;
    case SessionState::IDLE:
    case SessionState::LISTEN:
    case SessionState::CLOSED:
        break;
    }
    return Data2gLinkState::Idle;
}

} // namespace

struct Data2gSession::Impl {
    std::string myCall;
    int sampleRate{0};
    std::optional<std::uint64_t> seed;
    bool valid{false};
    std::optional<data2g::audio::Decimator> decimator;
    std::unique_ptr<data2g::arq::Engine> engine;
    std::vector<double> scratch;

    std::mutex requestMutex;
    std::vector<Request> requests;
    std::atomic<double> keyLeadSeconds{kDefaultKeyLeadSeconds};
    double appliedKeyLeadSeconds{kDefaultKeyLeadSeconds};

    // An operator session is under way, or ended less than
    // kClosingIdSeconds of audio ago.
    bool operatorSession{false};
    bool sessionEnded{false};
    double endedSeconds{0.0};
    // What the engine keyed during the step in hand, from its on_send hook.
    int burstsSent{0};
    bool refusalSent{false};
    // The engine is playing out a transmission that was dropped here.
    bool suppressing{false};
    std::uint64_t suppressed{0};
    std::uint64_t faults{0};

    void restart()
    {
        decimator.emplace(sampleRate);
        data2g::arq::EngineConfig config;
        config.seed = seed;
        config.ptt_delay_s = keyLeadSeconds.load();
        appliedKeyLeadSeconds = config.ptt_delay_s;
        engine = std::make_unique<data2g::arq::Engine>(myCall, config);
        engine->set_call(myCall);   // throws for a call that will not pack
        // Runs inside step(), on its thread, as each burst is keyed. A
        // refusal is the one burst the session queues that is not its own.
        engine->set_on_send([this](const data2g::arq::TxBurstPtr& burst) {
            ++burstsSent;
            if (burst && burst == engine->session().nak)
                refusalSent = true;
        });
        operatorSession = false;
        sessionEnded = false;
        suppressing = false;
    }

    bool sessionActive() const
    {
        return linkStateOf(engine->session().state) != Data2gLinkState::Idle;
    }

    // A whole transmission at the caller's rate. Each one starts from a
    // clean filter: it opens with the key lead's silence, and a short run of
    // zeros after it flushes the filter's tail.
    std::vector<float> atCallerRate(const std::vector<double>& atFs) const
    {
        data2g::audio::Interpolator interpolator(sampleRate);
        std::vector<double> padded(atFs);
        padded.insert(padded.end(), 64, 0.0);
        const std::vector<double> atRate = interpolator(padded);
        return {atRate.begin(), atRate.end()};
    }
};

Data2gSession::Data2gSession(std::string myCall, int sampleRate)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->myCall = std::move(myCall);
    m_impl->sampleRate = sampleRate;
    if (!Data2gMonitor::isSupportedSampleRate(sampleRate))
        return;
    try {
        m_impl->restart();
        m_impl->valid = true;
    } catch (const std::exception&) {
        m_impl->engine.reset();
    }
}

Data2gSession::Data2gSession(std::string myCall, int sampleRate, std::uint64_t seed)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->myCall = std::move(myCall);
    m_impl->sampleRate = sampleRate;
    m_impl->seed = seed;
    if (!Data2gMonitor::isSupportedSampleRate(sampleRate))
        return;
    try {
        m_impl->restart();
        m_impl->valid = true;
    } catch (const std::exception&) {
        m_impl->engine.reset();
    }
}

Data2gSession::~Data2gSession() = default;

bool Data2gSession::isValid() const
{
    return m_impl->valid;
}

int Data2gSession::sampleRate() const
{
    return m_impl->sampleRate;
}

void Data2gSession::requestConnect(std::string peerCall, Data2gBandwidth bandwidth)
{
    std::lock_guard lock(m_impl->requestMutex);
    m_impl->requests.emplace_back(ConnectRequest{std::move(peerCall), bandwidth});
}

void Data2gSession::requestDisconnect()
{
    std::lock_guard lock(m_impl->requestMutex);
    m_impl->requests.emplace_back(DisconnectRequest{});
}

void Data2gSession::requestAbort()
{
    std::lock_guard lock(m_impl->requestMutex);
    m_impl->requests.emplace_back(AbortRequest{});
}

void Data2gSession::send(std::vector<std::uint8_t> bytes)
{
    std::lock_guard lock(m_impl->requestMutex);
    m_impl->requests.emplace_back(SendRequest{std::move(bytes)});
}

void Data2gSession::setKeyLeadSeconds(double seconds)
{
    m_impl->keyLeadSeconds.store(std::clamp(seconds, 0.0, 1.0));
}

std::uint64_t Data2gSession::suppressedTransmissions() const
{
    return m_impl->suppressed;
}

std::uint64_t Data2gSession::faultCount() const
{
    return m_impl->faults;
}

Data2gSessionStep Data2gSession::step(std::span<const float> mono)
{
    Data2gSessionStep out;

    std::vector<Request> requests;
    {
        std::lock_guard lock(m_impl->requestMutex);
        requests.swap(m_impl->requests);
    }
    if (!m_impl->valid) {
        for (const Request& request : requests)
            if (std::holds_alternative<ConnectRequest>(request))
                out.events.emplace_back("REFUSED the session engine is not available");
        return out;
    }

    Impl& impl = *m_impl;
    try {
        data2g::arq::Engine& engine = *impl.engine;
        if (const double lead = impl.keyLeadSeconds.load(); lead != impl.appliedKeyLeadSeconds) {
            engine.set_ptt_delay(lead);
            impl.appliedKeyLeadSeconds = lead;
        }
        for (Request& request : requests) {
            if (auto* connect = std::get_if<ConnectRequest>(&request)) {
                try {
                    engine.connect(connect->peer, capOf(connect->bandwidth));
                    impl.operatorSession = true;
                    impl.sessionEnded = false;
                } catch (const std::exception& e) {
                    out.events.push_back(std::string("REFUSED ") + e.what());
                }
            } else if (std::holds_alternative<DisconnectRequest>(request)) {
                if (impl.sessionActive())
                    engine.disconnect();
            } else if (std::holds_alternative<AbortRequest>(request)) {
                engine.abort();
                // Nothing more goes out for this session, its closing
                // identification included.
                impl.operatorSession = false;
                impl.sessionEnded = false;
            } else if (auto* send = std::get_if<SendRequest>(&request)) {
                if (linkStateOf(engine.session().state) == Data2gLinkState::Connected)
                    engine.session().write(send->bytes);
                else
                    out.events.emplace_back("REFUSED not connected");
            }
        }

        impl.scratch.assign(mono.begin(), mono.end());
        const std::vector<double> atFs = (*impl.decimator)(impl.scratch);
        impl.burstsSent = 0;
        impl.refusalSent = false;
        const data2g::arq::Engine::Out engineOut = engine.step(atFs);
        out.abortTx = engineOut.tx_abort;

        std::vector<std::string> events = engine.events();
        out.events.insert(out.events.end(), std::make_move_iterator(events.begin()),
                          std::make_move_iterator(events.end()));
        out.received = engine.session().read();
        out.state = linkStateOf(engine.session().state);

        // The window in which this station may transmit: from an accepted
        // requestConnect() until kClosingIdSeconds of audio after that
        // session has ended. It is settled on this step's clock before
        // anything is handed back, so a step of any length that carries the
        // clock past the deadline releases nothing.
        if (impl.operatorSession) {
            if (out.state != Data2gLinkState::Idle) {
                impl.sessionEnded = false;
            } else if (!impl.sessionEnded) {
                impl.sessionEnded = true;
                impl.endedSeconds = engine.now();
            }
            if (impl.sessionEnded && engine.now() - impl.endedSeconds > kClosingIdSeconds) {
                impl.operatorSession = false;
                impl.sessionEnded = false;
            }
        }

        if (engineOut.tx_burst) {
            // Ours only if an operator session covers it, the engine named
            // what it keyed, and none of it is a refusal. A transmission is
            // all or nothing: an identification riding with a refusal is
            // dropped with it.
            const bool ours = impl.operatorSession && impl.burstsSent > 0 && !impl.refusalSent;
            if (ours)
                out.transmission = impl.atCallerRate(*engineOut.tx_burst);
            else
                ++impl.suppressed;
            impl.suppressing = !ours;
        }
        // The engine plays a dropped transmission out on its own clock. It is
        // not reported as transmitting: nothing here is on the air.
        if (!engineOut.ptt)
            impl.suppressing = false;
        out.transmitting = engineOut.ptt && !impl.suppressing;
    } catch (const std::exception&) {
        // The audio is untrusted (Principle VII), and a core in an unknown
        // state must not be left with a transmission in hand: drop the
        // session.
        ++impl.faults;
        out.transmission.clear();
        out.transmitting = false;
        out.abortTx = true;
        out.state = Data2gLinkState::Idle;
        out.received.clear();
        out.events.emplace_back("DISCONNECTED FAULT");
        try {
            impl.restart();
        } catch (const std::exception&) {
            impl.engine.reset();
            impl.valid = false;
        }
    }
    return out;
}

} // namespace AetherSDR::hfdata
