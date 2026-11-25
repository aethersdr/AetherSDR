// A smaller pan layout closes pans from the end. A close the model refuses
// (keyed TX pan, last pan, a pan another client owns) stops the shrink: no
// earlier pan closes in its place, the survivors are reflowed for the count
// that exists, and that layout is what is saved and reported (#7585).
// Pure policy, plus a source pin on the MainWindow wiring.

#include "gui/PanLayoutShrinkPolicy.h"

#include <QByteArray>
#include <QFile>
#include <QSet>
#include <QString>
#include <QStringList>

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    failures += ok ? 0 : 1;
}

struct Model {
    QSet<QString> refuses;
    QStringList asked;
    bool operator()(const QString& panId)
    {
        asked.append(panId);
        return !refuses.contains(panId);
    }
};

PanLayoutShrinkPolicy::Outcome shrink(Model& model, const QStringList& pans, int needed)
{
    return PanLayoutShrinkPolicy::closeFromEnd(
        pans, static_cast<int>(pans.size()), needed, QStringLiteral("placeholder"),
        [&model](const QString& panId) { return model(panId); });
}

QByteArray body(const QByteArray& code, const QByteArray& signature)
{
    const qsizetype at = code.indexOf(signature);
    return at < 0 ? QByteArray() : code.mid(at, code.indexOf("\n}\n", at) - at);
}

QByteArray readSource(const char* path, bool& ok)
{
    QFile file(QStringLiteral(AETHER_SOURCE_DIR) + QLatin1Char('/') + QLatin1String(path));
    ok = ok && file.open(QIODevice::ReadOnly);
    return file.readAll();
}

} // namespace

int main()
{
    const QStringList two{QStringLiteral("p1"), QStringLiteral("p2")};
    const QStringList three{QStringLiteral("p1"), QStringLiteral("p2"), QStringLiteral("p3")};
    // A three-pan arrangement that is not the three-pan default, so keeping
    // it cannot be confused with a reflow to defaultLayoutForCount(3).
    const QString previous = QStringLiteral("12h");

    {
        Model model;
        const auto outcome = shrink(model, three, 1);
        check(outcome.closed == QStringList({QStringLiteral("p3"), QStringLiteral("p2")})
                  && outcome.survivors == 1 && model.asked == outcome.closed,
              "control: nothing refused, 3 -> 1 closes the last two, from the end");
        check(PanLayoutShrinkPolicy::appliedLayout(QStringLiteral("1"), 1, outcome, previous)
                  == QStringLiteral("1"),
              "control: the requested layout is applied when its count is reached");
    }
    {
        Model model;
        model.refuses.insert(QStringLiteral("p3"));
        const auto outcome = shrink(model, three, 2);
        check(model.asked == QStringList{QStringLiteral("p3")},
              "a refused last candidate stops the shrink: no earlier pan is asked");
        check(outcome.closed.isEmpty() && outcome.survivors == 3,
              "and nothing closes, so all three pans stay");
        check(PanLayoutShrinkPolicy::appliedLayout(QStringLiteral("2v"), 2, outcome, previous)
                  == previous,
              "the layout in place before is what is applied, not the 2v requested");
    }
    {
        // HL2, two pans, TX on pan 2, keyed: pan 2 is refused. Pan 1 is the
        // pan the operator kept, so it must not close in its place.
        Model model;
        model.refuses.insert(QStringLiteral("p2"));
        const auto outcome = shrink(model, two, 1);
        check(outcome.closed.isEmpty() && outcome.survivors == 2
                  && model.asked == QStringList{QStringLiteral("p2")},
              "2 -> 1 with the last pan refused closes nothing, never the earlier pan");
        check(PanLayoutShrinkPolicy::keepsArrangement(outcome),
              "and the operator's arrangement is kept: nothing is reflowed");
    }
    {
        // 3 -> 1 with the middle pan refused: the last closes, then the shrink
        // stops at the refusal and the two left get a two-pan layout.
        Model model;
        model.refuses.insert(QStringLiteral("p2"));
        const auto outcome = shrink(model, three, 1);
        check(model.asked == QStringList({QStringLiteral("p3"), QStringLiteral("p2")})
                  && outcome.closed == QStringList{QStringLiteral("p3")} && outcome.survivors == 2,
              "3 -> 1 with the middle pan refused: the last closes, then the shrink stops");
        check(!PanLayoutShrinkPolicy::keepsArrangement(outcome)
                  && PanLayoutShrinkPolicy::appliedLayout(QStringLiteral("1"), 1, outcome, previous)
                         == QStringLiteral("2v"),
              "and the applied layout is the two-pan default, never the 1 requested");
    }
    {
        Model model;
        model.refuses = {QStringLiteral("p1"), QStringLiteral("p2"), QStringLiteral("p3")};
        const auto outcome = shrink(model, three, 1);
        check(outcome.closed.isEmpty() && outcome.survivors == 3
                  && model.asked == QStringList{QStringLiteral("p3")},
              "every close refused: only the first is asked, and none counts as closed");
        check(PanLayoutShrinkPolicy::reflowLayout(QStringLiteral("1"), 1, outcome.survivors)
                  == QStringLiteral("2h1"),
              "reflowLayout names the 3-pan default for three survivors");
    }
    {
        Model model;
        const QStringList withPlaceholder{QStringLiteral("p1"), QStringLiteral("p2"),
                                          QStringLiteral("placeholder")};
        const auto outcome = shrink(model, withPlaceholder, 2);
        check(!model.asked.contains(QStringLiteral("placeholder"))
                  && outcome.closed == QStringList{QStringLiteral("p2")},
              "the placeholder cell is never asked to close, and is not a refusal");
    }
    {
        // Source pin (MainWindow cannot be constructed in a test): the shrink
        // goes through the policy, and both callers save and the bridge
        // reports the layout applyPanLayout returns.
        bool read = true;
        const QByteArray mainWindow = readSource("src/gui/MainWindow.cpp", read);
        const QByteArray shortcuts = readSource("src/gui/MainWindow_Shortcuts.cpp", read);
        const QByteArray workspace = readSource("src/gui/MainWindow_Workspace.cpp", read);
        const QByteArray apply = body(mainWindow,
            "QString MainWindow::applyPanLayout(const QString& layoutId, const QString& previousLayout)");
        check(read && apply.contains("PanLayoutShrinkPolicy::closeFromEnd(")
                  && apply.contains("return m_radioModel.removePanadapter(panId);")
                  && apply.contains("PanLayoutShrinkPolicy::appliedLayout(layoutId, needed, outcome, previousLayout)")
                  && apply.contains("startCanvasPanLayoutSettle(reflowId, panCountForLayoutId(reflowId));")
                  && apply.contains("return reflowId;"),
              "applyPanLayout shrinks through the policy and returns the applied layout");
        const QByteArray save = "const QString appliedLayout = applyPanLayout(layoutId, previousLayout);";
        check(shortcuts.contains(save)
                  && shortcuts.contains("s.setValue(\"PanadapterLayout\", appliedLayout);")
                  && workspace.contains(save)
                  && workspace.contains("settings.setValue(QStringLiteral(\"PanadapterLayout\"), appliedLayout);"),
              "the layout dialog and the bridge both save the applied layout");
        check(workspace.contains("out[QStringLiteral(\"layout\")] = appliedLayout;")
                  && workspace.contains("out[QStringLiteral(\"targetPanCount\")] = appliedPanCount;")
                  && !workspace.contains("out[QStringLiteral(\"layout\")] = layoutId;"),
              "the bridge reports the applied layout and its pan count");
    }

    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
