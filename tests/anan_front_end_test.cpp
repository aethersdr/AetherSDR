// ANAN-G2 live front-end options: the receive band-pass banks and the speaker
// amplifier mute (RFC #6209).
//
// anan_p2_protocol_test pins the encoder. This pins what P2Client puts on the
// wire, read from the High Priority datagrams it really sends to a loopback
// listener: which filter each sender asks for as the receiver tunes, that every
// run=1 sender carries the options, and that the stop packet carries none. It
// then drives the "anan"/"frontend.set" verb through AnanBackend into the
// client that would send those packets.
//
// The bank sits in series ahead of the ADC, so a wrong or stale filter is
// silence, not a weaker signal. No radio: nothing replies, and nothing here
// needs a reply. No event loop runs while the listener waits, so the client's
// own keepalive timer never fires and every datagram counted is one a call
// below sent.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "core/backends/anan/AnanBackend.h"
#include "core/backends/anan/P2Client.h"
#include "core/backends/anan/P2Protocol.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QUdpSocket>
#include <QVariantMap>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace AetherSDR::anan {
// P2Client's friend: the keepalive tick without its timer, and the options the
// client resolves without a socket.
struct P2ClientTestAccess {
    static void keepalive(P2Client& c) { c.onKeepaliveTick(); }
    static HighPriorityOptions options(const P2Client& c) { return c.highPriorityOptions(); }
};

// AnanBackend's friend: the client the verb reaches, and the params a session
// restart resends.
class AnanFrontEndTestAccess {
public:
    static P2Client* client(AnanBackend& b) { return b.m_client; }
    static const P2Client::Params& pending(const AnanBackend& b) { return b.m_pendingParams; }
};
}  // namespace AetherSDR::anan

using namespace AetherSDR;
using namespace AetherSDR::anan;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

