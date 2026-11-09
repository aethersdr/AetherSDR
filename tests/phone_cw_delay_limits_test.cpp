#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/PhoneCwApplet.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QLineEdit>
#include <QSlider>

#include <cstdlib>
#include <iostream>
#include <variant>

using namespace AetherSDR;
namespace {
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("phone-cw-delay-limits"));
    require(settings.isValid(), "isolated test settings");
    QApplication application(argc, argv);
    AppSettings::instance().load();
    TransmitModel model;
    TransmitDelta previousRadio;
    previousRadio.cwDelay = 1500;
    model.applyChanges(previousRadio);
    PhoneCwApplet applet;
    applet.setTransmitModel(&model);
    QSlider* delay = nullptr;
    QLineEdit* editor = nullptr;
    for (QSlider* slider : applet.findChildren<QSlider*>()) {
        if (slider->accessibleName() == "CW delay") { delay = slider; }
    }
    for (QLineEdit* edit : applet.findChildren<QLineEdit*>()) {
        if (edit->accessibleName() == "CW delay value") { editor = edit; }
    }
    require(delay && editor, "existing CW delay controls are reachable");
    int requests = 0;
    int requestedMs = -1;
    QObject::connect(&model, &TransmitModel::controlRequested, &application,
        [&](const TransmitControlRequest& request) {
            if (request.field == TransmitControlRequest::Field::CwDelay) {
                ++requests;
                requestedMs = std::get<int>(request.value);
            }
        });
    applet.setCwControlLimits(5, 60, 300, 1100, 5, 1000, 50);
    require(delay->maximum() == 1000 && delay->singleStep() == 50
        && editor->accessibleDescription().contains("1000")
        && editor->accessibleDescription().contains("50"), "native delay limits reach both controls and accessibility");
    require(requests == 0 && model.cwDelay() == 1500,
        "narrowing a previous radio's delay range cannot write or replace engine state");
    delay->setValue(525);
    require(requests == 1 && requestedMs == 550 && delay->value() == 550
        && editor->text() == "550", "operator drag snaps once to the native 50 ms grid");
    TransmitDelta report;
    report.cwDelay = 750;
    model.applyChanges(report);
    require(requests == 1 && delay->value() == 750 && editor->text() == "750",
        "radio keyer-delay updates do not feed back as new requests");
    editor->setText(QStringLiteral("975"));
    emit editor->editingFinished();
    require(requests == 2 && requestedMs == 1000 && editor->text() == "1000",
        "explicit text commit uses the same native step as the slider");
    emit editor->editingFinished();
    require(requests == 2, "unchanged editor focus loss cannot duplicate a radio write");
    applet.setCwControlLimits(5, 100, 100, 6000, 10);
    require(requests == 2 && delay->maximum() == 2000 && delay->singleStep() == 10,
        "absent native delay record restores existing desktop limits without a write");
    delay->setValue(527);
    require(requests == 3 && requestedMs == 527,
        "other radio families retain continuous millisecond requests");
    return 0;
}
