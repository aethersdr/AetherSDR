// "Can this radio send CW text?" (#7146 part 1): the hostCwKeyingControl record
// and RadioModel::hasHostCwTextKeyer() / hasCwTextKeyer(). Pins what every
// backend declares and that no text-CW surface has moved to hasCwTextKeyer() yet
// (rigctld reaches the host keyer through hasHostCwTextKeyer(), #7106).
// Socket-free: real backends are only constructed and asked for capabilities();
// the model gets an injected stub backend. Nothing is opened or keyed.

#include "TestSettingsProfile.h"
#include "core/SmartCatProtocol.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/anan/AnanBackend.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/icom/IcomCivBackend.h"
#include "core/backends/sim/SimBackend.h"
#ifdef AETHER_BACKEND_RTL
#include "core/backends/rtl/RtlSdrBackend.h"
#endif
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QString>

#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void check(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

class CapsStub final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected{false};
    int carrierEdges{0};
    int textSends{0};

    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void setCwKeying(bool, bool, int, const TxCoordinator::Operation&,
                     const TxCoordinator::Completion&) override
    {
        ++carrierEdges;
    }
    QString sendCwText(const QString&, const TxCoordinator::Operation&,
                       const TxCoordinator::Completion&) override
    {
        ++textSends;
        return {};
    }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

// A connected HL2 that granted TX declares canTransmit; the rest is the real
// backend's declaration.
RadioCapabilities hl2Capabilities(bool canTransmit)
{
    hl2::Hl2Backend real;
    RadioCapabilities caps = real.capabilities();
    caps.canTransmit = canTransmit;
    return caps;
}

struct Fixture {
    RadioModel radio;
    CapsStub* backend{nullptr};

    Fixture(const RadioCapabilities& caps, const QString& family, bool connected = true)
    {
        auto owned = std::make_unique<CapsStub>();
        backend = owned.get();
        backend->caps = caps;
        radio.setBackendForTest(std::move(owned), family);
        SliceDelta slice;
        slice.frequency = 7.020;
        slice.mode = QStringLiteral("CW");
        slice.panId = QStringLiteral("0x40000000");
        slice.txSlice = true;
        radio.emitBackendSliceChangedForTest(0, slice);
        if (!radio.slice(0)) {
            std::fprintf(stderr, "FATAL: the slice was not materialised\n");
            std::exit(2);
        }
        backend->connected = connected;
    }
};

void testEveryBackendDeclares()
{
    check("HL2 engages hostCwKeyingControl",
          hl2::Hl2Backend{}.capabilities().hostCwKeyingControl.has_value());
    check("HL2 still has no radio-side text buffer",
          !hl2::Hl2Backend{}.capabilities().hasRadioSideCwKeyer);
    check("Flex declares hostCwKeyingControl absent",
          !FlexBackend{}.capabilities().hostCwKeyingControl.has_value());
    check("Icom declares hostCwKeyingControl absent",
          !icom::IcomCivBackend{}.capabilities().hostCwKeyingControl.has_value());
    check("ANAN declares hostCwKeyingControl absent",
          !anan::AnanBackend{}.capabilities().hostCwKeyingControl.has_value());
    check("Sim declares hostCwKeyingControl absent",
          !SimBackend{}.capabilities().hostCwKeyingControl.has_value());
#ifdef AETHER_BACKEND_RTL
    check("RTL declares hostCwKeyingControl absent",
          !rtl::RtlSdrBackend{}.capabilities().hostCwKeyingControl.has_value());
#endif
    check("the struct default is absent", !RadioCapabilities{}.hostCwKeyingControl.has_value());
}

void testHostKeyedRadio()
{
    Fixture f(hl2Capabilities(true), QStringLiteral("hl2"));
    check("HL2, connected, TX: hasRadioSideCwKeyer() is false", !f.radio.hasRadioSideCwKeyer());
    check("HL2, connected, TX: hasHostCwTextKeyer() is true", f.radio.hasHostCwTextKeyer());
    check("HL2, connected, TX: hasCwTextKeyer() is true", f.radio.hasCwTextKeyer());
}

void testHostKeyedRadioWithoutTransmit()
{
    Fixture f(hl2Capabilities(false), QStringLiteral("hl2"));
    check("HL2 without TX: hasHostCwTextKeyer() is false", !f.radio.hasHostCwTextKeyer());
    check("HL2 without TX: hasCwTextKeyer() is false", !f.radio.hasCwTextKeyer());
}

void testDisconnected()
{
    Fixture f(hl2Capabilities(true), QStringLiteral("hl2"), /*connected=*/false);
    check("disconnected: hasHostCwTextKeyer() is false", !f.radio.hasHostCwTextKeyer());
    check("disconnected: hasCwTextKeyer() keeps the permissive rule (true)", f.radio.hasCwTextKeyer());
    check("disconnected: and agrees with hasRadioSideCwKeyer()",
          f.radio.hasCwTextKeyer() == f.radio.hasRadioSideCwKeyer());
}

void testRadioSideBufferWins()
{
    // A radio with its own text buffer keeps text there, even if it also
    // accepted host-timed elements.
    RadioCapabilities caps;
    caps.canTransmit = true;
    caps.hasRadioSideCwKeyer = true;
    caps.hostCwKeyingControl = RadioCapabilities::HostCwKeyingControl{};
    Fixture f(caps, QStringLiteral("test"));
    check("radio-side buffer: hasHostCwTextKeyer() is false", !f.radio.hasHostCwTextKeyer());
    check("radio-side buffer: hasCwTextKeyer() is true", f.radio.hasCwTextKeyer());
}

void testNoRecordNoText()
{
    RadioCapabilities caps;
    caps.canTransmit = true;
    Fixture f(caps, QStringLiteral("test"));
    check("no record, no buffer: hasHostCwTextKeyer() is false", !f.radio.hasHostCwTextKeyer());
    check("no record, no buffer: hasCwTextKeyer() is false", !f.radio.hasCwTextKeyer());
}

// Part 1 moves no caller to hasCwTextKeyer(). rigctld send_morse reaches the
// host keyer through hasHostCwTextKeyer() since #7106 (rigctl_host_cw_text_test);
// SmartCAT KY still answers that the radio has no text keyer.
void testNoSurfaceMovedYet()
{
    Fixture f(hl2Capabilities(true), QStringLiteral("hl2"));
    SmartCatProtocol smartCat(&f.radio);
    check("SmartCAT KY set still answers ?;", smartCat.processCommand(QStringLiteral("KY TEST")) == QLatin1String("?;"));
    check("SmartCAT KY query still answers ?;", smartCat.processCommand(QStringLiteral("KY")) == QLatin1String("?;"));
    QCoreApplication::processEvents();
    check("no carrier edge and no text reached the backend",
          f.backend->carrierEdges == 0 && f.backend->textSends == 0);
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("cw-text-capability"));
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) {
        return 1;
    }
    testEveryBackendDeclares();
    testHostKeyedRadio();
    testHostKeyedRadioWithoutTransmit();
    testDisconnected();
    testRadioSideBufferWins();
    testNoRecordNoText();
    testNoSurfaceMovedYet();
    std::printf("%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
