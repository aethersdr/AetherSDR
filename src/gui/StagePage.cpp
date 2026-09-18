#include "StagePage.h"
#include "CompactMetrics.h"
#include "EditorFramelessTitleBar.h"
#include "ModemChrome.h"
#include "core/ThemeManager.h"

#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>

namespace AetherSDR {

namespace {

// Holds one stage panel and shrinks its graphics until it fits the page.
//
// These panels were drawn for a window twice the size of this one, and they
// spend that space on knobs, curves and meters rather than on text. So the
// page keeps every label at the size it was meant to be read at and takes the
// graphics down instead (CompactMetrics), rather than scaling the panel whole
// and shrinking the type with it.
//
// Nothing scrolls: whatever is on this page is all of it.
class StagePage final : public QWidget {
public:
    explicit StagePage(QWidget* panel, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_panel(panel)
        , m_metrics(panel)
        , m_designed(panel->size().expandedTo(panel->minimumSizeHint()))
    {
        auto* col = new QVBoxLayout(this);
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(0);
        col->addWidget(panel);
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        refit();
    }

    void showEvent(QShowEvent* event) override
    {
        QWidget::showEvent(event);
        // Panels finish sizing themselves in showForRx(), which runs after
        // this page is built, so the first honest measurement is here.
        refit();
    }

private:
    void refit()
    {
        if (!m_panel || m_metrics.isEmpty() || m_refitting) return;
        const QSize room = size();
        if (room.isEmpty() || m_designed.isEmpty()) return;

        m_refitting = true;
        // Start from what the designed size asks for, then walk down until the
        // panel's own minimum fits the page. A couple of passes settles it:
        // the graphics do not shrink linearly, because the text between them
        // does not shrink at all.
        qreal factor = std::min({qreal(1.0),
                                 qreal(room.width()) / m_designed.width(),
                                 qreal(room.height()) / m_designed.height()});
        for (int pass = 0; pass < 4; ++pass) {
            m_metrics.apply(factor);
            m_panel->ensurePolished();
            const QSize needs = m_panel->minimumSizeHint();
            if ((needs.width() <= room.width() && needs.height() <= room.height())
                || factor <= CompactMetrics::kMinFactor) {
                break;
            }
            factor *= 0.9;
        }
        m_refitting = false;
    }

    QWidget*       m_panel{nullptr};
    CompactMetrics m_metrics;
    QSize          m_designed;
    bool           m_refitting{false};
};

} // namespace

QWidget* makeStagePage(QWidget* panel)
{
    return new StagePage(panel);
}

// The embedded panels each ship their own window chrome -- a title bar with a
// min/max/close trio, and a legacy band colour from when they were floating
// editors. Inside a host window the trio does nothing, so strip it, the same
// way the Aetherial strip does for its own embedded panels.
//
// `keepTitle` decides whether the name plate goes too. A page showing one panel
// has already named it in the tab, and a second copy of "EQ" above the graph is
// a row of pixels saying nothing; a page stacking two panels keeps both plates,
// because there the titles are what tell them apart.
void tidyEmbeddedPanel(QWidget* panel, bool keepTitle)
{
    if (!panel) return;
    const auto recolour = [](QWidget* w) {
        QString sheet = w->styleSheet();
        if (sheet.contains(QLatin1String("#08121d"))) {
            sheet.replace(QLatin1String("#08121d"),
                          QLatin1String(ModemChrome::Colour::Background));
            // Through the theme, not setStyleSheet(): the replacement is a
            // {{token}} placeholder and nothing else would resolve it.
            AetherSDR::ThemeManager::instance().applyStyleSheet(w, sheet);
        }
    };
    recolour(panel);

    for (QObject* child : panel->children()) {
        // dynamic_cast rather than findChild: EditorFramelessTitleBar has no
        // Q_OBJECT macro.
        if (auto* tb = dynamic_cast<EditorFramelessTitleBar*>(child)) {
            if (keepTitle) {
                tb->setControlsVisible(false);
                recolour(tb);
            } else {
                tb->hide();
            }
            break;
        }
    }
}

} // namespace AetherSDR
