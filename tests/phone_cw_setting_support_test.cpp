#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/TransmitDelta.h"
#include "core/backends/SliceDelta.h"
#include "gui/PhoneCwApplet.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QPushButton>
#include <QSlider>
#include <QLineEdit>

#include <cstdlib>
#include <iostream>
#include <memory>

using namespace AetherSDR;
namespace {
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
template<class T>
T* control(PhoneCwApplet& applet, const QString& name)
{
    for (T* widget : applet.findChildren<T*>()) {
        if (widget->accessibleName() == name) { return widget; }
    }
    return nullptr;
}
class SettingsBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected = true;
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};
}

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("phone-cw-setting-support"));
    require(settings.isValid(), "isolated test settings");
    QApplication application(argc, argv);
    AppSettings::instance().load();
    RadioModel radio;
    auto owned = std::make_unique<SettingsBackend>();
    SettingsBackend* source = owned.get();
    using Field = TransmitControlRequest::Field;
    source->caps.canTransmit = true;
    source->caps.radioTransmitSettings = RadioTransmitSettings{{Field::MicGain,
        Field::CwSpeed, Field::CwPitch, Field::CwDelay, Field::CwBreakIn}};
    radio.setBackendForTest(std::move(owned), QStringLiteral("injected"));
    radio.transmitModel().setRadioTransmitSettings(source->caps.radioTransmitSettings);
    PhoneCwApplet applet;
    applet.setTransmitModel(&radio.transmitModel());
    applet.setRadioModel(&radio);
    QSlider* speed = control<QSlider>(applet, QStringLiteral("CW speed"));
    QSlider* pitch = control<QSlider>(applet, QStringLiteral("CW pitch"));
    QPushButton* iambic = control<QPushButton>(applet, QStringLiteral("Iambic keyer"));
    QPushButton* sidetone = control<QPushButton>(applet, QStringLiteral("CW sidetone"));
    QPushButton* hold = control<QPushButton>(applet, QStringLiteral("Hold break-in delay"));
    require(speed && pitch && iambic && sidetone && hold, "P/CW native setting controls exist");
    require(speed->isEnabled() && pitch->isEnabled(), "implemented native controls stay available");
    require(!iambic->isEnabled() && !sidetone->isEnabled() && !hold->isEnabled()
        && !iambic->accessibleDescription().isEmpty() && !sidetone->accessibleDescription().isEmpty(),
        "unimplemented settings remain dimmed with accessible reasons");
    QSlider* gain = control<QSlider>(applet, QStringLiteral("Microphone gain"));
    require(gain, "mic gain control exists");
    const int confirmedGain = radio.transmitModel().micLevel();
    gain->setValue(75);
    QApplication::processEvents();
    require(radio.transmitModel().micLevel() == confirmedGain && gain->value() == confirmedGain,
        "a refused or pending request cannot remain painted as confirmed mic gain");
    applet.setTransmitControlsAvailable(false);
    require(!speed->isEnabled() && pitch->isEnabled() && !iambic->isEnabled(),
        "receive-only parent gating composes with field restrictions and keeps receive pitch");
    applet.setTransmitControlsAvailable(true);
    require(speed->isEnabled() && !iambic->isEnabled(),
        "restoring TX availability cannot revive an unsupported keyer setting");
    source->caps.radioTransmitSettings->fields.insert(Field::CwIambic);
    radio.transmitModel().setRadioTransmitSettings(source->caps.radioTransmitSettings);
    emit radio.capabilitiesChanged(true, source->caps);
    require(iambic->isEnabled() && !sidetone->isEnabled(), "mid-session support change lifts only its restriction");
    source->caps.radioTransmitSettings->fields.insert(Field::CwSidetone);
    radio.transmitModel().setRadioTransmitSettings(source->caps.radioTransmitSettings);
    emit radio.capabilitiesChanged(true, source->caps);
    TransmitDelta sidetoneReadback;
    sidetoneReadback.cwSidetone = false;
    radio.transmitModel().applyChanges(sidetoneReadback);
    int sidetoneRequests = 0;
    QObject::connect(&radio.transmitModel(), &TransmitModel::controlRequested, &application,
        [&](const TransmitControlRequest& request) { if (request.field == Field::CwSidetone) { ++sidetoneRequests; } });
    QSlider* sidetoneGain = control<QSlider>(applet, QStringLiteral("Sidetone volume"));
    require(sidetoneGain && !sidetoneGain->isEnabled() && !sidetoneGain->accessibleDescription().isEmpty(),
        "native switch support cannot enable an unmapped sidetone percent gain");
    require(sidetone->isEnabled() && !sidetone->isChecked(), "native sidetone switch becomes available from its declared field");
    sidetone->click();
    application.processEvents();
    require(sidetoneRequests == 1 && !sidetone->isChecked() && !radio.transmitModel().cwSidetone(),
        "pending sidetone enable reconciles to confirmed native state");
    sidetoneReadback.cwSidetone = true;
    radio.transmitModel().applyChanges(sidetoneReadback);
    require(sidetoneRequests == 1 && sidetone->isChecked(), "sidetone readback updates the switch without another radio request");
    applet.setTransmitControlsAvailable(false);
    source->caps.radioTransmitSettings.reset();
    radio.transmitModel().setRadioTransmitSettings(std::nullopt);
    emit radio.capabilitiesChanged(true, source->caps);
    require(!speed->isEnabled() && !iambic->isEnabled(),
        "withdrawing native restrictions does not undo receive-only gating");
    applet.setTransmitControlsAvailable(true);
    require(speed->isEnabled() && iambic->isEnabled() && sidetone->isEnabled() && hold->isEnabled(),
        "legacy backend restores its existing controls after a native session");
    SliceModel receiveSlice(0);
    SliceDelta apfReadback;
    apfReadback.mode = QStringLiteral("CW");
    apfReadback.apf = true;
    apfReadback.apfLevel = 37;
    receiveSlice.applyChanges(apfReadback);
    applet.setSlice(&receiveSlice);
    QPushButton* apf = applet.findChild<QPushButton*>(QStringLiteral("cwApfBtn"));
    QSlider* apfLevel = applet.findChild<QSlider*>(QStringLiteral("cwApfSlider"));
    QLineEdit* apfValue = control<QLineEdit>(applet, QStringLiteral("APF bandwidth value"));
    require(apf && apfLevel && apfValue, "APF controls exist");
    struct Presentation {
        QWidget* widget;
        QString tooltip;
        QString description;
        QString style;
    };
    const QList<Presentation> legacyPresentation{
        {apf, apf->toolTip(), apf->accessibleDescription(), apf->styleSheet()},
        {apfLevel, apfLevel->toolTip(), apfLevel->accessibleDescription(), apfLevel->styleSheet()},
        {apfValue, apfValue->toolTip(), apfValue->accessibleDescription(), apfValue->styleSheet()}};
    const auto requireLegacyPresentation = [&] {
        for (const Presentation& original : legacyPresentation) {
            require(original.widget->toolTip() == original.tooltip,
                "native APF tooltip must be released for the legacy backend");
            require(original.widget->accessibleDescription() == original.description,
                "native APF accessible description must be released for the legacy backend");
            require(original.widget->styleSheet() == original.style,
                "native APF foreground treatment must be released for the legacy backend");
        }
    };
    using Dsp = ReceiveDspControl::Feature;
    applet.setSlice(nullptr);
    source->caps.hasAudioPeakingFilter = true;
    source->caps.receiveDspControl = ReceiveDspControl{{"CW"}, {Dsp::Apf}, {}, {Dsp::Apf}};
    emit radio.capabilitiesChanged(true, source->caps);
    applet.setHasAudioPeakingFilter(true);
    require(!apf->isEnabled(), "native APF waits for a bound slice");
    applet.setSlice(&receiveSlice);
    QWidget* apfRow = applet.findChild<QWidget*>(QStringLiteral("cwApfRow"));
    require(apf && apfLevel && apfRow && !apfRow->isHidden() && apf->isEnabled() && apf->isChecked(),
        "native APF enable remains visible and follows its confirmed slice value");
    require(!apfLevel->isEnabled() && !apfLevel->accessibleDescription().isEmpty(),
        "native APF without a shared percent adjustment leaves the level unavailable with a reason");
    require(apfValue && apfValue->text() == QStringLiteral("--"), "unsupported native APF level never displays a fabricated percent value");
    apfLevel->setValue(60);
    require(apfValue->text() == QStringLiteral("--"), "programmatic unsupported level changes keep the unavailable marker");
    require(receiveSlice.apfLevel() == 37, "programmatic edits cannot invoke an unavailable APF level");
    source->caps.receiveDspControl->unavailableLevels.clear();
    emit radio.capabilitiesChanged(true, source->caps);
    apfReadback.apfLevel = 38;
    receiveSlice.applyChanges(apfReadback);
    require(apfLevel->isEnabled() && apfValue->text() == QStringLiteral("38"),
        "a backend supporting the shared adjustment retains its numeric readback");
    requireLegacyPresentation();
    apfReadback.apf = false;
    receiveSlice.applyChanges(apfReadback);
    source->caps.receiveDspControl->unavailableLevels.append(Dsp::Apf);
    emit radio.capabilitiesChanged(true, source->caps);
    require(apf->isEnabled() && !apf->isChecked() && !apfLevel->isEnabled(),
        "native APF off retains its unavailable level treatment");
    source->caps.receiveDspControl.reset();
    emit radio.capabilitiesChanged(true, source->caps);
    require(apf->isEnabled() && !apf->isChecked() && !apfLevel->isEnabled(),
        "legacy APF off restores ordinary disabled level behavior");
    requireLegacyPresentation();
    apfReadback.apf = true;
    receiveSlice.applyChanges(apfReadback);
    require(apf->isChecked() && apfLevel->isEnabled() && apfValue->text() == QStringLiteral("38"),
        "legacy APF on restores its enabled numeric level");
    requireLegacyPresentation();
    source->caps.receiveDspControl = ReceiveDspControl{{"CW"}, {Dsp::Apf}, {}, {Dsp::Apf}};
    source->caps.receiveDspControl->unavailableLevels.append(Dsp::Apf);
    emit radio.capabilitiesChanged(true, source->caps);
    source->caps.hasAudioPeakingFilter = false;
    source->caps.receiveDspControl->features.clear();
    emit radio.capabilitiesChanged(true, source->caps);
    applet.setHasAudioPeakingFilter(false);
    require(!apfRow->isHidden() && !apf->isEnabled() && apf->isChecked()
        && !apf->accessibleDescription().isEmpty(),
        "unavailable or pending APF keeps its last observed value without hiding the row");
    source->caps.hasAudioPeakingFilter = true;
    source->caps.receiveDspControl->features.append(Dsp::Apf);
    applet.setHasAudioPeakingFilter(true);
    applet.setSlice(nullptr);
    require(!apf->isEnabled() && !apfLevel->isEnabled(),
        "a model-bound APF row stays unavailable without a slice");
    source->connected = false;
    applet.setSlice(&receiveSlice);
    emit radio.connectionStateChanged(false);
    require(!apf->isEnabled() && !apfLevel->isEnabled() && !apf->accessibleDescription().isEmpty(),
        "a disconnected model cannot enable APF using a previously bound slice");
    auto transient = std::make_unique<RadioModel>();
    auto transientBackend = std::make_unique<SettingsBackend>();
    transientBackend->caps.hasAudioPeakingFilter = true;
    transient->setBackendForTest(std::move(transientBackend), QStringLiteral("injected"));
    applet.setRadioModel(transient.get());
    applet.setSlice(&receiveSlice);
    transient.reset();
    require(!apf->isEnabled() && !apfLevel->isEnabled(),
        "destroying the availability model retires its registry and bound receive controls");
    applet.setRadioModel(nullptr);
    applet.setSlice(nullptr);
    require(!apf->isEnabled(), "rebinding after model destruction cannot refresh a dead model reference");
    return 0;
}
