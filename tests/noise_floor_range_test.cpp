// SmartSDR 4.3 Noise Floor Range (#6656): validation, Flex capability, encode,
// pan status decode, RadioModel admission and the Display-menu control.
// Recording command sinks and injected status only: no sockets, radio or TX.
#include "TestSettingsProfile.h"
#include "RadioSetupControlFixture.h"
#include "gui/SpectrumOverlayMenu.h"
#include "models/PanadapterModel.h"

#include <QApplication>
#include <QLabel>
#include <QLocale>
#include <QSignalSpy>
#include <QSlider>
#include <QWidget>

#include <cstdio>
#include <limits>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool ok, const char* claim)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", claim);
    failures += !ok;
}

const QString kPan = QStringLiteral("0x40000000");

PanControlRequest range(double db) { return {PanControlField::NoiseFloorRange, db}; }

void license(RadioModel& radio, const QString& enabled)
{
    using Kvs = QMap<QString, QString>;
    const Kvs kvs{{QStringLiteral("name"), QStringLiteral("noise_floor")},
                  {QStringLiteral("enabled"), enabled},
                  {QStringLiteral("reason"), QStringLiteral("PLUS")}};
    QMetaObject::invokeMethod(&radio, "onStatusReceived", Qt::DirectConnection,
                              Q_ARG(QString, QStringLiteral("license feature")), Q_ARG(Kvs, kvs));
}

void reportRange(PanadapterModel& pan, const QString& value)
{
    pan.applyStateExtension({{QStringLiteral("noise_floor_range"), value}});
}

void requestValidation()
{
    for (double db : {0.0, 0.5, 1.5, 9.5, 10.0}) {
        check(range(db).valid(), "0..10 dB on the half-dB grid is valid");
    }
    for (double db : {-0.5, 10.5, 0.3, 1.25, std::numeric_limits<double>::quiet_NaN(),
                      std::numeric_limits<double>::infinity()}) {
        check(!range(db).valid(), "out of range, off grid or non-finite is refused");
    }
    check(!PanControlRequest{PanControlField::NoiseFloorRange, true}.valid()
              && !PanControlRequest{PanControlField::NoiseFloorRange, QStringLiteral("1.5")}.valid(),
          "a bool or text value is refused");
    check(!PanControlRequest{PanControlField::BandZoom, 1.0}.valid(),
          "a double does not satisfy the zoom fields");
}

void flexDeclarationAndEncoding()
{
    FlexBackend flex;
    QString model = QStringLiteral("FLEX-8600");
    QString firmware;
    QStringList commands;
    flex.setModelProvider([&] { return model; });
    flex.setFirmwareVersionProvider([&] { return firmware; });
    flex.setSetupCommandSink([&](const QString& command, const RadioSetupCompletion&) {
        commands.append(command);
        return true;
    });
    for (const QString& version : {QString(), QStringLiteral("4.2.20.41343"),
                                   QStringLiteral("4.3.15"), QStringLiteral("4.3.16beta")}) {
        firmware = version;
        check(!flex.capabilities().noiseFloorRangeControl,
              "absent, older and malformed firmware declare no Noise Floor Range");
    }
    firmware = QStringLiteral("4.3.16.42034");
    const auto control = flex.capabilities().noiseFloorRangeControl;
    check(control && control->requiredLicenseFeature == QLatin1String("noise_floor"),
          "FLEX-8600 on 4.3.16 declares it under the noise_floor license");
    model = QStringLiteral("AU-520M");
    check(flex.capabilities().noiseFloorRangeControl.has_value(), "Aurora declares it");
    for (const QString& other : {QStringLiteral("FLEX-6700"), QStringLiteral("FLEX-6600M"),
                                 QStringLiteral("RT-2122"), QString()}) {
        model = other;
        check(!flex.capabilities().noiseFloorRangeControl,
              "6000 series, RT-2122 and unknown models declare no Noise Floor Range");
    }

    const QLocale previous;
    QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
    flex.requestPanControl(QStringLiteral("0x40000001"), range(2.5));
    flex.requestPanControl(QStringLiteral("0x40000001"), range(0.0));
    flex.requestPanControl(QStringLiteral("0x40000001"), range(10.0));
    QLocale::setDefault(previous);
    check(commands == QStringList{
              QStringLiteral("display pan set 0x40000001 noise_floor_range=2.5"),
              QStringLiteral("display pan set 0x40000001 noise_floor_range=0.0"),
              QStringLiteral("display pan set 0x40000001 noise_floor_range=10.0")},
          "FlexLib's f1 wire text with a '.' decimal under a comma locale");
    commands.clear();
    check(flex.requestPanControl(QStringLiteral("0x40000001"), range(10.5)) == ReceiveDispatch::Unsupported
              && flex.requestPanControl(QString(), range(1.5)) == ReceiveDispatch::Unsupported
              && commands.isEmpty(),
          "an invalid value or an empty pan id sends nothing");

    QVariantMap carried;
    QObject::connect(&flex, &IRadioBackend::extensionStatus, &flex,
                     [&](const QString& ns, const QString& name, const QVariantMap& fields) {
        if (ns == QLatin1String("flex") && name == QLatin1String("panState")) { carried = fields; }
    });
    flex.decodePanState(kPan, {{QStringLiteral("noise_floor_range"), QStringLiteral("3.5")}});
    check(carried.value(QStringLiteral("noise_floor_range")).toString() == QLatin1String("3.5")
              && carried.value(QStringLiteral("panId")).toString() == kPan,
          "pan status carries noise_floor_range to the model");
}

