#pragma once

// A USB backend for tests that build a Ctr2ProxyModel, so none reaches
// hidapi and a real CTR2. inertUsb() is the backend every model is built
// with; a case that needs devices injects them with setUsbBackend()
// afterwards. installUsbTripwire() replaces the default backend (the
// parameterless constructor's) with one that counts its calls, and
// usbTripwireCalls() is what the test asserts is still zero at the end.
// installFirmwareSourceTripwire() does the same for FirmwareInstallModel's
// default device lists (serial ports in download mode, and hidapi with no
// relay): a model built with them that polls is counted in
// firmwareSourceTripwireCalls(). inertFirmwareSources() lists nothing.

#include "core/Ctr2HidPort.h"
#include "models/Ctr2ProxyModel.h"
#include "models/FirmwareInstallModel.h"

#include <QList>

namespace Ctr2ProxyTestBackend {

inline AetherSDR::Ctr2ProxyModel::UsbBackend inertUsb()
{
    AetherSDR::Ctr2ProxyModel::UsbBackend backend;
    backend.devices = [] { return QList<AetherSDR::Ctr2HidPort::DeviceInfo>(); };
    backend.opener = [](const AetherSDR::Ctr2HidPort::DeviceInfo&, QString*) {
        return static_cast<AetherSDR::Ctr2HidPort*>(nullptr);
    };
    return backend;
}

inline int& usbTripwireCalls()
{
    static int calls = 0;
    return calls;
}

inline void installUsbTripwire()
{
    AetherSDR::Ctr2ProxyModel::UsbBackend tripwire;
    tripwire.devices = [] {
        ++usbTripwireCalls();
        return QList<AetherSDR::Ctr2HidPort::DeviceInfo>();
    };
    tripwire.opener = [](const AetherSDR::Ctr2HidPort::DeviceInfo&, QString*) {
        ++usbTripwireCalls();
        return static_cast<AetherSDR::Ctr2HidPort*>(nullptr);
    };
    tripwire.firmwareOpener = [](const AetherSDR::Ctr2HidPort::DeviceInfo&, quint32, QString*) {
        ++usbTripwireCalls();
        return static_cast<AetherSDR::Ctr2HidPort*>(nullptr);
    };
    AetherSDR::Ctr2ProxyModel::setDefaultUsbBackendForTesting(std::move(tripwire));
}

inline AetherSDR::FirmwareInstallModel::DeviceSources inertFirmwareSources()
{
    return {[] { return QList<AetherSDR::FirmwareInstallModel::Loader>(); },
            [] { return QList<AetherSDR::Ctr2HidPort::DeviceInfo>(); }};
}

inline int& firmwareSourceTripwireCalls()
{
    static int calls = 0;
    return calls;
}

inline void installFirmwareSourceTripwire()
{
    AetherSDR::FirmwareInstallModel::setDefaultDeviceSourcesForTesting(
        {[] {
             ++firmwareSourceTripwireCalls();
             return QList<AetherSDR::FirmwareInstallModel::Loader>();
         },
         [] {
             ++firmwareSourceTripwireCalls();
             return QList<AetherSDR::Ctr2HidPort::DeviceInfo>();
         }});
}

} // namespace Ctr2ProxyTestBackend
