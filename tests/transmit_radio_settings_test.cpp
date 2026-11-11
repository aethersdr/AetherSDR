#include "TestSettingsProfile.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>

#include <cstdlib>
#include <iostream>
#include <memory>
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
    TestSettingsProfile profile(QStringLiteral("transmit-radio-settings"));
    require(profile.isValid(), "isolated settings");
    QCoreApplication application(argc, argv);
    using Field = TransmitControlRequest::Field;
    TransmitModel model;
    const RadioTransmitSettings settings{{Field::RfPower, Field::TunePower, Field::MicGain,
        Field::ProcessorEnabled, Field::ProcessorLevel, Field::MonitorEnabled,
        Field::MonitorLevel, Field::CwSpeed, Field::CwPitch, Field::CwBreakIn,
        Field::CwDelay, Field::CwIambicMode, Field::CwSwap, Field::Filter, Field::CwSidetone, Field::CwlEnabled}};
    QList<TransmitControlRequest> requests;
    QObject::connect(&model, &TransmitModel::controlRequested, &application,
        [&](const TransmitControlRequest& request) { requests.append(request); });
    model.setRadioTransmitSettings(settings);
    require(requests.isEmpty() && model.supportsSetting(Field::MicGain)
        && !model.supportsSetting(Field::VoxEnabled), "setting support changes cannot write to a radio");
    TransmitDelta observed;
    observed.rfPower = 25;
    observed.tunePower = 10;
    observed.micLevel = 30;
    observed.speechProcEnable = false;
    observed.speechProcLevel = 1;
    observed.sbMonitor = false;
    observed.monGainSb = 15;
    observed.cwSpeed = 20;
    observed.cwPitch = 600;
    observed.cwBreakIn = false;
    observed.cwDelay = 500;
    observed.cwIambicMode = 0;
    observed.cwSwapPaddles = false;
    observed.cwSidetone = false;
    observed.cwlEnabled = false;
    observed.txFilterLow = 10;
    observed.txFilterHigh = 4000;
    model.applyChanges(observed);
    model.setSpeechProcessorLevelMaximum(100);
    model.setRfPower(70);
    require(model.rfPower() == 25 && model.rfPowerIsFromRadio()
        && std::get<int>(requests.last().value) == 70,
        "RF request cannot replace confirmed power or demote its provenance");
    model.setTunePower(20);
    require(model.tunePower() == 10 && requests.last().field == Field::TunePower
        && std::get<int>(requests.last().value) == 20, "native tune-power intent waits for radio readback");
    model.setMicLevel(45);
    require(model.micLevel() == 30 && std::get<int>(requests.last().value) == 45,
        "mic gain waits for radio readback");
    model.setSpeechProcessorEnable(true);
    require(!model.speechProcessorEnable()
        && std::get<ProcessorSetting>(requests.last().value).enabled,
        "processor request cannot invent an observed enable state");
    model.setSpeechProcessorLevel(60);
    require(model.speechProcessorLevel() == 1
        && std::get<ProcessorSetting>(requests.last().value).level == 60,
        "processor level remains radio authoritative");
    model.setSbMonitor(true);
    model.setMonGainSb(43);
    require(!model.sbMonitor() && model.monGainSb() == 15
        && std::get<MonitorSetting>(requests.last().value).level == 43,
        "monitor requests remain distinct from live switch and level");
    model.setCwSpeed(30);
    model.setCwPitch(750);
    model.setCwBreakIn(true);
    model.setCwDelay(750);
    model.setCwIambicMode(1);
    model.setCwSwapPaddles(true);
    require(model.cwSpeed() == 20 && model.cwPitch() == 600 && !model.cwBreakIn()
        && model.cwDelay() == 500 && model.cwIambicMode() == 0 && !model.cwSwapPaddles(),
        "keyer settings wait for native observations");
    model.setCwlEnabled(true);
    require(!model.cwlEnabled() && std::get<bool>(requests.last().value), "native CW BFO intent does not replace observed sideband");
    model.setCwSidetone(true);
    require(!model.cwSidetone() && std::get<bool>(requests.last().value), "radio sidetone enable waits for native readback");
    model.setTxFilter(100, 3500);
    const TxPassband requestedFilter = std::get<TxPassband>(requests.last().value);
    require(model.txFilterLow() == 10 && model.txFilterHigh() == 4000
        && requestedFilter.lowHz == 100 && requestedFilter.highHz == 3500,
        "radio-owned TX filter requests cannot optimistically replace either edge");
    const bool previousVox = model.voxEnable();
    const bool previousIambic = model.cwIambic();
    model.setVoxEnable(!previousVox);
    model.setCwIambic(!previousIambic);
    require(model.voxEnable() == previousVox && model.cwIambic() == previousIambic,
        "unimplemented setting requests cannot alter live state before refusal");
    observed.rfPower = 65;
    observed.tunePower = 20;
    observed.micLevel = 42;
    observed.speechProcLevel = 55;
    observed.sbMonitor = true;
    observed.monGainSb = 45;
    observed.cwSpeed = 28;
    observed.cwPitch = 725;
    observed.cwDelay = 700;
    observed.cwSidetone = true;
    observed.cwlEnabled = true;
    observed.txFilterLow = 100;
    observed.txFilterHigh = 3500;
    const qsizetype count = requests.size();
    model.applyChanges(observed);
    require(model.rfPower() == 65 && model.tunePower() == 20 && model.micLevel() == 42 && model.speechProcessorLevel() == 55
        && model.sbMonitor() && model.monGainSb() == 45 && model.cwSpeed() == 28
        && model.cwPitch() == 725 && model.cwDelay() == 700
        && model.txFilterLow() == 100 && model.txFilterHigh() == 3500 && model.cwSidetone() && model.cwlEnabled() && requests.size() == count,
        "readback adopts radio quantization without a command feedback loop");
    model.setRadioTransmitSettings(std::nullopt);
    model.setRfPower(90);
    model.setMicLevel(52);
    model.setCwSpeed(35);
    require(model.rfPower() == 90 && model.micLevel() == 52 && model.cwSpeed() == 35,
        "absent contract retains existing desktop behavior for other backends");

    std::unique_ptr<TransmitModel> mortal = std::make_unique<TransmitModel>();
    mortal->setRadioTransmitSettings(settings);
    QObject::connect(mortal.get(), &TransmitModel::controlRequested, &application,
        [&](const TransmitControlRequest&) { mortal.reset(); });
    mortal->setCwDelay(700);
    require(!mortal, "destruction during a radio-owned request stops setter work");
    return 0;
}
