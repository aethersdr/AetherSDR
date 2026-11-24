#pragma once

#include <QString>
#include <QStringList>

// Which pans a smaller layout closes, and which layout the survivors get.
// The layout keeps the first pans, so candidates are tried from the end. A
// close the model refuses (keyed TX pan, last pan, a pan another client owns)
// leaves that pan open, and the next earlier candidate is tried instead.
namespace AetherSDR::PanLayoutShrinkPolicy {

struct Outcome {
    QStringList closed;   // closes the model accepted, in call order
    int survivors{0};     // pans left once those closes complete
};

template <class Close>
Outcome closeFromEnd(const QStringList& candidates, int existing, int needed,
                     const QString& notAPan, Close&& close)
{
    Outcome outcome;
    int toRemove = existing - needed;
    for (qsizetype i = candidates.size() - 1; i >= 0 && toRemove > 0; --i) {
        const QString& panId = candidates.at(i);
        if (panId == notAPan) {
            continue;
        }
        if (close(panId)) {
            outcome.closed.append(panId);
            --toRemove;
        }
    }
    outcome.survivors = existing - static_cast<int>(outcome.closed.size());
    return outcome;
}

inline QString defaultLayoutForCount(int panCount)
{
    switch (panCount) {
    case 2: return QStringLiteral("2v");
    case 3: return QStringLiteral("2h1");
    case 4: return QStringLiteral("2x2");
    case 5: return QStringLiteral("3h2");
    case 6: return QStringLiteral("2x3");
    case 7: return QStringLiteral("4h3");
    case 8: return QStringLiteral("2x4");
    default: return QStringLiteral("1");
    }
}

// The requested layout when its count was reached, else the default layout
// for the pans that are left: never cells for a count that does not exist.
inline QString reflowLayout(const QString& requested, int needed, int survivors)
{
    return survivors == needed ? requested : defaultLayoutForCount(survivors);
}

} // namespace AetherSDR::PanLayoutShrinkPolicy
