// MIDI Controller Mapping on the style guide's CanonWindow (RFC #6226, #7040):
// a canon workspace whose manual binding editor is a canon dialog, whose
// "Source Already Bound" question defaults to Cancel, and whose every close
// path cancels a Learn still waiting for a control. No MIDI port is opened.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/MidiControlManager.h"
#include "core/ThemeManager.h"
#include "gui/CanonMessageBox.h"
#include "gui/CanonWindow.h"
#include "gui/MidiMappingDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <cstdio>
#include <functional>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const char* name)
{
    std::printf("[%s] %s\n", ok ? "OK" : "FAIL", name);
    g_failures += !ok;
}

void registerParams(MidiControlManager& manager)
{
    for (const auto& [id, name] : {std::pair{"rx.afGain", "AF Gain"}, std::pair{"rx.rfGain", "RF Gain"}}) {
        MidiParam p;
        p.id = QString::fromLatin1(id);
        p.displayName = QString::fromLatin1(name);
        p.category = QStringLiteral("RX");
        p.type = MidiParamType::Slider;
        p.setter = [](float) {};
        p.getter = [] { return 0.0f; };
        manager.registerParam(p);
    }
}

// Answers the first visible top-level window of type T whose title is
// `title` once it is up; a safety timer closes whatever is left so a miss
// fails a check instead of hanging.
template <class T>
struct Driver {
    QString title;
    std::function<void(T*)> answer;
    QString seenDefault;
    bool seen{false};
    QTimer timer;
    QTimer safety;

    Driver(QString t, std::function<void(T*)> a) : title(std::move(t)), answer(std::move(a))
    {
        QObject::connect(&timer, &QTimer::timeout, [this] {
            if (seen) return;
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* window = qobject_cast<T*>(widget);
                if (!window || !window->isVisible() || window->windowTitle() != title) continue;
                seen = true;
                for (QPushButton* button : window->template findChildren<QPushButton*>())
                    if (button->isDefault()) seenDefault = button->text();
                answer(window);
                return;
            }
        });
        timer.start(20);
        safety.setSingleShot(true);
        QObject::connect(&safety, &QTimer::timeout, [] {
            for (QWidget* widget : QApplication::topLevelWidgets())
                if (auto* d = qobject_cast<QDialog*>(widget); d && d->isModal() && d->isVisible())
                    d->reject();
        });
        safety.start(10000);
    }
};

template <class T>
std::function<void(T*)> click(const QString& text)
{
    return [text](T* window) {
        for (QPushButton* button : window->template findChildren<QPushButton*>())
            if (button->text() == text) { button->click(); return; }
        window->reject();
    };
}

template <class T>
std::function<void(T*)> key(Qt::Key k)
{
    return [k](T* window) { QTest::keyClick(window->focusWidget() ? window->focusWidget() : window, k); };
}

void canonWorkspace()
{
    MidiControlManager manager;
    auto& tm = ThemeManager::instance();
    const QString before = tm.activeTheme();
    for (const char* theme : {"Default Dark", "Default Light"}) {
        check(tm.setActiveTheme(QLatin1String(theme)), theme);
        MidiMappingDialog dialog(&manager);
        check(qobject_cast<CanonWindow*>(&dialog) != nullptr, "MIDI Controller Mapping is a CanonWindow");
        check(dialog.windowType() == Qt::Window
                  && dialog.windowFlags().testFlag(Qt::FramelessWindowHint),
              "it is a frameless workspace window");
        check(dialog.windowTitle() == "MIDI Controller Mapping", "the title is unchanged");
        check(dialog.objectName() == "MidiMappingDialog", "it keeps the name screens.json waits for");
        check(dialog.property("themeContainer").toString() == "canon/midiMapping",
              "it uses the canon/midiMapping scope");
        check(dialog.findChild<QLabel*>("canonHeaderTitle") != nullptr, "it carries the canon header");
        bool closeButton = false;
        for (QPushButton* button : dialog.findChildren<QPushButton*>())
            closeButton = closeButton || button->text() == "Close";
        check(!closeButton, "the footer Close is gone; the canon close paths replace it");
        const QString sheet = dialog.bodyWidget()->styleSheet();
        check(!sheet.contains(": ;"), "no token in the sheet resolved empty");
        check(sheet.contains("QMenu") && sheet.contains("selection-color"),
              "the fields' menus keep a panel and their selection an ink");
        bool literal = false;
        for (QWidget* widget : dialog.findChildren<QWidget*>())
            for (const char* hex : {"#30d050", "#808080", "#602020", "#ff6060", "#008ba8", "#8aa8c0"})
                literal = literal || widget->styleSheet().contains(QLatin1String(hex), Qt::CaseInsensitive);
        check(!literal, "the old hard-coded colours are gone");
        check(dialog.minimumHeight() <= 700, "the minimum height fits a 768 px screen");
        check(dialog.minimumHeight() >= dialog.minimumSizeHint().height()
                  && dialog.minimumWidth() >= dialog.minimumSizeHint().width(),
              "the minimum size is never smaller than the layout's");
        // Learn is drawn outlined in canon cyan, Manual… in the plain line
        // colour: read the drawn top edge, mid-way along each button.
        auto* learn = dialog.findChild<QPushButton*>("midiLearnButton");
        auto* manual = dialog.findChild<QPushButton*>("midiManualAddButton");
        dialog.show();
        QTest::qWaitForWindowExposed(&dialog);
        const auto edgeIsCyan = [&](QPushButton* button) {
            const QImage image = button->grab().toImage();
            const QColor cyan = tm.color(button, QStringLiteral("color.canon.cyan"));
            const qreal dpr = image.devicePixelRatio();
            for (int y = 0; y < qRound(3 * dpr); ++y)
                if (QColor(image.pixel(image.width() / 2, y)).rgb() == cyan.rgb()) return true;
            return false;
        };
        check(learn && manual && edgeIsCyan(learn) && !edgeIsCyan(manual),
              "Learn stays visually primary over Manual… (#4760)");
        QPushButton* refresh = nullptr;
        for (QPushButton* b : dialog.findChildren<QPushButton*>())
            if (b->text() == "Refresh") refresh = b;
        check(refresh && !edgeIsCyan(refresh),
              "no other button is outlined as primary (QDialog's implicit default is not)");
        dialog.hide();
        auto* status = dialog.findChild<QLabel*>("midiPortStatus");
        check(status && status->text() == "Disconnected"
                  && status->palette().color(QPalette::WindowText)
                         == tm.color(status, QStringLiteral("color.text.label")),
              "the port status starts in the canon label ink");
    }
    tm.setActiveTheme(before);
}

