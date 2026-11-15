#pragma once

#include <QCommandLineParser>

namespace AetherSDR::aetherd {

void addGrantAdminOptions(QCommandLineParser& parser);
// One explicit operator action against our current-user server. Loads only
// the chosen grant-admin credential from the OS vault; never accepts a secret
// in argv/stdin/settings, never opens a radio, and never sends key-on.
int runGrantAdmin(const QCommandLineParser& parser);

} // namespace AetherSDR::aetherd
