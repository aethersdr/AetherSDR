// CanonWindow (RFC #6226): the title-bar-less window must still move and
// close.
//
// Moving: startSystemMove() is unavailable on the offscreen platform, the same
// way it silently fails on xcb and on Windows with WA_TranslucentBackground.
// A drag on the window's ground must fall back to FramelessMoveHelper's manual
// move instead of being swallowed.
//
// Closing: a caption-less QDialog gets no Close shortcut from the platform, so
// CanonWindow wires QKeySequence::Close (⌘W on macOS, Ctrl+W elsewhere).
//
// Reduced motion (RFC #6226): the sparks stop animating when the OS asks for
// reduced motion, and resume when it stops asking.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/CanonIndicators.h"
#include "gui/CanonWindow.h"

#include <QApplication>
#include <QHash>
#include <QImage>
#include <QRegularExpression>
#include <QGuiApplication>
#include <QScreen>
#include <QPushButton>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointer>
#include <QTest>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  expected (%s) to be true\n", \
                     __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

namespace {

void sendMouse(QWidget* w, QEvent::Type type, const QPoint& local,
               Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent ev(type, QPointF(local), QPointF(w->mapToGlobal(local)),
                   button, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-canon-window-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL could not create isolated settings profile\n");
        return 1;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    ThemeManager::instance();

    // ---- a drag on the ground moves the window ----
    {
        CanonWindow w(QStringLiteral("Canon"));
        w.resize(400, 300);
        w.show();
        EXPECT_TRUE(QTest::qWaitForWindowExposed(&w));

        const QPoint start = w.pos();
        const QPoint press(200, 200);   // empty ground, clear of the close button
        sendMouse(&w, QEvent::MouseButtonPress, press, Qt::LeftButton, Qt::LeftButton);
        // The window moves under the cursor, so each step's local position is
        // where the cursor now sits relative to the moved window.
        for (int step = 1; step <= 3; ++step) {
            const QPoint global = w.mapToGlobal(QPoint()) - w.pos() + start + press
                                  + QPoint(30 * step, 40 * step / 3);
            sendMouse(&w, QEvent::MouseMove, w.mapFromGlobal(global), Qt::NoButton, Qt::LeftButton);
        }
        sendMouse(&w, QEvent::MouseButtonRelease, w.mapFromGlobal(w.mapToGlobal(press)),
                  Qt::LeftButton, Qt::NoButton);

        const QPoint moved = w.pos() - start;
        if (moved != QPoint(90, 40)) {
            std::fprintf(stderr, "  drag moved the window by (%d, %d), expected (90, 40)\n",
                         moved.x(), moved.y());
        }
        EXPECT_TRUE(moved == QPoint(90, 40));
        // The release ends the manual move: a later move does nothing.
        sendMouse(&w, QEvent::MouseMove, QPoint(10, 10), Qt::NoButton, Qt::NoButton);
        EXPECT_TRUE(w.pos() - start == QPoint(90, 40));
    }

    // ---- the platform Close shortcut closes the window ----
    {
        const QList<QKeySequence> close = QKeySequence::keyBindings(QKeySequence::Close);
        EXPECT_TRUE(!close.isEmpty());
        if (!close.isEmpty()) {
            QPointer<CanonWindow> w = new CanonWindow(QStringLiteral("Canon"));
            w->setAttribute(Qt::WA_DeleteOnClose);
            w->resize(400, 300);
            w->show();
            w->activateWindow();
            EXPECT_TRUE(QTest::qWaitForWindowActive(w));
            const QKeyCombination key = close.first()[0];
            QTest::keyClick(w, key.key(), key.keyboardModifiers());
            QTest::qWait(0);
            EXPECT_TRUE(!w || !w->isVisible());
            delete w;
        }
    }

    // ---- Escape still closes it (QDialog's own handling) ----
    {
        CanonWindow w(QStringLiteral("Canon"));
        w.resize(400, 300);
        w.show();
        w.activateWindow();
        EXPECT_TRUE(QTest::qWaitForWindowActive(&w));
        QTest::keyClick(&w, Qt::Key_Escape);
        EXPECT_TRUE(!w.isVisible());
    }

    // ---- closing with Escape flushes the saved geometry to disk: QDialog's
    //      reject() hides the window without a close event ----
    {
        const QString key = QStringLiteral("CanonTestEscapeGeometry");
        CanonWindow w(QStringLiteral("Canon"));
        w.setGeometryKey(key);
        w.resize(400, 300);
        w.show();
        w.activateWindow();
        EXPECT_TRUE(QTest::qWaitForWindowActive(&w));
        w.move(w.pos() + QPoint(30, 20));   // saved in memory only
        const QString moved = AppSettings::instance().value(key).toString();
        EXPECT_TRUE(!moved.isEmpty());
        QTest::keyClick(&w, Qt::Key_Escape);
        EXPECT_TRUE(!w.isVisible());
        AppSettings::instance().load();     // drops memory, re-reads the disk
        EXPECT_TRUE(AppSettings::instance().value(key).toString() == moved);
    }

    // ---- a pinned launch size wins over the saved size, and a window whose
    //      saved position is unusable centres at the pinned size ----
    {
        const QString key = QStringLiteral("CanonTestLaunchGeometry");
        // A size, and a position on no screen (an unplugged monitor).
        AppSettings::instance().setValue(key, QStringLiteral("-30000,-30000,1420,900"));
        CanonWindow w(QStringLiteral("Canon"));
        w.setGeometryKey(key);
        w.setLaunchSize(QSize(720, 480));
        w.show();
        EXPECT_TRUE(QTest::qWaitForWindowExposed(&w));
        EXPECT_TRUE(w.size() == QSize(720, 480));
        const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
        EXPECT_TRUE(w.pos() == screen.center() - w.rect().center());
    }

    // ---- a workspace is its own top-level window, and Return or Enter in a
    //      field never clicks a button (AetherTX: Return in a knob's value
    //      field pressed the Gate tab, QDialog's first auto-default) ----
    {
        QWidget parent;
        for (const auto kind : {CanonWindow::Kind::Dialog, CanonWindow::Kind::Workspace}) {
            const bool workspace = kind == CanonWindow::Kind::Workspace;
            CanonWindow w(QStringLiteral("Canon"), &parent, kind);
            EXPECT_TRUE(w.windowType() == (workspace ? Qt::Window : Qt::Dialog));
            EXPECT_TRUE(w.testAttribute(Qt::WA_QuitOnClose) == !workspace);
            auto* box = new QVBoxLayout(w.bodyWidget());
            auto* tab = new QPushButton(QStringLiteral("Gate"));
            auto* field = new QLineEdit;
            box->addWidget(tab);
            box->addWidget(field);
            int clicks = 0;
            QObject::connect(tab, &QPushButton::clicked, [&clicks] { ++clicks; });
            w.resize(400, 300);
            w.show();
            w.activateWindow();
            EXPECT_TRUE(QTest::qWaitForWindowActive(&w));
            field->setFocus();
            QTest::keyClick(field, Qt::Key_Return);
            QTest::keyClick(field, Qt::Key_Enter, Qt::KeypadModifier);
            // A dialog keeps QDialog's default-button behaviour (About's OK).
            EXPECT_TRUE(clicks == (workspace ? 0 : 2));
            EXPECT_TRUE(w.isVisible());
            // A focused button still takes Return itself in a workspace.
            if (workspace) {
                tab->setFocus();
                QTest::keyClick(tab, Qt::Key_Return);
                EXPECT_TRUE(clicks == 1);
            }
        }
    }

    // ---- the sparks hold still under the OS reduced-motion preference ----
    {
        CanonWindow w(QStringLiteral("Canon"));
        auto* lay = new QVBoxLayout(w.bodyWidget());
        QPixmap image(32, 32);
        image.fill(Qt::darkBlue);
        auto* ring = new SparkRing(image, 32);
        auto* border = new SparkBorder(new QPushButton(QStringLiteral("Logbook")), 4);
        lay->addWidget(ring);
        lay->addWidget(border);
        w.resize(400, 300);
        w.show();
        EXPECT_TRUE(QTest::qWaitForWindowExposed(&w));

        // The offscreen platform reports no preference: both animate.
        EXPECT_TRUE(ring->isAnimating());
        EXPECT_TRUE(border->isAnimating());

        ring->setMotionPreference(Qt::MotionPreference::ReducedMotion);
        border->setMotionPreference(Qt::MotionPreference::ReducedMotion);
        EXPECT_TRUE(!ring->isAnimating());
        EXPECT_TRUE(!border->isAnimating());

        // Re-showing must not restart a held spark.
        w.hide();
        w.show();
        EXPECT_TRUE(QTest::qWaitForWindowExposed(&w));
        EXPECT_TRUE(!ring->isAnimating());
        EXPECT_TRUE(!border->isAnimating());

        ring->setMotionPreference(Qt::MotionPreference::NoPreference);
        border->setMotionPreference(Qt::MotionPreference::NoPreference);
        EXPECT_TRUE(ring->isAnimating());
        EXPECT_TRUE(border->isAnimating());

        // Lifting the preference while hidden waits for the next show.
        w.hide();
        ring->setMotionPreference(Qt::MotionPreference::NoPreference);
        EXPECT_TRUE(!ring->isAnimating());
    }

    // ---- the canon indicators show their state: a checked box carries a
    //      tick and a selected radio a centre dot, not just a fill or ring ----
    {
        static const QRegularExpression url(QStringLiteral(
            "(QCheckBox|QRadioButton)::indicator:(un)?checked \\{ image: url\\(([^)]+)\\)"));
        const QString rules = canonIndicatorRules();
        QHash<QString, QString> files;   // "QCheckBox:checked" -> path
        for (auto it = url.globalMatch(rules); it.hasNext();) {
            const auto m = it.next();
            files.insert(m.captured(1) + (m.captured(2).isEmpty() ? QStringLiteral(":checked")
                                                                  : QStringLiteral(":unchecked")),
                         m.captured(3));
        }
        EXPECT_TRUE(files.size() == 4);
        // Sampled at 3x, where the canvas is 54 px. The tick's corner sits at
        // (7.9, 12.0) in the 18 px canvas; the dot is centred at (9, 9).
        const auto at3x = [](const QString& path, QPoint p) {
            QString big = path;
            big.replace(QStringLiteral(".png"), QStringLiteral("@3x.png"));
            const QImage img(big);
            return img.isNull() ? QColor() : img.pixelColor(p);
        };
        const QColor onAccent = ThemeManager::instance().color(QStringLiteral("color.canon.onAccent"));
        const QColor cyan = ThemeManager::instance().color(QStringLiteral("color.canon.cyan"));
        const QPoint tick(24, 36), centre(27, 27);
        const QColor checkOn = at3x(files.value(QStringLiteral("QCheckBox:checked")), tick);
        const QColor checkOff = at3x(files.value(QStringLiteral("QCheckBox:unchecked")), tick);
        const QColor radioOn = at3x(files.value(QStringLiteral("QRadioButton:checked")), centre);
        const QColor radioOff = at3x(files.value(QStringLiteral("QRadioButton:unchecked")), centre);
        const auto near = [](const QColor& a, const QColor& b) {
            return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green())
                 + std::abs(a.blue() - b.blue()) < 40;
        };
        EXPECT_TRUE(near(checkOn, onAccent));    // the tick, in text-on-accent
        EXPECT_TRUE(!near(checkOff, onAccent));  // unchecked: no tick there
        EXPECT_TRUE(near(radioOn, cyan));        // the dot
        EXPECT_TRUE(!near(radioOff, cyan));      // unselected: empty centre
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "PASS canon_window_test\n");
        return 0;
    }
    std::fprintf(stderr, "%d failures\n", g_failures);
    return 1;
}
