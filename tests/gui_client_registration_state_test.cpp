#include "core/GuiClientRegistrationState.h"

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) {
        ++failures;
    }
}

} // namespace

int main()
{
    GuiClientRegistrationState state;
    check(state.phase() == GuiClientRegistrationState::Phase::Idle,
          "registration starts idle");

    state.begin();
    const GuiClientRegistrationState::Result accepted = state.complete(0, {});
    check(accepted.accepted(), "successful reply continues the connection handshake");
    check(state.phase() == GuiClientRegistrationState::Phase::Registered,
          "successful reply marks the GUI client registered");

    state.begin();
    state.noteRadioMessage(
        QStringLiteral("The maximum number of connected clients has been reached"),
        MessageSeverity::Fatal);
    const GuiClientRegistrationState::Result fullRadio =
        state.complete(static_cast<int>(0xF3000001U), {});
    check(!fullRadio.accepted(), "nonzero reply rejects GUI registration");
    check(!fullRadio.automaticRetryAllowed(),
          "rejected GUI registration waits for an explicit reconnect");
    check(state.phase() == GuiClientRegistrationState::Phase::Rejected,
          "rejected reply enters the terminal rejected state");
    check(fullRadio.detail
              == QStringLiteral("The maximum number of connected clients has been reached"),
          "fatal radio message supplies the rejection detail when the reply body is empty");

    state.begin();
    state.noteRadioMessage(QStringLiteral("older fatal detail"), MessageSeverity::Fatal);
    const GuiClientRegistrationState::Result bodyWins =
        state.complete(static_cast<int>(0xF3000001U),
                       QStringLiteral("reply-specific detail"));
    check(bodyWins.detail == QStringLiteral("reply-specific detail"),
          "command reply detail takes precedence over a preceding fatal message");

    state.begin();
    state.noteRadioMessage(QStringLiteral("routine notice"), MessageSeverity::Info);
    const GuiClientRegistrationState::Result noDetail =
        state.complete(static_cast<int>(0xF3000001U), {});
    check(noDetail.detail.isEmpty(),
          "non-error radio messages cannot become registration failure details");

    state.reset();
    check(state.phase() == GuiClientRegistrationState::Phase::Idle,
          "disconnect reset returns the state machine to idle");

    return failures == 0 ? 0 : 1;
}
