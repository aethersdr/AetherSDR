// Focused CWX panel behavior tests.
// Run: ./build/cwx_panel_test

#include "gui/CwxPanel.h"
#include "models/CwxModel.h"

#include <QApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QMap>
#include <QPushButton>
#include <QStringList>
#include <QTextEdit>
#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-56s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

QPushButton* buttonByText(CwxPanel& panel, const QString& text)
{
    const auto buttons = panel.findChildren<QPushButton*>();
    for (auto* button : buttons) {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

QTextEdit* inputEdit(CwxPanel& panel)
{
    const auto edits = panel.findChildren<QTextEdit*>();
    for (auto* edit : edits) {
        if (edit->placeholderText() == QLatin1String("Type CW message..."))
            return edit;
    }
    return nullptr;
}

struct Fixture {
    CwxModel model;
    CwxPanel panel{&model};
    QStringList commands;

    Fixture()
    {
        QObject::connect(&model, &CwxModel::commandReady,
                         [this](const QString& command) {
                             commands.push_back(command);
                         });
    }
};

bool requireSendWidgets(Fixture& fixture, QPushButton*& send, QPushButton*& live,
                        QPushButton*& setup, QTextEdit*& input)
{
    send = buttonByText(fixture.panel, "Send");
    live = buttonByText(fixture.panel, "Live");
    setup = buttonByText(fixture.panel, "Setup");
    input = inputEdit(fixture.panel);

    const bool ok = send && live && setup && input;
    report("CWX controls are present", ok);
    return ok;
}

QString sendCommand(const QString& text, int block)
{
    QString encoded = text;
    encoded.replace(' ', QChar(0x7f));
    return QString("cwx send \"%1\" %2").arg(encoded).arg(block);
}

void testLiveButtonTogglesOff()
{
    Fixture f;
    QPushButton *send = nullptr, *live = nullptr, *setup = nullptr;
    QTextEdit* input = nullptr;
    if (!requireSendWidgets(f, send, live, setup, input))
        return;

    live->click();
    report("Live click enables live mode",
           f.model.isLive() && live->isChecked());

    live->click();
    report("second Live click disables live mode",
           !f.model.isLive() && !live->isChecked());
}

void testSendButtonSendsWhenLiveOff()
{
    Fixture f;
    QPushButton *send = nullptr, *live = nullptr, *setup = nullptr;
    QTextEdit* input = nullptr;
    if (!requireSendWidgets(f, send, live, setup, input))
        return;

    input->setPlainText("CQ TEST");
    send->click();

    report("Send button sends current input",
           f.commands == QStringList{sendCommand("CQ TEST", 1)});
    report("Send button clears input after send",
           input->toPlainText().isEmpty());
}

void testEnterStillSendsWhenLiveOff()
{
    Fixture f;
    QPushButton *send = nullptr, *live = nullptr, *setup = nullptr;
    QTextEdit* input = nullptr;
    if (!requireSendWidgets(f, send, live, setup, input))
        return;

    input->setPlainText("73");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
    QCoreApplication::sendEvent(input, &enter);

    report("Enter key still sends current input",
           f.commands == QStringList{sendCommand("73", 1)});
}

void testSendButtonTurnsLiveOffWithoutDuplicateSend()
{
    Fixture f;
    QPushButton *send = nullptr, *live = nullptr, *setup = nullptr;
    QTextEdit* input = nullptr;
    if (!requireSendWidgets(f, send, live, setup, input))
        return;

    live->click();
    input->setPlainText("ALREADY KEYED");
    f.commands.clear();

    send->click();

    report("Send button exits live mode",
           !f.model.isLive() && !live->isChecked());
    report("Send button does not duplicate-send live text",
           f.commands.isEmpty());
    report("Send button keeps live text visible when exiting live",
           input->toPlainText() == QLatin1String("ALREADY KEYED"));
}

void testSetupTurnsLiveOff()
{
    Fixture f;
    QPushButton *send = nullptr, *live = nullptr, *setup = nullptr;
    QTextEdit* input = nullptr;
    if (!requireSendWidgets(f, send, live, setup, input))
        return;

    live->click();
    setup->click();

    report("Setup exits live mode",
           !f.model.isLive() && !live->isChecked() && setup->isChecked());
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    std::printf("CWX panel behavior test harness\n\n");

    testLiveButtonTogglesOff();
    testSendButtonSendsWhenLiveOff();
    testEnterStillSendsWhenLiveOff();
    testSendButtonTurnsLiveOffWithoutDuplicateSend();
    testSetupTurnsLiveOff();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}
