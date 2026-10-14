// Socket-free behavioral checks against the published FlexLib 4.3.16 CWX.cs:
// Send/Insert/SaveMacro substitute quotes with 0x1e; StatusUpdate reverses it.
// AetherSDR currently exposes send and macro operations, but no insert verb.
#include "models/CwxModel.h"
#include "core/backends/flex/CommandParser.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QMap>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <cstdio>
#include <initializer_list>

using namespace AetherSDR;

namespace {
int failures = 0;

void check(bool ok, const char* name)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", name);
    if (!ok) {
        ++failures;
    }
}

void feedStatus(CwxModel& model, const QByteArray& payload)
{
    const ParsedMessage message = CommandParser::parseLine(
        QString::fromUtf8(QByteArray("S12345678|cwx ") + payload + '\n'));
    check(message.type == MessageType::Status && message.object == "cwx",
          "real command parser routes CWX status");
    model.applyStatus(message.kvs);
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    // CQ <quote>DX<quote> = + ( & $; explicit protocol fixture, not the encoder.
    const QByteArray encoded = QByteArray::fromHex("43517f1e44581e7f3d7f2b7f287f267f24");
    const QString text = QStringLiteral("CQ \"DX\" = + ( & $");
    {
        CwxModel model;
        QSignalSpy replies(&model, &CwxModel::replyCommandReady);
        QSignalSpy commands(&model, &CwxModel::commandReady);
        QSignalSpy requested(&model, &CwxModel::transmissionRequested);
        model.send(text);
        check(replies.size() == 1 && commands.isEmpty(), "batched send uses reply path");
        if (replies.size() == 1) {
            const QByteArray wire = CommandParser::buildCommand(42, replies[0][0].toString());
            check(wire == QByteArray("C42|cwx send \"") + encoded + "\" 1\n",
                  "batched command has actual 0x1e/0x7f bytes and only framing quotes");
            check(replies[0][2].toInt() == text.size(), "quote substitution preserves drain character count");
            model.handleSendReply(0, "100,1", replies[0][1].toInt(), replies[0][2].toInt());
            check(model.cwxEndIndex() == 100 + text.size() - 1, "quoted text retains queue drain endpoint");
        }
        check(requested.size() == 1 && requested[0][0].toString() == text,
              "local keyer receives plain quotes, spaces and prosigns");
    }
    {
        CwxModel model;
        QSignalSpy replies(&model, &CwxModel::replyCommandReady);
        QSignalSpy requested(&model, &CwxModel::transmissionRequested);
        model.sendChar("\"");
        model.sendChar(" ");
        check(replies.size() == 2, "live characters each retain reply tracking");
        if (replies.size() == 2) {
            check(replies[0][0].toString().toUtf8() == QByteArray("cwx send \"")
                      + QByteArray::fromHex("1e") + "\" 1",
                  "live quote is encoded");
            check(replies[1][0].toString().toUtf8() == QByteArray("cwx send \"")
                      + QByteArray::fromHex("7f") + "\" 2",
                  "live space is unchanged");
            check(replies[0][2].toInt() == 1 && replies[1][2].toInt() == 1,
                  "live quote and space each count as one character");
        }
        check(requested.size() == 2 && requested[0][0].toString() == "\""
                  && requested[1][0].toString() == " ", "live sidetone text remains plain");
    }
    {
        CwxModel model;
        QStringList commands;
        QObject::connect(&model, &CwxModel::commandReady, &model,
                         [&commands](const QString& command) { commands.append(command); });
        QObject::connect(&model, &CwxModel::replyCommandReady, &model,
                         [&commands](const QString& command, int, int) { commands.append(command); });
        model.send(QStringLiteral("+\"DX\" + ="));
        const QString first = QString::fromUtf8(QByteArray("cwx send \"")
            + QByteArray::fromHex("1e44581e") + "\" 1");
        const QString last = QString::fromUtf8(QByteArray("cwx send \"")
            + QByteArray::fromHex("7f2b7f3d") + "\" 2");
        check(commands == QStringList{"cwx wpm 23", first, "cwx wpm 20", last},
              "quotes survive speed segmentation and standalone prosigns");
        check(model.speed() == 20, "speed modifiers preserve base speed");
    }
    {
        CwxModel model;
        QSignalSpy commands(&model, &CwxModel::commandReady);
        const QString raw = QStringLiteral("+CQ \"DX\" = + ( & $");
        model.saveMacro(11, raw);
        check(model.macro(11) == raw, "local macro retains raw speed modifiers and quotes");
        check(commands.size() == 1 && commands[0][0].toString().toUtf8()
                  == QByteArray("cwx macro save 12 \"") + encoded + '"',
              "macro save strips speed prefixes and encodes quoted text");
        QSignalSpy changed(&model, &CwxModel::macroChanged);
        commands.clear();
        // Radio's BT alias is '*'; a trailing quote stand-in stays inside the field.
        const QByteArray statusText = QByteArray::fromHex("43517f1e44581e7f2a7f2b7f287f267f24");
        feedStatus(model, QByteArray("macro12=\"") + statusText + "\" wpm=25");
        check(model.macro(11) == text && model.speed() == 25,
              "quoted macro status decodes 0x1e, spaces and BT without swallowing next key");
        check(changed.size() == 1 && changed[0][0].toInt() == 11
                  && changed[0][1].toString() == text, "macro change notification contains decoded text");
        check(commands.isEmpty(), "macro status never sends a command");
        QSignalSpy replies(&model, &CwxModel::replyCommandReady);
        model.sendMacro(12);
        check(replies.size() == 1 && replies[0][0].toString().toUtf8()
                  == QByteArray("cwx send \"") + encoded + "\" 1",
              "radio macro can be resent with quotes safely encoded");
        feedStatus(model, QByteArray("macro1=") + QByteArray::fromHex("1e4f4b1e7f2a"));
        check(model.macro(0) == "\"OK\" =", "unquoted status also decodes quote stand-ins");
        feedStatus(model, "macro2=\"CQ TEST*\" macro3=CQ\x7f" "TEST macro4=\"\"");
        check(model.macro(1) == "CQ TEST=" && model.macro(2) == "CQ TEST"
                  && model.macro(3).isEmpty(), "legacy status spaces, BT and empty macro stay compatible");
    }
    {
        CwxModel model;
        QSignalSpy replies(&model, &CwxModel::replyCommandReady);
        model.send("CQ TEST = + ( & $");
        check(replies.size() == 1 && replies[0][0].toString().toUtf8()
                  == QByteArray("cwx send \"CQ\x7f" "TEST\x7f=\x7f+\x7f(\x7f&\x7f$\" 1"),
              "text without quotes is byte-for-byte compatible with older sends");
    }
    for (bool denyAvailability : {true, false}) {
        CwxModel model;
        model.applyStatus({{"macro1", "\"CQ\""}});
        if (denyAvailability) {
            model.setSendAvailability([] { return false; });
        } else {
            model.setTransmissionAdmission([] { return CwxModel::TransmissionPermit{}; });
        }
        QSignalSpy commands(&model, &CwxModel::commandReady);
        QSignalSpy replies(&model, &CwxModel::replyCommandReady);
        QSignalSpy requested(&model, &CwxModel::transmissionRequested);
        QSignalSpy dispatched(&model, &CwxModel::transmissionDispatched);
        model.send("\"CQ\"");
        model.sendChar("\"");
        model.sendMacro(1);
        check(commands.isEmpty() && replies.isEmpty() && requested.isEmpty() && dispatched.isEmpty(),
              denyAvailability ? "TX availability denial still blocks all sends"
                               : "TX admission denial still blocks all sends");
    }
    return failures == 0 ? 0 : 1;
}
