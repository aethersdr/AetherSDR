#pragma once
// Per-platform alias for the Ulanzi Dial backend manager.  Each
// concrete implementation exposes the same Qt signal contract
// (tuneSteps / buttonEvent / connectionChanged) and the same
// start/stop/isConnected/deviceName surface, so consumers (the
// mapper dialog and MainWindow's dispatcher) only need this alias
// and do not have to switch on platform themselves.

#include <QtGlobal>

#ifdef Q_OS_LINUX
#include "core/EvdevEncoderManager.h"
namespace AetherSDR { using UlanziDialBackend = EvdevEncoderManager; }
#elif defined(Q_OS_WIN)
#include "core/UlanziDialWindowsManager.h"
namespace AetherSDR { using UlanziDialBackend = UlanziDialWindowsManager; }
#elif defined(Q_OS_MAC)
#include "core/UlanziDialMacOSManager.h"
namespace AetherSDR { using UlanziDialBackend = UlanziDialMacOSManager; }
#else
namespace AetherSDR { class UlanziDialBackend; }  // unsupported platform: no backend
#endif
