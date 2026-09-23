// Regression harness for #5915 (#4363, #4990): leaving minimal mode after a
// launch in it crashed in the Intel D3D11 driver, because the spectrum's first
// QRhi frame was drawn synchronously by QRhiWidget::resizeEvent in the middle
// of the exit's resize cascade.
//
// Two halves, both offscreen and GPU-free:
//
//  1. The Qt behaviour the fix relies on, on real widgets: a spectrum-shaped
//     child that has never been shown carries no explicit hide(); held hidden
//     with retainSizeWhenHidden it sees NO resize while its window resizes
//     around it, keeps its layout slot, and on the deferred show gets its
//     first resize at the settled size. A control run without the hold shows
//     the same cascade does reach it mid-flight, so the check has teeth.
//
//  2. MainWindow is intentionally not linked into this target (see
//     connection_panel_size_test), so the ordering inside
//     MainWindow::toggleMinimalMode's exit branch is pinned from the source:
//     spectra held before the splitter is shown and before the first resize,
//     released only in a deferred turn after the re-anchor, with rendering
//     resumed before they are shown, and that turn queued before the canvas
//     re-entry.

#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QResizeEvent>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <string>

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-62s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok)
        ++g_failed;
}

void report(const std::string& name, bool ok, const std::string& detail = {})
{
    report(name.c_str(), ok, detail);
}

std::string sizeText(const QSize& s)
{
    return std::to_string(s.width()) + "x" + std::to_string(s.height());
}

// Stands in for SpectrumWidget: QRhiWidget renders inside resizeEvent, so a
// resize delivered to it IS a frame drawn at that size.
class SpectrumProbe : public QWidget {
public:
    using QWidget::QWidget;
    int resizes = 0;
    QSize firstSize;
protected:
    void resizeEvent(QResizeEvent* e) override
    {
        if (resizes++ == 0)
            firstSize = e->size();
        QWidget::resizeEvent(e);
    }
};

// The shape that matters: window > splitter > [pan applet: title + spectrum,
// applet panel]. Built and put into minimal mode before the first show, as
// MainWindow does when MinimalModeEnabled was saved True.
struct Shell {
    QWidget window;
    QSplitter* splitter = nullptr;
    QWidget* pan = nullptr;
    QLabel* panTitle = nullptr;
    SpectrumProbe* spectrum = nullptr;
    QWidget* applets = nullptr;

    Shell()
    {
        auto* outer = new QVBoxLayout(&window);
        outer->setContentsMargins(0, 0, 0, 0);
        splitter = new QSplitter(Qt::Horizontal, &window);
        pan = new QWidget;
        auto* panLayout = new QVBoxLayout(pan);
        panLayout->setContentsMargins(0, 0, 0, 0);
        panTitle = new QLabel(QStringLiteral("pan A"));
        panLayout->addWidget(panTitle);
        spectrum = new SpectrumProbe;
        spectrum->setMinimumSize(100, 100);
        panLayout->addWidget(spectrum, 1);
        applets = new QWidget;
        applets->setMinimumWidth(240);
        splitter->addWidget(pan);
        splitter->addWidget(applets);
        outer->addWidget(splitter);

        splitter->hide();          // launched in minimal mode
        window.resize(260, 700);
        window.show();
        QApplication::processEvents();
    }

    // The exit's geometry steps, each a separate relayout, with the event
    // loop run between them (stricter than production, which runs them back
    // to back, so a probe that stays quiet here stays quiet there).
    void cascade()
    {
        const QSize steps[] = {{700, 700}, {1024, 720}, {1400, 900}, {1428, 1104}};
        for (const QSize& s : steps) {
            window.resize(s);
            QApplication::processEvents();
        }
    }
};

