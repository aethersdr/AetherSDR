// MON and VOX say why they are unavailable on a radio that declares no support
// (RadioCapabilities::txMonitorControl / voxControl empty, as on the
// Hermes-Lite 2), instead of looking live and doing nothing.
//
// Pinned, with the same predicate MainWindow applies
// (`!connected || caps.<control>.has_value()`):
//   * HL2-like caps: MON, its slider, VOX and its slider are disabled, carry
//     the reason in accessibleDescription and tooltip, and a button lit before
//     is not left lit;
//   * caps that support them: enabled, original description restored;
//   * no radio connected: enabled (the permissive state).

#include "TestSettingsProfile.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/TransmitDelta.h"
#include "gui/PhoneApplet.h"
#include "gui/PhoneCwApplet.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QPushButton>
#include <QSlider>
#include <QSignalSpy>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

template <typename W>
static W* findByName(QWidget& parent, const QString& name)
{
    for (W* w : parent.findChildren<W*>()) {
        if (w->accessibleName() == name) {
            return w;
        }
    }
    return nullptr;
}

static void apply(PhoneApplet& phone, PhoneCwApplet& cw, bool connected,
                  const RadioCapabilities& caps)
{
    phone.setVoxAvailable(!connected || caps.voxControl.has_value());
    cw.setMonitorAvailable(!connected || caps.txMonitorControl.has_value());
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("phone-mon-vox-availability-test"));
    QApplication app(argc, argv);

    TransmitModel model;
    PhoneApplet phone;
    PhoneCwApplet cw;
    phone.setTransmitModel(&model);
    cw.setTransmitModel(&model);
    auto* vox = findByName<QPushButton>(phone, QStringLiteral("VOX voice-operated transmit"));
    auto* voxLevel = findByName<QSlider>(phone, QStringLiteral("VOX level"));
    auto* mon = findByName<QPushButton>(cw, QStringLiteral("TX monitor"));
    auto* monLevel = findByName<QSlider>(cw, QStringLiteral("Monitor volume"));
    check(vox && voxLevel && mon && monLevel, "all four controls are discoverable by accessibleName");
    if (!(vox && voxLevel && mon && monLevel)) {
        return 1;
    }

    const QString voxDesc = vox->accessibleDescription();
    const QString voxLevelDesc = voxLevel->accessibleDescription();
    const QString monDesc = mon->accessibleDescription();
    const QString monLevelDesc = monLevel->accessibleDescription();
    check(vox->isEnabled() && voxLevel->isEnabled() && mon->isEnabled() && monLevel->isEnabled(),
          "precondition: all four are live before any radio says otherwise");

    RadioCapabilities hl2;           // HL2-like: neither declared
    hl2.voxControl = std::nullopt;
    hl2.txMonitorControl = std::nullopt;

    RadioCapabilities flex;          // a radio that supports both
    flex.voxControl = RadioCapabilities::VoxControl{/*hasDelay*/ true};
    flex.txMonitorControl = RadioCapabilities::TxMonitorControl{};

    TransmitDelta status;
    status.voxEnable = true;
    status.sbMonitor = true;
    status.voxLevel = 37;
    status.monGainSb = 63;
    model.applyChanges(status);
    check(vox->isChecked() && mon->isChecked(), "supporting radio: model status lights both controls");
    QSignalSpy commands(&model, &TransmitModel::controlRequested);

    apply(phone, cw, true, hl2);
    check(!vox->isEnabled() && !voxLevel->isEnabled(), "HL2: VOX and VOX level are disabled");
    check(!mon->isEnabled() && !monLevel->isEnabled(), "HL2: MON and monitor volume are disabled");
    check(!vox->isChecked() && !mon->isChecked(), "HL2: neither button is left lit");
    check(vox->accessibleDescription().startsWith(QLatin1String("Unavailable:"))
              && voxLevel->accessibleDescription().startsWith(QLatin1String("Unavailable:")),
          "HL2: VOX reason is announced through accessibleDescription");
    check(mon->accessibleDescription().startsWith(QLatin1String("Unavailable:"))
              && monLevel->accessibleDescription().startsWith(QLatin1String("Unavailable:")),
          "HL2: MON reason is announced through accessibleDescription");
    check(vox->toolTip() == vox->accessibleDescription()
              && mon->toolTip() == mon->accessibleDescription(),
          "HL2: tooltip and accessibleDescription carry the same reason");

    status.voxLevel = 38;
    status.monGainSb = 64;
    model.applyChanges(status);
    check(!vox->isChecked() && !mon->isChecked(),
          "unsupported radio: a model refresh cannot relight either button");
    check(model.voxEnable() && model.sbMonitor(), "availability does not alter model state");
    check(commands.isEmpty(), "availability and model refresh send no control requests");

    apply(phone, cw, true, flex);
    check(vox->isEnabled() && voxLevel->isEnabled() && mon->isEnabled() && monLevel->isEnabled(),
          "supporting radio: all four are enabled");
    check(vox->accessibleDescription() == voxDesc && voxLevel->accessibleDescription() == voxLevelDesc
              && mon->accessibleDescription() == monDesc && monLevel->accessibleDescription() == monLevelDesc,
          "supporting radio: original descriptions restored");
    check(vox->toolTip().isEmpty() && mon->toolTip().isEmpty(),
          "supporting radio: no leftover reason tooltip");
    check(vox->isChecked() && mon->isChecked(),
          "supporting radio: restoring availability immediately reflects current model state");
    check(voxLevel->value() == 38 && monLevel->value() == 64,
          "supporting radio: current model levels are preserved");
    check(commands.isEmpty(), "restoring availability sends no control requests");

    RadioCapabilities icom = flex;
    icom.voxControl = RadioCapabilities::VoxControl{/*hasDelay*/ false};
    apply(phone, cw, true, hl2);
    apply(phone, cw, true, icom);
    check(vox->isEnabled() && mon->isEnabled() && vox->isChecked() && mon->isChecked(),
          "Icom: VOX without delay and MON retain model state after switching radios");

    RadioCapabilities monitorOnly = flex;
    monitorOnly.voxControl = std::nullopt;
    apply(phone, cw, true, monitorOnly);
    check(!vox->isEnabled() && mon->isEnabled() && mon->isChecked(),
          "independent capabilities: missing VOX does not disable MON");
    RadioCapabilities voxOnly = flex;
    voxOnly.txMonitorControl = std::nullopt;
    apply(phone, cw, true, voxOnly);
    check(vox->isEnabled() && vox->isChecked() && !mon->isEnabled(),
          "independent capabilities: missing MON does not disable VOX");

    apply(phone, cw, true, hl2);
    apply(phone, cw, false, hl2);
    check(vox->isEnabled() && voxLevel->isEnabled() && mon->isEnabled() && monLevel->isEnabled(),
          "no radio connected: all four are enabled again");
    check(vox->isChecked() && mon->isChecked(), "disconnect restores the model presentation");
    check(commands.isEmpty(), "all capability transitions leave the command path untouched");

    std::printf("phone_mon_vox_availability_test: %s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
