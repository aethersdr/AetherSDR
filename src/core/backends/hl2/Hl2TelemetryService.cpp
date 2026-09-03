#include "core/backends/hl2/Hl2TelemetryService.h"

#include "core/backends/hl2/Hl2TelemetryPoller.h"

#include <QTimer>

namespace AetherSDR::hl2 {

struct Hl2TelemetryService::Impl {
    Hl2TelemetryPoller* poller = nullptr;
    QTimer* stateTimer = nullptr;
    Hl2LinkState state = Hl2LinkState::NotConnected;
    std::optional<DiscoveryReply> reply;
    QElapsedTimer at;
    int unanswered = 0;
    QElapsedTimer demand;
};

// DELIBERATELY UNIMPLEMENTED, for one commit only.
//
// This stub reproduces TODAY's behaviour exactly — an empty health snapshot in
// the disconnected state — so the test that comes with it compiles and fails
// for the real reason rather than failing to build. The next commit implements
// it and the test goes green. See the test's header comment.
Hl2TelemetryService::Hl2TelemetryService(QObject* parent)
    : QObject(parent)
{
}

Hl2TelemetryService::~Hl2TelemetryService() = default;

void Hl2TelemetryService::setTarget(const QHostAddress&) {}
void Hl2TelemetryService::setExpectedMac(const std::array<std::uint8_t, 6>&) {}
void Hl2TelemetryService::setLinkState(Hl2LinkState) {}
void Hl2TelemetryService::noteDemand() {}

IRadioBackend::HealthSnapshot Hl2TelemetryService::healthRows() const
{
    return {};   // exactly what a disconnected app reports today
}

std::optional<DiscoveryReply> Hl2TelemetryService::lastReply() const
{
    return std::nullopt;
}

}  // namespace AetherSDR::hl2