void checkQtPremises()
{
    // --- with the hold (the fix) ---
    {
        Shell sh;
        report("never-shown spectrum is not an explicit hide (not skipped)",
               !sh.spectrum->testAttribute(Qt::WA_WState_ExplicitShowHide));
        report("no frame drawn while launched minimal", sh.spectrum->resizes == 0);

        QSizePolicy sp = sh.spectrum->sizePolicy();
        sp.setRetainSizeWhenHidden(true);
        sh.spectrum->setSizePolicy(sp);
        sh.spectrum->hide();
        sh.splitter->show();
        sh.cascade();

        report("held spectrum sees no resize during the exit cascade",
               sh.spectrum->resizes == 0,
               "resizes=" + std::to_string(sh.spectrum->resizes));
        report("applet panel is visible during the cascade (no empty window)",
               sh.applets->isVisible() && sh.pan->isVisible());
        const int titleY = sh.panTitle->y();
        const QSize slot = sh.spectrum->geometry().size();

        QTimer::singleShot(0, &sh.window, [&sh] {
            QSizePolicy p = sh.spectrum->sizePolicy();
            p.setRetainSizeWhenHidden(false);
            sh.spectrum->setSizePolicy(p);
            sh.spectrum->show();
        });
        QApplication::processEvents();

        report("deferred show draws the first frame at the settled size",
               sh.spectrum->resizes >= 1 && sh.spectrum->firstSize == sh.spectrum->size(),
               "first=" + sizeText(sh.spectrum->firstSize)
                   + " now=" + sizeText(sh.spectrum->size()));
        report("retainSizeWhenHidden kept the spectrum's slot",
               slot == sh.spectrum->size() && titleY == sh.panTitle->y(),
               "slot=" + sizeText(slot) + " now=" + sizeText(sh.spectrum->size()));
    }

    // --- control: the old order, splitter and spectrum shown first ---
    {
        Shell sh;
        sh.splitter->show();
        QApplication::processEvents();
        const QSize before = sh.spectrum->size();
        sh.cascade();
        report("control: without the hold the cascade reaches the spectrum",
               sh.spectrum->resizes > 1 && sh.spectrum->firstSize == before
                   && before != sh.spectrum->size(),
               "resizes=" + std::to_string(sh.spectrum->resizes)
                   + " first=" + sizeText(sh.spectrum->firstSize)
                   + " final=" + sizeText(sh.spectrum->size()));
    }
}

// Comments are not code. A plain indexOf accepts a DISABLED statement: comment
// out the production `sw->hide();` and a raw text search still finds it, so the
// order checks below pass while the gate no longer holds anything back. Review
// of #5916 (Ozy311) demonstrated exactly that, with mutated source fixtures.
QByteArray codeOnly(const QByteArray& src)
{
    QByteArray out;
    out.reserve(src.size());
    enum { Code, Line, Block } st = Code;
    for (qsizetype i = 0; i < src.size(); ++i) {
        const char c = src[i];
        const char n = (i + 1 < src.size()) ? src[i + 1] : '\0';
        if (st == Code && c == '/' && n == '/') { st = Line; ++i; continue; }
        if (st == Code && c == '/' && n == '*') { st = Block; ++i; continue; }
        if (st == Line && c == '\n') { st = Code; out.append(c); continue; }
        if (st == Block && c == '*' && n == '/') { st = Code; ++i; continue; }
        if (st == Code)
            out.append(c);
    }
    return out;
}