// Every way out destroys the WA_DeleteOnClose window, and cancels a Learn.
void closePathsCancelLearn()
{
    MidiControlManager manager;
    registerParams(manager);
    for (int path = 0; path < 3; ++path) {
        QPointer<MidiMappingDialog> dialog = new MidiMappingDialog(&manager);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
        const bool exposed = QTest::qWaitForWindowExposed(dialog);
        manager.startLearn(QStringLiteral("rx.afGain"));
        if (path == 0) QTest::keyClick(dialog, Qt::Key_Escape);
        if (path == 1) QTest::keySequence(dialog, QKeySequence::Close);
        if (path == 2) dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(exposed && dialog.isNull(),
              path == 0 ? "Esc destroys the window"
              : path == 1 ? "Ctrl+W / Cmd+W destroys the window"
                          : "the corner close button destroys the window");
        check(!manager.isLearning(), "closing the window cancels a Learn still waiting");
        delete dialog.data();
    }
}

// Return in the editable profile field clicks nothing: a workspace never
// clicks a default button, so Clear All, Refresh and the rest stay put.
void returnInProfileFieldClicksNothing()
{
    MidiControlManager manager;
    registerParams(manager);
    MidiBinding b; b.channel = 0; b.msgType = MidiBinding::CC; b.number = 7; b.paramId = "rx.afGain";
    manager.addBinding(b);
    MidiMappingDialog dialog(&manager);
    dialog.show();
    QTest::qWaitForWindowExposed(&dialog);
    auto* profile = dialog.findChild<QComboBox*>("midiProfileCombo");
    if (!profile || !profile->lineEdit()) { check(false, "the profile field exists"); return; }
    profile->lineEdit()->setFocus();
    QTest::keyClick(profile->lineEdit(), Qt::Key_Return);
    QTest::qWait(50);
    bool boxUp = false;
    for (QWidget* w : QApplication::topLevelWidgets())
        boxUp = boxUp || (qobject_cast<CanonMessageBox*>(w) && w->isVisible());
    check(dialog.isVisible() && manager.bindings().size() == 1 && !boxUp,
          "Return in the profile field neither clears, saves nor closes");
    // The row's 24 px edit and delete glyphs sit wholly inside the table.
    auto* table = dialog.findChild<QTableWidget*>("midiBindingTable");
    bool glyphsFit = table && table->rowCount() == 1;
    for (int column : {5, 6}) {
        QWidget* glyph = table ? table->cellWidget(0, column) : nullptr;
        glyphsFit = glyphsFit && glyph
            && table->viewport()->rect().contains(
                   QRect(glyph->mapTo(table->viewport(), QPoint(0, 0)), glyph->size()));
    }
    check(glyphsFit, "the edit and delete glyphs are not clipped by the table");
    dialog.hide();
}

