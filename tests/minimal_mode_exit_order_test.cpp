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
    auto at = [&exitBranch](const char* needle) { return exitBranch.indexOf(needle); };

    const qsizetype hold = at("sw->hide();");
    const qsizetype splitterShow = at("m_splitter->show();");
    const qsizetype firstResize = at("setFixedWidth(QWIDGETSIZE_MAX);");
    const qsizetype reanchor = at("reanchorCustomFrameGeometry(geom);");
    const qsizetype deferred = at("QTimer::singleShot(0, this, [this, heldSpectra]");
    const qsizetype release = at("sw->show();");
    const qsizetype resume = at("setUpdatesEnabled(true)");
    const qsizetype canvas = at("toggleWorkspaceCanvas(true)");

    report("spectra are held before the splitter is shown",
           hold >= 0 && splitterShow > hold);
    report("...and before the first geometry step of the exit",
           firstResize > splitterShow);
    report("the whole splitter is not hidden on exit (empty-window flash)",
           at("m_splitter->hide()") < 0);
    report("spectra are released in a deferred turn after the re-anchor",
           reanchor > firstResize && deferred > reanchor && release > deferred);
    report("rendering resumes only inside that deferred turn",
           resume > deferred);
    // QRhiWidget draws its first frame from the resize the show delivers; a
    // render-to-texture widget shown with updates still off drops that frame
    // and does not repaint on a later update() (the spectrum stayed blank on
    // Linux until grabbed). Offscreen has no QRhi, so this is pinned here.
    report("rendering resumes before the held spectra are shown",
           resume > deferred && release > resume);
    report("the deferred show is queued before the canvas re-entry",
           canvas > release);
    report("a never-shown spectrum is not treated as explicitly hidden",
           at("WA_WState_ExplicitShowHide") >= 0 && at("WA_WState_ExplicitShowHide") < hold);
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
