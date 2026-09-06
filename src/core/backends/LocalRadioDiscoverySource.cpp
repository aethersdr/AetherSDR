#include "core/backends/LocalRadioDiscoveryMapping.h"

#include "core/RadioDiscovery.h"
#include "core/RtlSdrDiscovery.h"
#include "core/backends/anan/AnanDiscovery.h"
#include "core/backends/hl2/Hl2Discovery.h"
#include "core/backends/sim/SimBackend.h"

namespace AetherSDR {
namespace {

class LocalRadioDiscoverySource final : public RadioDiscoverySource {
public:
    explicit LocalRadioDiscoverySource(LocalDiscoveryOptions options) : m_options(options) {}
    ~LocalRadioDiscoverySource() override { stop(); }

    QStringList enabledSources() const override
    {
        QStringList sources;
        if (m_options.local) {
            sources = {QStringLiteral("flex"), QStringLiteral("hl2"), QStringLiteral("anan")};
            if (RtlSdrDiscovery::isAvailable()) {
                sources.append(QStringLiteral("rtl"));
            }
        }
        if (m_options.simulator) {
            sources.append(QStringLiteral("sim"));
        }
        sources.sort();
        return sources;
    }

    void start() override
    {
        if (m_started) {
            return;
        }
        m_started = true;
        m_running = true;
        if (m_options.local) {
            m_flex = std::make_unique<RadioDiscovery>();
            m_hl2 = std::make_unique<hl2::Hl2Discovery>();
            m_anan = std::make_unique<anan::AnanDiscovery>();
            wire(m_flex.get(), QStringLiteral("flex"), QStringLiteral("lan"));
            wire(m_hl2.get(), QStringLiteral("hl2"), QStringLiteral("lan"));
            wire(m_anan.get(), QStringLiteral("anan"), QStringLiteral("lan"));
            m_flex->startListening();
            m_hl2->start();
            m_anan->start();
            if (RtlSdrDiscovery::isAvailable()) {
                m_rtl = std::make_unique<RtlSdrDiscovery>();
                wire(m_rtl.get(), QStringLiteral("rtl"), QStringLiteral("usb"));
                m_rtl->start();
            }
        }
        if (m_options.simulator) {
            DiscoveredRadio demo;
            demo.family = SimBackend::familyName();
            demo.serial = SimBackend::demoSerial();
            demo.name = SimBackend::demoModelName();
            demo.model = SimBackend::demoModelName();
            demo.transport = QStringLiteral("sim");
            emit radioChanged(demo);
        }
    }

    void stop() override
    {
        m_started = true; // A stopped source is terminal, even before start.
        m_running = false;
        if (m_flex) { m_flex->stopListening(); }
        if (m_hl2) { m_hl2->stop(); }
        if (m_anan) { m_anan->stop(); }
        if (m_rtl) { m_rtl->stop(); }
    }

private:
    template<typename Source>
    void wire(Source* source, const QString& family, const QString& transport)
    {
        const auto changed = [this, family, transport](const RadioInfo& info) {
            if (!m_running) {
                return;
            }
            emit radioChanged(discovery::normalize(info, family, transport));
        };
        connect(source, &Source::radioDiscovered, this, changed);
        connect(source, &Source::radioUpdated, this, changed);
        connect(source, &Source::radioLost, this, [this, family](const QString& serial) {
            if (m_running) {
                emit radioLost(family, serial);
            }
        });
    }

    const LocalDiscoveryOptions m_options;
    bool m_started{false};
    bool m_running{false};
    std::unique_ptr<RadioDiscovery> m_flex;
    std::unique_ptr<hl2::Hl2Discovery> m_hl2;
    std::unique_ptr<anan::AnanDiscovery> m_anan;
    std::unique_ptr<RtlSdrDiscovery> m_rtl;
};

} // namespace

std::unique_ptr<RadioDiscoverySource> makeLocalRadioDiscoverySource(LocalDiscoveryOptions options)
{
    return std::make_unique<LocalRadioDiscoverySource>(options);
}

} // namespace AetherSDR