// Manual…: the editor is a canon dialog in the window's scope with OK the
// default; Cancel and Esc add nothing, OK adds the binding. A second binding on
// the same source asks with Cancel the default; only Replace replaces.
void manualEditorAndSourceQuestion()
{
    MidiControlManager manager;
    registerParams(manager);
    MidiMappingDialog dialog(&manager);
    dialog.show();
    QTest::qWaitForWindowExposed(&dialog);
    auto* param = dialog.findChild<QComboBox*>("midiParamCombo");
    auto* manual = dialog.findChild<QPushButton*>("midiManualAddButton");
    if (!param || !manual) { check(false, "the parameter list and Manual… exist"); return; }

    const auto openEditor = [&](const QString& paramId, std::function<void(CanonWindow*)> answer,
                                bool* canon = nullptr, int number = 7) {
        param->setCurrentIndex(param->findData(paramId));
        Driver<CanonWindow> driver(QStringLiteral("Add MIDI Binding"), [&, answer](CanonWindow* w) {
            if (canon)
                *canon = w->property("themeContainer").toString() == "canon/midiMapping"
                         && w->findChild<QLabel*>("canonHeaderTitle") != nullptr;
            if (auto* spin = w->findChild<QSpinBox*>("manualNumberSpin")) spin->setValue(number);
            answer(w);
        });
        manual->click();
        QTest::qWait(30);
        return driver.seenDefault;
    };

    bool canon = false;
    const QString editorDefault = openEditor("rx.afGain", click<CanonWindow>("Cancel"), &canon);
    check(canon, "the manual editor is a canon dialog in the window's scope");
    check(editorDefault == "OK", "OK is the manual editor's default");
    check(manager.bindings().isEmpty(), "Cancel in the editor adds nothing");
    openEditor("rx.afGain", key<CanonWindow>(Qt::Key_Escape));
    check(manager.bindings().isEmpty(), "Esc in the editor adds nothing");
    openEditor("rx.afGain", click<CanonWindow>("OK"));
    check(manager.bindings().size() == 1 && manager.bindings().value(0).number == 7,
          "OK adds the binding");

    // RF Gain on the same source (channel 1, CC 7) shadows AF Gain.
    const auto answerQuestion = [&](std::function<void(CanonMessageBox*)> answer) {
        QString seenDefault;
        bool seen = false;
        Driver<CanonMessageBox> question(QStringLiteral("Source Already Bound"),
                                         [&, answer](CanonMessageBox* box) {
            seen = true;
            for (QPushButton* button : box->findChildren<QPushButton*>())
                if (button->isDefault()) seenDefault = button->text();
            answer(box);
        });
        openEditor("rx.rfGain", click<CanonWindow>("OK"));
        QTest::qWait(30);
        return std::make_pair(seen, seenDefault);
    };
    for (const auto& [answer, label] :
         std::initializer_list<std::pair<std::function<void(CanonMessageBox*)>, const char*>>{
             {click<CanonMessageBox>("Cancel"), "Cancel keeps the older binding and adds nothing"},
             {key<CanonMessageBox>(Qt::Key_Return), "Return keeps the older binding and adds nothing"},
             {key<CanonMessageBox>(Qt::Key_Escape), "Esc keeps the older binding and adds nothing"}}) {
        const auto [seen, seenDefault] = answerQuestion(answer);
        check(seen && seenDefault == "Cancel", "a shared source asks first, with Cancel the default");
        check(manager.bindings().size() == 1 && manager.bindings().value(0).paramId == "rx.afGain", label);
    }
    answerQuestion(click<CanonMessageBox>("Replace"));
    check(manager.bindings().size() == 1 && manager.bindings().value(0).paramId == "rx.rfGain",
          "Replace swaps the older binding for the new one");
    dialog.hide();
}

// Size and position persist under MidiMappingDialogGeometry, the key the old
// dialog wrote, including its base64 rectangles.
void geometryCarriesOver()
{
    static const QString kKey = QStringLiteral("MidiMappingDialogGeometry");
    auto& settings = AppSettings::instance();
    settings.remove(kKey);
    MidiControlManager manager;
    MidiMappingDialog restored(&manager);
    const QSize chosen(restored.minimumWidth() + 30, restored.minimumHeight() + 20);
    QDialog legacy;
    legacy.resize(chosen);
    settings.setValue(kKey, QString::fromLatin1(legacy.saveGeometry().toBase64()));
    restored.show();
    QTest::qWaitForWindowExposed(&restored);
    for (int i = 0; i < 50 && restored.size() != chosen; ++i)
        QTest::qWait(20);
    check(restored.size() == chosen, "geometry saved by the old dialog restores under its key");
    restored.hide();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile("midi-mapping-dialog");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();

    canonWorkspace();
    closePathsCancelLearn();
    returnInProfileFieldClicksNothing();
    manualEditorAndSourceQuestion();
    geometryCarriesOver();

    return g_failures ? 1 : 0;
}
