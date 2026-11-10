#include "Nr2WisdomDialog.h"
#include "CanonIndicators.h"
#include "core/ThemeManager.h"

#include <QAccessible>
#include <QBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>

namespace AetherSDR {

namespace {

constexpr const char* kTitle = "FFTW Wisdom";
constexpr const char* kClosesWhenDone =
    "This window will automatically close when wisdom generation is complete.";
constexpr const char* kCancelPending =
    "Cancel requested — waiting for the current FFT plan to finish.";
// The planner reports nothing for the length of one FFTW_PATIENT plan, which
// can be minutes; after this long the reassurance line says it is still alive.
constexpr qint64 kQuietMs = 10000;

constexpr const char* kWisdomStyle = R"(
QWidget {
    color: {{color.canon.inkSoft}};
    background: transparent;
}
QLabel#nr2WisdomStatusLabel {
    color: {{color.canon.ink}};
}
QProgressBar {
    text-align: center;
    font-size: 13px;
    font-weight: bold;
    color: {{color.canon.ink}};
    background: {{color.canon.nested}};
    border: 1px solid {{color.canon.line}};
    border-radius: 4px;
}
QProgressBar::chunk {
    background: {{color.brand.gradient}};
    border-radius: 3px;
}
QPushButton {
    color: {{color.canon.cyan}};
    background: {{color.canon.control}};
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 4px;
    padding: 5px 12px;
    font-weight: 600;
}
QPushButton:hover {
    background: {{color.canon.nested}};
    color: {{color.canon.aqua}};
}
QPushButton:focus {
    border-color: {{color.canon.aqua}};
}
QPushButton:disabled {
    color: {{color.canon.muted}};
    border-color: {{color.canon.line}};
    background: transparent;
}
)";

} // namespace

Nr2WisdomDialog::Nr2WisdomDialog(QWidget* parent)
    : CanonWindow(QString::fromUtf8(kTitle), parent)
{
    setObjectName(QStringLiteral("nr2WisdomDialog"));
    theme::setContainer(this, QStringLiteral("canon/nr2Wisdom"));
    // Modeless: generation can take minutes, and the operator keeps using the
    // radio meanwhile. A tool window floats above the main window without
    // its own taskbar entry, and appearing does not take the focus.
    setWindowModality(Qt::NonModal);
    setWindowFlag(Qt::Tool, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
#ifdef Q_OS_MACOS
    // Qt maps Qt::Tool to an NSPanel, which AppKit hides whenever the
    // application deactivates. QWidgetPrivate::create() forwards this to the
    // QWindow property _q_macAlwaysShowToolWindow, which sets
    // NSWindow.hidesOnDeactivate; it is read when the window is created, so it
    // must stay ahead of the first show() or winId().
    setAttribute(Qt::WA_MacAlwaysShowToolWindow, true);
#endif
    setMinimumWidth(500);
    applyCanonSheet(bodyWidget(), [] { return QString::fromLatin1(kWisdomStyle); });

    auto* body = new QVBoxLayout(bodyWidget());
    body->setContentsMargins(14, 12, 14, 14);
    body->setSpacing(10);
    body->addWidget(makeCanonHeader(QString::fromUtf8(kTitle)));

    m_status = new QLabel(QStringLiteral("Optimizing FFT plans for NR2...\n\n%1")
                              .arg(QString::fromUtf8(kClosesWhenDone)),
                          this);
    m_status->setObjectName(QStringLiteral("nr2WisdomStatusLabel"));
    m_status->setWordWrap(true);
    body->addWidget(m_status);

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("nr2WisdomProgressBar"));
    m_progress->setAccessibleName(QStringLiteral("FFTW wisdom generation progress"));
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    body->addWidget(m_progress);

    auto* activityRow = new QHBoxLayout;
    activityRow->setContentsMargins(0, 0, 0, 0);
    m_activity = new QLabel(QStringLiteral("Working"), this);
    m_activity->setObjectName(QStringLiteral("nr2WisdomActivityLabel"));
    m_activity->setAccessibleName(QStringLiteral("FFTW wisdom generation activity"));
    activityRow->addWidget(m_activity);
    activityRow->addStretch();
    m_elapsed = new QLabel(QStringLiteral("Elapsed: 0:00"), this);
    m_elapsed->setObjectName(QStringLiteral("nr2WisdomElapsedTimeLabel"));
    m_elapsed->setAccessibleName(QStringLiteral("Elapsed time"));
    activityRow->addWidget(m_elapsed);
    body->addLayout(activityRow);

    m_reassurance = new QLabel(
        QStringLiteral("Still working — FFTW planning can take several minutes on some systems."),
        this);
    m_reassurance->setObjectName(QStringLiteral("nr2WisdomReassuranceLabel"));
    m_reassurance->setAccessibleName(QStringLiteral("FFTW wisdom generation reassurance"));
    m_reassurance->setWordWrap(true);
    // Reserve the row while hidden: the line toggles on every quiet interval,
    // and without the reservation each toggle would resize the window (#4728).
    QSizePolicy reassurancePolicy = m_reassurance->sizePolicy();
    reassurancePolicy.setRetainSizeWhenHidden(true);
    m_reassurance->setSizePolicy(reassurancePolicy);
    m_reassurance->hide();
    body->addWidget(m_reassurance);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    m_cancel = new QPushButton(QStringLiteral("Cancel"), this);
    m_cancel->setObjectName(QStringLiteral("nr2WisdomCancelButton"));
    m_cancel->setAutoDefault(false);
    m_cancel->setDefault(false);
    buttonRow->addWidget(m_cancel);
    body->addLayout(buttonRow);

    connect(m_cancel, &QPushButton::clicked, this, &Nr2WisdomDialog::requestCancel);
    connect(this, &QDialog::rejected, this, &Nr2WisdomDialog::requestCancel);

    m_activityTimer = new QTimer(this);
    m_activityTimer->setInterval(500);
    connect(m_activityTimer, &QTimer::timeout, this, &Nr2WisdomDialog::tickActivity);
    m_elapsedTimer.start();
    m_lastActivity.start();
    m_activityTimer->start();
}