namespace {

// Waits until `expected` datagrams have arrived, then sweeps briefly for any
// the caller did not expect, so "one packet" cannot pass on two.
std::vector<QByteArray> collect(QUdpSocket& listener, int expected)
{
    std::vector<QByteArray> out;
    while (static_cast<int>(out.size()) < expected) {
        if (!listener.waitForReadyRead(250)) {
            break;
        }
        while (listener.hasPendingDatagrams()) {
            out.push_back(listener.receiveDatagram().data());
        }
    }
    if (listener.waitForReadyRead(60)) {
        while (listener.hasPendingDatagrams()) {
            out.push_back(listener.receiveDatagram().data());
        }
    }
    return out;
}

// The one High Priority packet a call sent, or an empty array when it sent
// none or more than one (the count check names which).
QByteArray one(QUdpSocket& listener, const char* what)
{
    const auto got = collect(listener, 1);
    check(got.size() == 1, what);
    if (got.size() != 1 || got[0].size() != 1444) {
        return {};
    }
    return got[0];
}

std::uint16_t u16At(const QByteArray& d, int i)
{
    if (d.size() < i + 2) {
        return 0xFFFF;   // matches no expected word
    }
    return static_cast<std::uint16_t>((static_cast<std::uint8_t>(d[i]) << 8)
                                      | static_cast<std::uint8_t>(d[i + 1]));
}

int byteAt(const QByteArray& d, int i)
{
    return d.size() > i ? static_cast<std::uint8_t>(d[i]) : -1;
}

// ADC0's receive word is bytes 1434-1435, ADC1's is 1430-1431.
bool bothBanks(const QByteArray& d, std::uint16_t word)
{
    return u16At(d, 1434) == word && u16At(d, 1430) == word;
}

P2Client::Params loopbackParams()
{
    P2Client::Params p;
    p.host = QStringLiteral("127.0.0.1");
    return p;
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("anan-front-end-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    QUdpSocket listener;
    if (!listener.bind(QHostAddress::LocalHost, kHighPriorityPort)) {
        // An environmental skip, not a pass; 77 is SKIP_RETURN_CODE in tests.cmake.
        std::fprintf(stderr, "anan_front_end_test: SKIP -- cannot bind 127.0.0.1:%u (%s)\n",
                     static_cast<unsigned>(kHighPriorityPort),
                     qPrintable(listener.errorString()));
        return 77;
    }

    // ---- Every run=1 sender carries the options; the stop packet does not ----
    {
        P2Client client;
        P2Client::Params p = loopbackParams();
        p.bandFiltersEnabled = true;
        p.speakerAmpMuted = true;
        check(client.start(p), "the client starts against loopback");
        const QByteArray started = one(listener, "start() sends one High Priority packet");
        check(byteAt(started, 1400) == 0x02, "the start packet already mutes the amplifier");
        check(bothBanks(started, 0x1000),
              "and, with nothing tuned yet, selects Bypass on both banks");

        client.setDdc0FrequencyHz(14'100'000.0);
        const QByteArray tuned = one(listener, "a retune sends one packet");
        check(bothBanks(tuned, 0x0002), "20 m selects the 13 MHz filter on both banks");
        check(u16At(tuned, 1432) == 0x0100, "and bytes 1432-1433 carry ANT1 alone");
        check(byteAt(tuned, 1400) == 0x02, "and the mute stays");

        client.setDdc0FrequencyHz(28'400'000.0);
        check(bothBanks(one(listener, "a second retune sends one packet"), 0x0004),
              "retuning to 10 m moves both banks to the 20 MHz filter");

        P2ClientTestAccess::keepalive(client);
        const QByteArray kept = one(listener, "the keepalive sends one packet");
        check(bothBanks(kept, 0x0004) && byteAt(kept, 1400) == 0x02,
              "the keepalive carries the filter and the mute");

        client.setStepAttenuationDb(0, 5);
        const QByteArray attenuated = one(listener, "an attenuator change sends one packet");
        check(bothBanks(attenuated, 0x0004) && byteAt(attenuated, 1400) == 0x02,
              "the attenuator's packet carries the filter and the mute");

        client.setBandFiltersEnabled(false);
        check(bothBanks(one(listener, "turning the bank off sends one packet"), 0x1000),
              "and the bypass flags decide again: both banks bypassed");
        client.setSpeakerAmpMuted(false);
        check(byteAt(one(listener, "unmuting sends one packet"), 1400) == 0,
              "unmuting clears byte 1400");

        client.setBandFiltersEnabled(true);
        client.setSpeakerAmpMuted(true);
        check(collect(listener, 2).size() == 2, "each live setter sends its own packet");
        client.stop();
        const QByteArray stopped = one(listener, "stop() sends one packet");
        check(byteAt(stopped, 4) == 0, "the stop packet is run=0");
        check(byteAt(stopped, 1400) == 0 && bothBanks(stopped, 0x1000),
              "and hands the radio back unmuted and bypassed, whatever was live");
    }

    // ---- The selection replaces the bypass flags ----
    {
        P2Client client;
        P2Client::Params p = loopbackParams();
        p.bypassAdc0Filters = false;
        p.bypassAdc1Filters = false;
        p.bandFiltersEnabled = true;
        client.start(p);
        collect(listener, 1);
        client.setDdc0FrequencyHz(7'100'000.0);
        check(bothBanks(one(listener, "a retune sends one packet"), 0x0010),
              "40 m selects the 9.5 MHz filter on both banks with both bypass flags off");
        client.setBandFiltersEnabled(false);
        check(bothBanks(one(listener, "turning the bank off sends one packet"), 0x0000),
              "and with the bank off the cleared flags decide");
        client.stop();
        collect(listener, 1);
    }

    // ---- A new session starts untuned, not on the last session's band ----
    {
        P2Client client;
        P2Client::Params p = loopbackParams();
        p.bandFiltersEnabled = true;
        client.start(p);
        collect(listener, 1);
        client.setDdc0FrequencyHz(14'100'000.0);
        collect(listener, 1);
        client.stop();
        collect(listener, 1);

        client.start(p);
        check(bothBanks(one(listener, "the next start() sends one packet"), 0x1000),
              "the next session's first packet bypasses rather than reselecting 20 m");
        client.stop();
        collect(listener, 1);

        P2Client::Params off = loopbackParams();
        client.start(off);
        const QByteArray fresh = one(listener, "a start() with the options off sends one packet");
        check(bothBanks(fresh, 0x1000) && byteAt(fresh, 1400) == 0,
              "and a session started with both options off carries neither");
        client.stop();
        collect(listener, 1);
    }

    // ---- The "anan"/"frontend.set" verb ----
    {
        AnanBackend backend;
        P2Client* client = AnanFrontEndTestAccess::client(backend);
        // The client lives on the backend's I/O thread; this returns once the
        // calls queued before it have run there.
        const auto flush = [client] {
            check(QMetaObject::invokeMethod(client, [] {}, Qt::BlockingQueuedConnection),
                  "the client's thread runs the calls queued to it");
        };
        QSignalSpy results(&backend, &IRadioBackend::extensionResult);
        QSignalSpy errors(&backend, &IRadioBackend::extensionError);

        check(backend.capabilities().extensionNamespaces.contains(QStringLiteral("anan")),
              "the backend declares the anan namespace the page gates on");

        backend.invokeExtension(QStringLiteral("anan"), QStringLiteral("frontend.set"), 11,
                                QVariantMap{{QStringLiteral("bandFiltersEnabled"), true}});
        flush();
        check(results.size() == 1 && results.at(0).at(0).toULongLong() == 11
                  && results.at(0).at(1).toMap().value(QStringLiteral("ok")).toBool(),
              "frontend.set answers its request id with ok");
        check(P2ClientTestAccess::options(*client).adc0Filter.has_value(),
              "bandFiltersEnabled reaches the client");
        check(!P2ClientTestAccess::options(*client).muteSpeakerAmp,
              "and leaves the mute alone");

        backend.invokeExtension(QStringLiteral("anan"), QStringLiteral("frontend.set"), 12,
                                QVariantMap{{QStringLiteral("speakerAmpMuted"), true}});
        flush();
        check(P2ClientTestAccess::options(*client).muteSpeakerAmp,
              "speakerAmpMuted reaches the client");
        check(P2ClientTestAccess::options(*client).adc0Filter.has_value(),
              "and a partial call leaves the band filters on");

        backend.invokeExtension(QStringLiteral("anan"), QStringLiteral("frontend.set"), 0,
                                QVariantMap{{QStringLiteral("bandFiltersEnabled"), false}});
        flush();
        check(results.size() == 2, "request id 0 gets no reply");
        check(!P2ClientTestAccess::options(*client).adc0Filter.has_value()
                  && P2ClientTestAccess::options(*client).muteSpeakerAmp,
              "bandFiltersEnabled false reaches the client, and the mute stays");
        check(!AnanFrontEndTestAccess::pending(backend).bandFiltersEnabled
                  && AnanFrontEndTestAccess::pending(backend).speakerAmpMuted,
              "a session restart resends what the verb last set");
        check(errors.isEmpty(), "frontend.set raises no error");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "anan_front_end_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
