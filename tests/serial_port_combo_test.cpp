#include "gui/SerialPortCombo.h"

#include <QApplication>
#include <QList>
#include <cstdio>

namespace {
struct Port {
    QString name;
    QString portName() const { return name; }
    QString description() const { return QStringLiteral("test adapter"); }
};

int failures = 0;
void check(const char* name, bool passed)
{
    std::printf("[%s] %s\n", passed ? "OK" : "FAIL", name);
    if (!passed) {
        ++failures;
    }
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    using namespace AetherSDR::SerialPortCombo;
    QComboBox combo;
    QLineEdit edit;
    QList<Port> ports{{"portA"}, {"portB"}};
    int enumerations = 0;
    auto enumerate = [&] {
        ++enumerations;
        return ports;
    };
    int selectionSignals = 0;
    int editSignals = 0;
    QObject::connect(&combo, &QComboBox::currentIndexChanged, &combo,
                     [&](int) { ++selectionSignals; });
    QObject::connect(&edit, &QLineEdit::textChanged, &edit,
                     [&](const QString&) { ++editSignals; });

    check("initial saved port", !populate(&combo, &edit, "portB", ports)
          && combo.currentData() == "portB");
    ports = {{"portB"}, {"portA"}};
    check("reordered named port", !refresh(&combo, &edit, enumerate)
          && combo.currentData() == "portB");
    ports = {{"portA"}};
    check("absent named port retains path", refresh(&combo, &edit, enumerate)
          && isCustom(&combo) && edit.text() == "portB");
    ports = {{"portA"}, {"portB"}};
    check("returning named port reselected", !refresh(&combo, &edit, enumerate)
          && combo.currentData() == "portB");
    check("automatic population emits no settings signals",
          selectionSignals == 0 && editSignals == 0);

    combo.setCurrentIndex(combo.findData("__custom__"));
    edit.clear();
    selectionSignals = 0;
    editSignals = 0;
    check("empty Custom survives populated enumeration",
          refresh(&combo, &edit, enumerate) && isCustom(&combo) && edit.text().isEmpty());
    ports.clear();
    check("empty Custom survives empty enumeration",
          refresh(&combo, &edit, enumerate) && isCustom(&combo) && edit.text().isEmpty());
    ports = {{"portC"}};
    check("empty Custom survives a newly arriving port",
          refresh(&combo, &edit, enumerate) && isCustom(&combo) && edit.text().isEmpty());
    check("empty Custom refresh emits no settings signals",
          selectionSignals == 0 && editSignals == 0);

    edit.setText("/dev/custom-symlink");
    check("custom path survives refresh", refresh(&combo, &edit, enumerate)
          && edit.text() == "/dev/custom-symlink");
    check("popup fixture starts closed", !combo.view()->isVisible());
    combo.showPopup();
    check("popup fixture opens", combo.view()->isVisible());
    const int beforePopup = enumerations;
    check("open popup retains Custom and skips enumeration",
          refresh(&combo, &edit, enumerate) && enumerations == beforePopup
          && edit.text() == "/dev/custom-symlink");
    combo.hidePopup();

    check("unconfigured population retains first-port default",
          !populate(&combo, &edit, QString(), ports) && combo.currentData() == "portC");
    ports.clear();
    check("unconfigured empty enumeration exposes Custom editor",
          populate(&combo, &edit, QString(), ports) && isCustom(&combo)
          && edit.text().isEmpty());
    check("missing combo does not enumerate",
          !refresh(nullptr, &edit, enumerate) && enumerations == beforePopup);
    return failures == 0 ? 0 : 1;
}