void Nr2WisdomDialog::setStepDescription(const QString& description)
{
    if (m_cancelRequested) {
        return;
    }
    noteActivity();
    m_status->setText(QStringLiteral("%1\n\n%2").arg(description, QString::fromUtf8(kClosesWhenDone)));
}

void Nr2WisdomDialog::setProgress(int percent)
{
    if (m_cancelRequested) {
        return;
    }
    noteActivity();
    m_progress->setValue(percent);
}

void Nr2WisdomDialog::showResult(Outcome outcome)
{
    m_activityTimer->stop();
    m_reassurance->hide();
    m_cancel->setEnabled(false);
    m_progress->setRange(0, 100);
    m_progress->setValue(outcome == Outcome::Complete ? 100 : 0);
    switch (outcome) {
    case Outcome::Complete:
        m_activity->setText(QStringLiteral("Complete"));
        m_status->setText(QStringLiteral("Wisdom generation complete!"));
        break;
    case Outcome::Canceled:
        m_activity->setText(QStringLiteral("Stopped"));
        m_status->setText(QStringLiteral("Wisdom generation canceled. Audio was left unchanged."));
        break;
    case Outcome::Failed:
        m_activity->setText(QStringLiteral("Stopped"));
        m_status->setText(QStringLiteral("Wisdom generation failed. Audio was left unchanged."));
        break;
    }
    announce(m_status);
}

void Nr2WisdomDialog::requestCancel()
{
    if (m_cancelRequested) {
        return;
    }
    m_cancelRequested = true;
    // The activity line is left to tickActivity(), which switches it to an
    // animated "Canceling..." that keeps showing the app is alive while the
    // plan in flight finishes.
    m_reassurance->setText(QString::fromUtf8(kCancelPending));
    m_reassurance->show();
    announce(m_reassurance);
    m_cancel->setEnabled(false);
    m_progress->setRange(0, 0);
    m_status->setText(QStringLiteral(
        "Canceling FFTW wisdom generation...\n\n"
        "Audio will continue unchanged. This may take a moment while the current FFT plan finishes."));
    emit cancelRequested();
}

void Nr2WisdomDialog::tickActivity()
{
    const qint64 elapsedMs = m_elapsedTimer.elapsed();
    const qint64 elapsedSeconds = elapsedMs / 1000;
    m_elapsed->setText(QStringLiteral("Elapsed: %1:%2")
                           .arg(elapsedSeconds / 60)
                           .arg(elapsedSeconds % 60, 2, 10, QLatin1Char('0')));
    const int dotCount = static_cast<int>((elapsedMs / 500) % 4);
    m_activity->setText(QStringLiteral("%1%2").arg(
        m_cancelRequested ? QStringLiteral("Canceling") : QStringLiteral("Working"),
        QString(dotCount, QLatin1Char('.'))));
    if (m_cancelRequested) {
        return;
    }
    const bool quiet = m_lastActivity.elapsed() >= kQuietMs;
    // isHidden(), not isVisible(): the line's own state, whether or not the
    // window is on screen.
    if (quiet && m_reassurance->isHidden()) {
        m_reassurance->show();
        announce(m_reassurance);
    } else if (!quiet) {
        m_reassurance->hide();
    }
}

void Nr2WisdomDialog::noteActivity()
{
    m_lastActivity.restart();
    m_reassurance->hide();
}

void Nr2WisdomDialog::announce(QLabel* label)
{
    label->setAccessibleName(label->text());
    QAccessibleEvent event(label, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&event);
}

} // namespace AetherSDR
