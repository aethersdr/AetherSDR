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
#include "gui/PhoneApplet.h"
#include "gui/PhoneCwApplet.h"

#include <QApplication>
#include <QPushButton>
#include <QSlider>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

template <typename W>
static W* findByName(QWidget& parent, const QString& name)
{
    for (W* w : parent.findChildren<W*>()) {
        if (w->accessibleName() == name) return w;
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

    PhoneApplet phone;
    PhoneCwApplet cw;
    auto* vox = findByName<QPushButton>(phone, QStringLiteral("VOX voice-operated transmit"));
    auto* voxLevel = findByName<QSlider>(phone, QStringLiteral("VOX level"));
    auto* mon = findByName<QPushButton>(cw, QStringLiteral("TX monitor"));
    auto* monLevel = findByName<QSlider>(cw, QStringLiteral("Monitor volume"));
    check(vox && voxLevel && mon && monLevel, "all four controls are discoverable by accessibleName");
    if (!(vox && voxLevel && mon && monLevel)) return 1;

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

    // Light both first, as an operator on another radio would have.
    vox->setChecked(true);
    mon->setChecked(true);

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

    apply(phone, cw, true, flex);
    check(vox->isEnabled() && voxLevel->isEnabled() && mon->isEnabled() && monLevel->isEnabled(),
          "supporting radio: all four are enabled");
    check(vox->accessibleDescription() == voxDesc && voxLevel->accessibleDescription() == voxLevelDesc
              && mon->accessibleDescription() == monDesc && monLevel->accessibleDescription() == monLevelDesc,
          "supporting radio: original descriptions restored");
    check(vox->toolTip().isEmpty() && mon->toolTip().isEmpty(),
          "supporting radio: no leftover reason tooltip");

    apply(phone, cw, true, hl2);
    apply(phone, cw, false, hl2);
    check(vox->isEnabled() && voxLevel->isEnabled() && mon->isEnabled() && monLevel->isEnabled(),
          "no radio connected: all four are enabled again");

    std::printf("phone_mon_vox_availability_test: %s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