// The order the exit must keep, as a predicate, so the same rules can be run
// against deliberately broken copies of the source below.
bool exitOrderHolds(const QByteArray& branchSource, std::string* why = nullptr)
{
    const QByteArray b = codeOnly(branchSource);
    auto at = [&b](const char* needle) { return b.indexOf(needle); };
    const qsizetype hold = at("sw->hide();");
    const qsizetype splitterShow = at("m_splitter->show();");
    const qsizetype firstResize = at("setFixedWidth(QWIDGETSIZE_MAX);");
    const qsizetype reanchor = at("reanchorCustomFrameGeometry(geom);");
    const qsizetype deferred = at("QTimer::singleShot(0, this, [this, heldSpectra]");
    const qsizetype release = at("sw->show();");
    const qsizetype resume = at("setUpdatesEnabled(true)");
    const qsizetype canvas = at("toggleWorkspaceCanvas(true)");
    const qsizetype explicitHide = at("WA_WState_ExplicitShowHide");

    struct Rule { const char* name; bool ok; };
    const Rule rules[] = {
        {"spectra held before the splitter is shown", hold >= 0 && splitterShow > hold},
        {"held before the first geometry step", firstResize > splitterShow},
        {"the whole splitter is not hidden (empty-window flash)", at("m_splitter->hide()") < 0},
        {"released in a deferred turn after the re-anchor",
         reanchor > firstResize && deferred > reanchor && release > deferred},
        {"rendering resumes inside that deferred turn", resume > deferred},
        // QRhiWidget draws its first frame from the resize the show delivers; a
        // render-to-texture widget shown with updates still off drops that frame
        // and does not repaint on a later update() (the spectrum stayed blank on
        // Linux until something grabbed it). Offscreen has no QRhi, so the order
        // is pinned here rather than exercised.
        {"rendering resumes BEFORE the held spectra are shown", release > resume},
        {"the deferred show is queued before the canvas re-entry", canvas > release},
        {"a never-shown spectrum is not treated as explicitly hidden",
         explicitHide >= 0 && explicitHide < hold},
    };
    for (const Rule& r : rules) {
        if (!r.ok) {
            if (why)
                *why = r.name;
            return false;
        }
    }
    return true;
}

void checkMainWindowOrder()
{
    QFile source(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow.cpp"));
    report("can inspect MainWindow.cpp", source.open(QIODevice::ReadOnly));
    const QByteArray text = source.readAll();

    const qsizetype fn = text.indexOf("void MainWindow::toggleMinimalMode(bool on)");
    const qsizetype exitStart = text.indexOf("// Sync the View-menu action", fn);
    const qsizetype exitEnd = text.indexOf("s.setValue(\"MinimalModeEnabled\"", exitStart);
    report("toggleMinimalMode exit branch located",
           fn >= 0 && exitStart > fn && exitEnd > exitStart);
    const QByteArray exitBranch = text.mid(exitStart, exitEnd - exitStart);

    std::string why;
    report("the exit branch keeps the order the fix depends on",
           exitOrderHolds(exitBranch, &why), why.empty() ? "" : "first broken rule: " + why);

    // Mutation: the checks must REJECT a branch whose production statements have
    // been commented out. Without codeOnly() every one of these still passed.
    struct Mutant { const char* what; QByteArray from, to; };
    const Mutant mutants[] = {
        {"sw->hide() commented out", "sw->hide();", "// sw->hide();"},
        {"sw->show() commented out", "sw->show();", "// sw->show();"},
        {"the deferred turn commented out",
         "QTimer::singleShot(0, this, [this, heldSpectra]",
         "// QTimer::singleShot(0, this, [this, heldSpectra]"},
    };
    for (const Mutant& m : mutants) {
        QByteArray broken = exitBranch;
        const qsizetype where = broken.indexOf(m.from);
        if (where < 0) {
            report(std::string("mutation setup: ") + m.what, false, "statement not found");
            continue;
        }
        broken.replace(where, m.from.size(), m.to);
        std::string ignored;
        report(std::string("rejected: ") + m.what, !exitOrderHolds(broken, &ignored));
    }

    // And the comment stripper itself, since everything above now rests on it.
    report("codeOnly drops // and /* */ but keeps the code",
           codeOnly("a(); // b();\nc(); /* d(); */ e();").simplified() == QByteArray("a(); c(); e();"),
           codeOnly("a(); // b();\nc(); /* d(); */ e();").simplified().toStdString());
}

} // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    checkQtPremises();
    checkMainWindowOrder();

    std::printf("\n%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
