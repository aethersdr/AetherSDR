#pragma once

#include <QString>

#include <memory>

namespace AetherSDR {
class RadioModel;

namespace control {

class LocalControlServer;
class RadioConnectionTarget;
class RadioResourceAdapter;
class ReceiveControlTarget;
class SliceFrequencyTarget;

// The desktop app serving aetherd's local v1 control endpoint for its own radio
// (docs/aetherd-app-control-socket.md). An on-radio panel daemon such as the
// ANAN G2's g2paneld then drives whichever program holds the radio, and only
// one program ever controls the radio.
//
// What a client gets is narrower than aetherd's --control:
// - The receive verbs (frequency, mode, filter, audio, AGC threshold, NB, NR,
//   RIT, pan center, bandwidth and RF gain, the radio's speaker) on the radio
//   the app is connected to, through the same model targets aetherd binds.
// - No radio.connect or radio.disconnect: the app's own connection UI owns the
//   connection. The connection target only observes it, so no client can take,
//   drop or reconnect the app's radio, and destroying the host leaves the
//   connection as it is.
// - No transmit verbs and no radio catalogue.
//
// The endpoint is the current user's only (LocalControlServer's private
// runtime-directory policy). The model must outlive the host; both live on the
// GUI thread.
class AppControlHost final {
public:
    // Listens on the logical name (as aetherd's --socket). Returns nullptr and
    // sets *error when the endpoint cannot be claimed, for example when an
    // aetherd is already serving that name.
    [[nodiscard]] static std::unique_ptr<AppControlHost> start(
        RadioModel* radio, const QString& socketName, QString* error = nullptr);

    ~AppControlHost();
    AppControlHost(const AppControlHost&) = delete;
    AppControlHost& operator=(const AppControlHost&) = delete;

    // The endpoint path clients connect to.
    [[nodiscard]] QString endpoint() const;

private:
    AppControlHost();

    // Declared in aetherd's order: the server first, so it is destroyed last,
    // after every target it holds a pointer to.
    std::unique_ptr<LocalControlServer> m_server;
    std::unique_ptr<RadioConnectionTarget> m_connection;
    std::unique_ptr<SliceFrequencyTarget> m_frequency;
    std::unique_ptr<ReceiveControlTarget> m_receive;
    std::unique_ptr<RadioResourceAdapter> m_resources;
};

} // namespace control
} // namespace AetherSDR
