// A smaller pan layout closes pans from the end. A close the model refuses
// (keyed TX pan, last pan, a pan another client owns) leaves that pan open:
// the next earlier candidate closes instead, and the survivors are reflowed
// for the count that exists. Pure policy; no radio, no widgets (#7585).

#include "gui/PanLayoutShrinkPolicy.h"

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

} // namespace

int main()
{
    const QStringList two{QStringLiteral("p1"), QStringLiteral("p2")};
    const QStringList three{QStringLiteral("p1"), QStringLiteral("p2"), QStringLiteral("p3")};

    {
        Model model;
        const auto outcome = shrink(model, three, 1);
        check(outcome.closed == QStringList({QStringLiteral("p3"), QStringLiteral("p2")})
                  && outcome.survivors == 1 && model.asked == outcome.closed,
              "control: nothing refused, 3 -> 1 closes the last two, from the end");
        check(PanLayoutShrinkPolicy::reflowLayout(QStringLiteral("1"), 1, outcome.survivors)
                  == QStringLiteral("1"),
              "control: the requested layout is reflowed when its count is reached");
    }
    {
        Model model;
        model.refuses.insert(QStringLiteral("p3"));
        const auto outcome = shrink(model, three, 2);
        check(model.asked == QStringList({QStringLiteral("p3"), QStringLiteral("p2")}),
              "a refused last candidate: the next earlier candidate is tried");
        check(outcome.closed == QStringList{QStringLiteral("p2")} && outcome.survivors == 2,
              "and it closes instead, so the pan count matches the layout");
    }
    {
        // HL2, two pans, TX on pan 2, keyed: pan 2 is refused and pan 1 is the
        // only other candidate. Taking it would leave the TX pan alone, which
        // is still one pan; the count matches either way.
        Model model;
        model.refuses.insert(QStringLiteral("p2"));
        const auto outcome = shrink(model, two, 1);
        check(outcome.closed == QStringList{QStringLiteral("p1")} && outcome.survivors == 1,
              "2 -> 1 with the last pan refused closes the earlier one");
    }
    {
        Model model;
        model.refuses = {QStringLiteral("p1"), QStringLiteral("p2"), QStringLiteral("p3")};
        const auto outcome = shrink(model, three, 1);
        check(outcome.closed.isEmpty() && outcome.survivors == 3,
              "every close refused: no pan counts as closed");
        check(PanLayoutShrinkPolicy::reflowLayout(QStringLiteral("1"), 1, outcome.survivors)
                  == QStringLiteral("2h1"),
              "and the survivors are reflowed for the 3 pans that exist, not the 1 requested");
    }
    {
        Model model;
        model.refuses = {QStringLiteral("p2"), QStringLiteral("p3")};
        const auto outcome = shrink(model, three, 1);
        check(outcome.closed == QStringList{QStringLiteral("p1")} && outcome.survivors == 2
                  && PanLayoutShrinkPolicy::reflowLayout(QStringLiteral("1"), 1, 2)
                         == QStringLiteral("2v"),
              "two refused of three: one closes, the two left get a two-pan layout");
    }
    {
        Model model;
        const QStringList withPlaceholder{QStringLiteral("p1"), QStringLiteral("p2"),
                                          QStringLiteral("placeholder")};
        const auto outcome = shrink(model, withPlaceholder, 2);
        check(!model.asked.contains(QStringLiteral("placeholder"))
                  && outcome.closed == QStringList{QStringLiteral("p2")},
              "the placeholder cell is never asked to close");
    }

    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
