#include "TestSettingsProfile.h"
#include "core/AutomationServer.h"

#include <QApplication>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonObject>
#include <QLineEdit>
#include <QWidget>
#include <cstdio>

namespace AetherSDR {
class AutomationServerTestAccess
{
public:
    static QJsonObject request(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};
}

namespace {
bool samePixels(const QImage& left, const QImage& right)
{
    if (left.size() != right.size() || left.isNull()) {
        return false;
    }
    for (int y = 0; y < left.height(); ++y) {
        for (int x = 0; x < left.width(); ++x) {
            if (left.pixel(x, y) != right.pixel(x, y)) {
                return false;
            }
        }
    }
    return true;
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("automation-sensitive-grab-command"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    QWidget root;
    root.setObjectName(QStringLiteral("authGrabRoot"));
    auto* layout = new QHBoxLayout(&root);
    auto* code = new QLineEdit(&root);
    code->setText(QStringLiteral("private-test-code"));
    code->setProperty("aetherSensitiveValue", true);
    code->setEchoMode(QLineEdit::Normal);
    code->setFocusPolicy(Qt::NoFocus); // no blinking caret between grabs
    layout->addWidget(code);
    root.resize(300, 60);
    root.show();
    app.processEvents();
    const QImage shown = root.grab().toImage();
    code->setEchoMode(QLineEdit::Password);
    const QImage expected = root.grab().toImage();
    code->setEchoMode(QLineEdit::Normal);

    AetherSDR::AutomationServer server; // Direct dispatch; no socket listener.
    const QJsonObject response = AetherSDR::AutomationServerTestAccess::request(
        server, QByteArrayLiteral("grab authGrabRoot"));
    const QString path = response.value(QStringLiteral("path")).toString();
    const QImage captured(path);
    QFile::remove(path);
    if (!response.value(QStringLiteral("ok")).toBool() || captured.isNull()
        || !samePixels(captured, expected) || samePixels(captured, shown)
        || code->echoMode() != QLineEdit::Normal) {
        std::fprintf(stderr, "automation grab exposed or did not restore shown code\n");
        return 1;
    }
    return 0;
}
