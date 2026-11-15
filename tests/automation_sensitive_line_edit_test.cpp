#include "AutomationSensitiveLineEdit.h"

#include <QApplication>
#include <cstdio>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QLineEdit edit;
    edit.setText(QStringLiteral("private-test-code"));
    edit.setEchoMode(QLineEdit::Password);
    if (automationLineEditValue(&edit) != QStringLiteral("<hidden>")) {
        std::fprintf(stderr, "masked code was exposed\n");
        return 1;
    }
    edit.setProperty("aetherSensitiveValue", true);
    edit.setEchoMode(QLineEdit::Normal);
    if (automationLineEditValue(&edit) != QStringLiteral("<hidden>")) {
        std::fprintf(stderr, "shown code was exposed\n");
        return 1;
    }
    const QImage shownImage = edit.grab().toImage();
    QImage maskedImage;
    {
        AutomationSensitiveGrabMask mask(&edit);
        if (edit.echoMode() != QLineEdit::Password) {
            std::fprintf(stderr, "grab left shown code visible\n");
            return 1;
        }
        maskedImage = edit.grab().toImage();
        if (maskedImage.isNull()) {
            std::fprintf(stderr, "grab produced no image\n");
            return 1;
        }
    }
    if (shownImage == maskedImage) {
        std::fprintf(stderr, "grab pixels did not mask the shown code\n");
        return 1;
    }
    if (edit.echoMode() != QLineEdit::Normal) {
        std::fprintf(stderr, "grab did not restore Show state\n");
        return 1;
    }
    QWidget container;
    QLineEdit child(&container);
    child.setText(QStringLiteral("nested-code"));
    child.setProperty("aetherSensitiveValue", true);
    {
        AutomationSensitiveGrabMask mask(&container);
        if (child.echoMode() != QLineEdit::Password) {
            std::fprintf(stderr, "parent grab left child code visible\n");
            return 1;
        }
    }
    if (child.echoMode() != QLineEdit::Normal) {
        std::fprintf(stderr, "parent grab did not restore child state\n");
        return 1;
    }
    edit.clear();
    if (!automationLineEditValue(&edit).isEmpty()) {
        std::fprintf(stderr, "empty code did not stay empty\n");
        return 1;
    }
    edit.setProperty("aetherSensitiveValue", false);
    edit.setText(QStringLiteral("public-value"));
    if (automationLineEditValue(&edit) != QStringLiteral("public-value")) {
        std::fprintf(stderr, "ordinary text was redacted\n");
        return 1;
    }
    return 0;
}
