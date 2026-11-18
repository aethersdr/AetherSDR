// The VFO DSP tab's RTTY Mark and Shift steppers (buttons and label scroll)
// stay inside the app's range, MemoryFields::kRttyMark*/kRttyShift* (ruling
// on #7093): ▶ never steps past 4000 / 2000 Hz and ◀ never below 0. A value
// already past an edge leaves that arrow inert and the other one still steps.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/MemoryFieldValues.h"
#include "core/backends/SliceDelta.h"
#include "gui/GuardedSlider.h"
#include "gui/VfoWidget.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QBoxLayout>
#include <QPushButton>
#include <QtTest>

using namespace AetherSDR;

namespace {

QBoxLayout* layoutHolding(QLayout* root, QWidget* w)
{
    if (!root) return nullptr;
    if (auto* box = qobject_cast<QBoxLayout*>(root); box && box->indexOf(w) >= 0) return box;
    for (int i = 0; i < root->count(); ++i)
        if (auto* found = layoutHolding(root->itemAt(i)->layout(), w)) return found;
    return nullptr;
}

// The one ScrollableLabel showing `text`; null if none or several match, so
// the test can never drive another control's arrows by mistake.
ScrollableLabel* labelWithText(VfoWidget& vfo, const QString& text)
{
    ScrollableLabel* match = nullptr;
    for (auto* l : vfo.findChildren<ScrollableLabel*>()) {
        if (l->text() != text) continue;
        if (match) return nullptr;
        match = l;
    }
    return match;
}

// The ◀ and ▶ buttons sit either side of their value label in one row.
QPushButton* neighbor(ScrollableLabel* lbl, int offset)
{
    QBoxLayout* box = nullptr;
    for (QWidget* p = lbl->parentWidget(); p && !box; p = p->parentWidget())
        box = layoutHolding(p->layout(), lbl);
    if (!box) return nullptr;
    auto* item = box->itemAt(box->indexOf(lbl) + offset);
    return item ? qobject_cast<QPushButton*>(item->widget()) : nullptr;
}

} // namespace

class VfoRttyStepLimitsTest : public QObject
{
    Q_OBJECT
private slots:
    void edges()
    {
        QCOMPARE(MemoryFields::kRttyMarkMaxHz, 4000);
        QCOMPARE(MemoryFields::kRttyShiftMaxHz, 2000);

        SliceModel slice(0);
        SliceDelta d;
        d.mode = QStringLiteral("RTTY");
        d.rttyMark = 4000;
        d.rttyShift = 2000;
        slice.applyChanges(d);
        VfoWidget vfo;
        vfo.setSlice(&slice);
        vfo.show();
        QVERIFY(QTest::qWaitForWindowExposed(&vfo));

        auto* mark = labelWithText(vfo, QStringLiteral("4000"));
        auto* shift = labelWithText(vfo, QStringLiteral("2000"));
        // Mark and Shift arrows are the TriBtns either side of these labels.
        QVERIFY(mark);
        QVERIFY(shift);
        auto* markPlus = neighbor(mark, +1);
        auto* markMinus = neighbor(mark, -1);
        auto* shiftPlus = neighbor(shift, +1);
        auto* shiftMinus = neighbor(shift, -1);
        QVERIFY(markPlus && markMinus && shiftPlus && shiftMinus);

        int requests = 0;
        connect(&slice, &SliceModel::controlRequested, this, [&] { ++requests; });

        // At the ceilings: ▶ and scroll-up send nothing.
        markPlus->click();
        QCOMPARE(slice.rttyMark(), 4000);
        emit mark->scrolled(1);
        QCOMPARE(slice.rttyMark(), 4000);
        shiftPlus->click();
        QCOMPARE(slice.rttyShift(), 2000);
        emit shift->scrolled(1);
        QCOMPARE(slice.rttyShift(), 2000);
        QCOMPARE(requests, 0);

        // Already above the ceiling (from the radio): ▶ inert, ◀ steps down.
        SliceDelta above;
        above.rttyMark = 4100;
        above.rttyShift = 2010;
        slice.applyChanges(above);
        markPlus->click();
        QCOMPARE(slice.rttyMark(), 4100);
        shiftPlus->click();
        QCOMPARE(slice.rttyShift(), 2010);
        QCOMPARE(requests, 0);
        markMinus->click();
        QCOMPARE(slice.rttyMark(), 4075);
        shiftMinus->click();
        QCOMPARE(slice.rttyShift(), 2005);

        // One step below the ceiling still reaches it.
        SliceDelta below;
        below.rttyMark = 3975;
        below.rttyShift = 1995;
        slice.applyChanges(below);
        markPlus->click();
        QCOMPARE(slice.rttyMark(), 4000);
        shiftPlus->click();
        QCOMPARE(slice.rttyShift(), 2000);

        // The floors: ◀ reaches 0 and stops there.
        SliceDelta low;
        low.rttyMark = 25;
        low.rttyShift = 5;
        slice.applyChanges(low);
        markMinus->click();
        QCOMPARE(slice.rttyMark(), 0);
        shiftMinus->click();
        QCOMPARE(slice.rttyShift(), 0);
        requests = 0;
        markMinus->click();
        emit mark->scrolled(-1);
        QCOMPARE(slice.rttyMark(), 0);
        shiftMinus->click();
        emit shift->scrolled(-1);
        QCOMPARE(slice.rttyShift(), 0);
        QCOMPARE(requests, 0);
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("vfo-rtty-step-limits"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    VfoRttyStepLimitsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "vfo_rtty_step_limits_test.moc"
