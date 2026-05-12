#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QObject>
#include <QStringList>

#include <iostream>

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    return condition;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    TransmitModel tx;
    QStringList commands;
    QStringList blockedMessages;
    QObject::connect(&tx, &TransmitModel::commandReady,
                     [&commands](const QString& cmd) { commands.append(cmd); });
    QObject::connect(&tx, &TransmitModel::pttBlocked,
                     [&blockedMessages](const QString& message) { blockedMessages.append(message); });

    bool ok = true;

    tx.startTwoToneTune();
    ok &= expect(commands == QStringList({
                     "transmit set tune_mode=two_tone",
                     "transmit tune 1",
                 }),
                 "two-tone tune sets mode before starting tune");

    tx.applyTransmitStatus({{"tune", "1"}});
    commands.clear();
    tx.toggleTwoToneTune();
    ok &= expect(commands == QStringList({
                     "transmit tune 0",
                     "transmit set tune_mode=single_tone",
                 }),
                 "two-tone tune toggle stops and restores single-tone mode");

    tx.applyTransmitStatus({{"tune", "0"}});
    commands.clear();
    tx.toggleTwoToneTune();
    ok &= expect(commands == QStringList({
                     "transmit set tune_mode=two_tone",
                     "transmit tune 1",
                 }),
                 "two-tone tune toggle starts two-tone when not tuning");

    commands.clear();
    tx.setTuneMode("single_tone");
    ok &= expect(commands == QStringList({"transmit set tune_mode=single_tone"}),
                 "single-tone tune mode command is accepted");

    commands.clear();
    tx.setTuneMode("invalid");
    ok &= expect(commands.isEmpty(),
                 "invalid tune mode is ignored");

    tx.setPttPreflight([](TransmitModel::PttSource source) {
        return source == TransmitModel::PttSource::Tune
            ? QStringLiteral("blocked")
            : QString();
    });

    blockedMessages.clear();
    commands.clear();
    tx.startTune();
    ok &= expect(commands.isEmpty()
                 && blockedMessages == QStringList({"blocked"}),
                 "tune preflight blocks tune command");

    blockedMessages.clear();
    commands.clear();
    tx.startTwoToneTune();
    ok &= expect(commands.isEmpty()
                 && blockedMessages == QStringList({"blocked"}),
                 "two-tone preflight blocks setup and tune commands");

    return ok ? 0 : 1;
}