void panStatusDecode()
{
    PanadapterModel pan(kPan);
    QSignalSpy changed(&pan, &PanadapterModel::noiseFloorRangeChanged);
    check(!pan.noiseFloorRange(), "unknown until the radio reports it");
    reportRange(pan, QStringLiteral("2.5"));
    check(pan.noiseFloorRange() == 2.5 && changed.size() == 1, "a reported value is adopted");
    reportRange(pan, QStringLiteral("2.5"));
    check(changed.size() == 1, "an unchanged report does not re-emit");
    for (const QString& bad : {QStringLiteral("abc"), QStringLiteral("nan"),
                               QStringLiteral("inf"), QString()}) {
        reportRange(pan, bad);
    }
    check(pan.noiseFloorRange() == 2.5 && changed.size() == 1,
          "malformed and non-finite values keep the last good value");
}

void admission(RadioModel& radio, RadioSetupTestBackend* backend, PanadapterModel* pan)
{
    QSignalSpy availability(&radio, &RadioModel::panNoiseFloorRangeAvailabilityChanged);
    backend->setModelProvider([] { return QStringLiteral("FLEX-6700"); });
    check(radio.panNoiseFloorRangeUnavailableReason().contains(QLatin1String("FLEX-8000")),
          "a 6000-series radio is told why");
    check(radio.requestPanControl(pan, range(1.5)) == ReceiveDispatch::Unsupported
              && backend->commands.isEmpty(), "and sends nothing");

    backend->setModelProvider([] { return QStringLiteral("FLEX-8600"); });
    check(radio.panNoiseFloorRangeUnavailableReason().contains(QLatin1String("Waiting")),
          "an unconfirmed license waits");
    check(radio.requestPanControl(pan, range(1.5)) == ReceiveDispatch::Unsupported
              && backend->commands.isEmpty(), "and sends nothing");

    license(radio, QStringLiteral("0"));
    check(radio.panNoiseFloorRangeUnavailableReason().contains(QLatin1String("SmartSDR+")),
          "a disabled license names SmartSDR+");
    check(radio.requestPanControl(pan, range(1.5)) == ReceiveDispatch::Unsupported
              && backend->commands.isEmpty(), "and sends nothing");

    license(radio, QStringLiteral("1"));
    check(radio.panNoiseFloorRangeUnavailableReason().isEmpty(), "an enabled license admits it");
    check(availability.size() >= 2, "license changes announce availability");
    check(radio.requestPanControl(pan, range(4.0)) == ReceiveDispatch::Dispatched
              && backend->commands == QStringList{
                     QStringLiteral("display pan set 0x40000000 noise_floor_range=4.0")},
          "an admitted request reaches the radio");
    backend->commands.clear();

    PanadapterModel stray(kPan);
    check(radio.requestPanControl(&stray, range(4.0)) == ReceiveDispatch::Unsupported
              && radio.requestPanControl(pan, range(4.2)) == ReceiveDispatch::Unsupported
              && backend->commands.isEmpty(),
          "a pan the radio does not own, or an invalid value, sends nothing");
}

void displayMenu(RadioModel& radio, RadioSetupTestBackend* backend, PanadapterModel* pan)
{
    license(radio, QStringLiteral("0"));
    QWidget parent;
    SpectrumOverlayMenu menu(&parent);
    menu.setRadioModel(&radio);
    menu.setPanId(kPan);
    auto* slider = parent.findChild<QSlider*>(QStringLiteral("displayNoiseFloorRangeSlider"));
    check(slider, "the Display panel has the NF Range slider");
    if (!slider) { return; }
    check(!slider->isEnabled()
              && slider->toolTip().contains(QLatin1String("SmartSDR+"))
              && slider->accessibleDescription() == slider->toolTip(),
          "unlicensed: dimmed, with the reason in the tooltip and accessible description");

    license(radio, QStringLiteral("1"));
    check(!slider->isEnabled() && slider->toolTip().contains(QLatin1String("Waiting")),
          "licensed but unreported: still dimmed, waiting for the radio");

    reportRange(*pan, QStringLiteral("2.5"));
    check(slider->isEnabled() && slider->value() == 5
              && slider->accessibleDescription() == slider->toolTip()
              && !slider->toolTip().contains(QLatin1String("Waiting")),
          "reported: enabled at the radio's value with a description");
    check(backend->commands.isEmpty(), "following status sends nothing");

    slider->triggerAction(QAbstractSlider::SliderSingleStepAdd);
    check(backend->commands == QStringList{
              QStringLiteral("display pan set 0x40000000 noise_floor_range=3.0")},
          "a step requests the next half dB");
    check(pan->noiseFloorRange() == 2.5, "the model still holds the radio's value");
    backend->commands.clear();

    reportRange(*pan, QStringLiteral("3.0"));
    check(slider->value() == 6 && backend->commands.isEmpty(),
          "the radio's confirmation moves the slider without an echo");

    license(radio, QStringLiteral("0"));
    check(!slider->isEnabled(), "losing the license dims it again");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("noise-floor-range-test"));
    QApplication app(argc, argv);
    requestValidation();
    flexDeclarationAndEncoding();
    panStatusDecode();
    {
        RadioModel radio;
        RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
        backend->setFirmwareVersionProvider([] { return QStringLiteral("4.3.16.42034"); });
        PanadapterModel* pan = RadioSetupControlTestAccess::addPan(radio, kPan);
        admission(radio, backend, pan);
        displayMenu(radio, backend, pan);
    }
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
