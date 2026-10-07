#include "DvkPanel.h"
#include "models/DvkModel.h"
#include "core/DvkWavTransfer.h"
#include <QAccessible>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QShortcut>
#include <QPainter>
#include <QFrame>
#include <QMenu>
#include <QMouseEvent>
#include <QFileDialog>
#include <QDir>
#include <QRegularExpression>
#include <QHash>
#include <QStyle>
#include "RoundedMenu.h"
#include "core/ThemeManager.h"
#include "core/TxKeyingMarker.h"

namespace AetherSDR {

namespace {

// The panel on the AetherSDR style guide (RFC #6226), in the title bar's
// vocabulary (#6198): canon surfaces and secondary controls, the title bar's
// active-tab pair for the selected slot, and MOX amber for the one control
// that keys the radio. Widgets carry state as properties (dvkRole, selected,
// empty, live, mode, state, tone) so one tracked sheet styles the panel.
constexpr const char* kPanelStyle =
    "#dvkPanel { background: {{color.canon.raised}}; border-right: 1px solid {{color.canon.line}}; }"

    "QLabel#dvkTitle { color: {{color.canon.ink}}; font-size: 13px; font-weight: bold; background: transparent; }"
    "QLabel#dvkStatus { color: {{color.canon.muted}}; font-size: 11px; background: transparent; }"
    "QLabel#dvkStatus[tone=\"error\"] { color: {{color.accent.danger}}; }"
    "QLabel#dvkStatusDot { border-radius: 4px; background: {{color.titlebar.status.available}}; }"
    "QLabel#dvkStatusDot[state=\"live\"] { background: {{color.canon.cyan}}; }"
    "QLabel#dvkStatusDot[state=\"air\"] { background: {{color.tx.mox.border}}; }"
    "QFrame#dvkHeaderRule { background: {{color.canon.line}}; border: none; }"

    "QFrame[dvkRole=\"slot\"] { background: {{color.canon.nested}}; border: 1px solid {{color.canon.line}};"
    " border-radius: 8px; }"
    "QFrame[dvkRole=\"slot\"]:hover { border-color: {{color.canon.lineHi}}; }"
    "QFrame[dvkRole=\"slot\"][selected=\"true\"] { background: {{color.titlebar.tab.active.background}};"
    " border-color: {{color.titlebar.tab.active.border}}; }"

    "QPushButton[dvkRole=\"fkey\"] { background: {{color.canon.control}}; color: {{color.canon.cyan}};"
    " border: 1px solid {{color.canon.lineHi}}; border-radius: 4px; font-size: 11px; font-weight: bold;"
    " padding: 0px; }"
    "QPushButton[dvkRole=\"fkey\"]:hover { background: {{color.canon.nested}}; color: {{color.canon.aqua}}; }"
    "QPushButton[dvkRole=\"fkey\"]:focus { border-color: {{color.canon.aqua}}; }"
    "QPushButton[dvkRole=\"fkey\"][empty=\"true\"] { color: {{color.canon.muted}}; border-color: {{color.canon.line}}; }"
    "QPushButton[dvkRole=\"fkey\"][live=\"live\"] { background: {{color.titlebar.tab.active.background}};"
    " color: {{color.canon.aqua}}; border-color: {{color.canon.cyan}}; }"
    "QPushButton[dvkRole=\"fkey\"][live=\"air\"] { background: {{color.background.tx}};"
    " color: {{color.tx.mox.text}}; border-color: {{color.tx.mox.border}}; }"

    "QLabel[dvkRole=\"name\"] { color: {{color.canon.inkSoft}}; font-size: 12px; background: transparent; border: none; }"
    "QLabel[dvkRole=\"name\"][empty=\"true\"] { color: {{color.canon.muted}}; }"
    "QLabel[dvkRole=\"length\"] { color: {{color.canon.muted}}; font-size: 11px; background: transparent; border: none; }"

    "QProgressBar[dvkRole=\"progress\"] { background: {{color.canon.line}}; border: none; border-radius: 1px; }"
    "QProgressBar[dvkRole=\"progress\"]::chunk { background: {{color.canon.cyan}}; border-radius: 1px; }"
    "QProgressBar[dvkRole=\"progress\"][mode=\"air\"]::chunk { background: {{color.tx.mox.border}}; }"

    "QLineEdit[dvkRole=\"rename\"] { background: {{color.canon.control}}; color: {{color.canon.ink}};"
    " border: 1px solid {{color.canon.cyan}}; border-radius: 4px; padding: 0px 4px; font-size: 12px;"
    " selection-background-color: {{color.canon.cyan}}; selection-color: {{color.canon.onAccent}}; }"

    "QPushButton[dvkRole=\"transport\"] { background: {{color.canon.control}}; color: {{color.canon.cyan}};"
    " border: 1px solid {{color.canon.lineHi}}; border-radius: 4px; padding: 0px 6px;"
    " font-size: 11px; font-weight: bold; }"
    "QPushButton[dvkRole=\"transport\"]:hover { background: {{color.canon.nested}}; color: {{color.canon.aqua}}; }"
    "QPushButton[dvkRole=\"transport\"]:focus { border-color: {{color.canon.aqua}}; }"
    "QPushButton[dvkRole=\"transport\"]:checked { background: {{color.titlebar.tab.active.background}};"
    " color: {{color.canon.aqua}}; border-color: {{color.canon.cyan}}; }"
    "QPushButton#dvkPlay:checked { background: {{color.background.tx}}; color: {{color.tx.mox.text}};"
    " border-color: {{color.tx.mox.border}}; }"
    "QPushButton[dvkRole=\"transport\"]:disabled { background: transparent; color: {{color.canon.muted}};"
    " border-color: {{color.canon.line}}; }";

// Property changes do not restyle a widget until it is re-polished.
void setStyleProperty(QWidget* widget, const char* name, const QVariant& value)
{
    if (widget->property(name) == value) {
        return;
    }
    widget->setProperty(name, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

}  // namespace

DvkPanel::DvkPanel(DvkModel* model, QWidget* parent)
    : QWidget(parent), m_model(model)
{
    theme::setContainer(this, QStringLiteral("panel/dvk"));
    setObjectName(QStringLiteral("dvkPanel"));
    setAttribute(Qt::WA_StyledBackground);
    setAccessibleName(QStringLiteral("Digital Voice Keyer"));
    auto* outerVbox = new QVBoxLayout(this);
    outerVbox->setContentsMargins(8, 8, 8, 8);
    outerVbox->setSpacing(6);

    // Header, laid out like a title-bar radio tab: name over a status line
    // led by a status dot. The dot is decoration; the words carry the state.
    auto* title = new QLabel("Digital Voice Keyer");
    title->setObjectName(QStringLiteral("dvkTitle"));
    outerVbox->addWidget(title);

    auto* statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->setSpacing(6);
    m_statusDot = new QLabel;
    m_statusDot->setObjectName(QStringLiteral("dvkStatusDot"));
    m_statusDot->setFixedSize(8, 8);
    statusRow->addWidget(m_statusDot, 0, Qt::AlignTop);
    m_statusLabel = new QLabel("Idle");
    m_statusLabel->setObjectName(QStringLiteral("dvkStatus"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    // Two lines are reserved so a wrapped refusal never shifts the slots.
    m_statusLabel->setFixedHeight(2 * m_statusLabel->fontMetrics().lineSpacing() + 2);
    statusRow->addWidget(m_statusLabel, 1);
    outerVbox->addLayout(statusRow);

    auto* headerRule = new QFrame;
    headerRule->setObjectName(QStringLiteral("dvkHeaderRule"));
    headerRule->setFixedHeight(1);
    outerVbox->addWidget(headerRule);

    // Grid of slots — each row gets equal stretch
    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(4);

    for (int i = 0; i < 12; ++i) {
        int id = i + 1;
        grid->setRowStretch(i, 1);

        // Inset container per row: VBox with content row + progress bar
        auto* rowFrame = new QFrame;
        rowFrame->setObjectName(QString("dvkSlot%1").arg(id));
        rowFrame->setProperty("dvkRole", QStringLiteral("slot"));
        rowFrame->setAttribute(Qt::WA_Hover);
        rowFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        auto* rowVbox = new QVBoxLayout(rowFrame);
        rowVbox->setContentsMargins(6, 3, 8, 3);
        rowVbox->setSpacing(2);

        auto* rowLayout = new QHBoxLayout;
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);

        auto* fkeyBtn = new QPushButton(QString("F%1").arg(id));
        fkeyBtn->setObjectName(QString("dvkPlaySlot%1").arg(id));
        fkeyBtn->setProperty("dvkRole", QStringLiteral("fkey"));
        fkeyBtn->setProperty("empty", true);
        fkeyBtn->setFixedSize(34, 22);
        fkeyBtn->setToolTip(QString("Play recording %1 on-air (F%1)").arg(id));
        markTxKeying(fkeyBtn);   // plays the slot on air → keys TX
        rowLayout->addWidget(fkeyBtn);

        auto* nameLabel = new QLabel(QString("Recording %1").arg(id));
        nameLabel->setObjectName(QString("dvkSlotName%1").arg(id));
        nameLabel->setProperty("dvkRole", QStringLiteral("name"));
        nameLabel->setProperty("empty", true);
        rowLayout->addWidget(nameLabel, 1);

        auto* durLabel = new QLabel("Empty");
        durLabel->setObjectName(QString("dvkSlotLength%1").arg(id));
        durLabel->setProperty("dvkRole", QStringLiteral("length"));
        durLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        durLabel->setFixedWidth(40);
        rowLayout->addWidget(durLabel);

        rowVbox->addLayout(rowLayout, 1);

        auto* progressBar = new QProgressBar;
        progressBar->setObjectName(QString("dvkSlotProgress%1").arg(id));
        progressBar->setAccessibleName(QString("Slot %1 progress").arg(id));
        progressBar->setProperty("dvkRole", QStringLiteral("progress"));
        progressBar->setFixedHeight(2);
        progressBar->setTextVisible(false);
        progressBar->setRange(0, 100);
        progressBar->setValue(0);
        // Hidden but laid out, so a row keeps its height when the bar appears.
        QSizePolicy keepSpace = progressBar->sizePolicy();
        keepSpace.setRetainSizeWhenHidden(true);
        progressBar->setSizePolicy(keepSpace);
        progressBar->hide();
        rowVbox->addWidget(progressBar);

        grid->addWidget(rowFrame, i, 0);

        m_rowFrames.append(rowFrame);
        m_fkeyBtns.append(fkeyBtn);
        m_nameLabels.append(nameLabel);
        m_durLabels.append(durLabel);
        m_progressBars.append(progressBar);
        updateSlotAccessibility(id, DvkModel::defaultName(id), 0);

        // Right-click context menu on row
        rowFrame->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(rowFrame, &QFrame::customContextMenuRequested, this, [this, id](const QPoint& pos) {
            selectSlot(id);
            showContextMenu(id, m_rowFrames[id - 1]->mapToGlobal(pos));
        });

        // Left-click row to select, double-click name label to rename
        rowFrame->installEventFilter(this);
        rowFrame->setProperty("slotId", id);
        nameLabel->installEventFilter(this);
        nameLabel->setProperty("slotId", id);

        // F-key button click → playback toggle (only if slot has a recording)
        connect(fkeyBtn, &QPushButton::clicked, this, [this, id]() {
            selectSlot(id);
            togglePlayback(id);
        });
    }

    outerVbox->addLayout(grid, 1);

    // Control buttons: REC | STOP | PLAY | PREV (matches SmartSDR layout)
    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(4);

    // The glyph prefixes would be read aloud, so each button carries a plain name.
    m_recBtn = new QPushButton(QString::fromUtf8("\u25CF REC"));
    m_recBtn->setObjectName(QStringLiteral("dvkRecord"));
    m_recBtn->setAccessibleName(QStringLiteral("Record"));
    m_recBtn->setAccessibleDescription(
        QStringLiteral("Records your transmit audio into the selected slot, up to 10 seconds. Does not transmit."));
    m_recBtn->setCheckable(true);
    btnRow->addWidget(m_recBtn);

    m_stopBtn = new QPushButton(QString::fromUtf8("\u25A0 STOP"));
    m_stopBtn->setObjectName(QStringLiteral("dvkStop"));
    m_stopBtn->setAccessibleName(QStringLiteral("Stop"));
    m_stopBtn->setAccessibleDescription(
        QStringLiteral("Stops the recording, preview or playback in progress."));
    btnRow->addWidget(m_stopBtn);

    m_playBtn = new QPushButton(QString::fromUtf8("\u25B6 PLAY"));
    m_playBtn->setObjectName(QStringLiteral("dvkPlay"));
    m_playBtn->setAccessibleName(QStringLiteral("Play on air"));
    m_playBtn->setAccessibleDescription(
        QStringLiteral("Transmits the selected slot on the transmit slice."));
    m_playBtn->setCheckable(true);
    markTxKeying(m_playBtn);   // plays the selected slot on air → keys TX
    btnRow->addWidget(m_playBtn);

    m_prevBtn = new QPushButton(QString::fromUtf8("\u25C0 PREV"));
    m_prevBtn->setObjectName(QStringLiteral("dvkPreview"));
    m_prevBtn->setAccessibleName(QStringLiteral("Preview"));
    m_prevBtn->setAccessibleDescription(
        QStringLiteral("Plays the selected slot to your speakers without transmitting."));
    m_prevBtn->setCheckable(true);
    btnRow->addWidget(m_prevBtn);

    for (QPushButton* button : {m_recBtn, m_stopBtn, m_playBtn, m_prevBtn}) {
        button->setProperty("dvkRole", QStringLiteral("transport"));
        button->setFixedHeight(24);
        button->setMinimumWidth(0);
    }

    outerVbox->addLayout(btnRow);

    AetherSDR::ThemeManager::instance().applyStyleSheet(this, styleTemplate());

    // Wire buttons
    // A button that cannot start re-syncs to the current status, so it never
    // latches "checked" for an operation that was not sent.
    connect(m_recBtn, &QPushButton::clicked, this, [this](bool checked) {
        if (m_selectedSlot < 1) return;
        if (!checked) {
            m_model->recStop();
        } else if (m_model->canStartOperation()) {
            m_model->recStart(m_selectedSlot);
        } else {
            onStatusChanged(static_cast<int>(m_model->status()), m_model->activeId());
        }
    });

    connect(m_stopBtn, &QPushButton::clicked, this, &DvkPanel::stopActiveOperation);

    connect(m_playBtn, &QPushButton::clicked, this, [this](bool checked) {
        if (m_selectedSlot < 1) return;
        if (!checked) {
            m_model->playbackStop();
        } else if (m_model->canStartOperation() && durationForSlot(m_selectedSlot) > 0) {
            m_model->playbackStart(m_selectedSlot);
        } else {
            onStatusChanged(static_cast<int>(m_model->status()), m_model->activeId());
        }
    });

    connect(m_prevBtn, &QPushButton::clicked, this, [this](bool checked) {
        if (m_selectedSlot < 1) return;
        if (!checked) {
            m_model->previewStop();
        } else if (m_model->canStartOperation() && durationForSlot(m_selectedSlot) > 0) {
            m_model->previewStart(m_selectedSlot);
        } else {
            onStatusChanged(static_cast<int>(m_model->status()), m_model->activeId());
        }
    });

    // Wire model signals
    connect(m_model, &DvkModel::statusChanged, this, &DvkPanel::onStatusChanged);
    connect(m_model, &DvkModel::recordingChanged, this, &DvkPanel::onRecordingChanged);
    connect(m_model, &DvkModel::admissionChanged, this, &DvkPanel::refreshTransport);

    // Surface radio rejections instead of silently toggling buttons.  Without
    // this the REC button latched "checked" on a rejected rec_start. (#3377)
    connect(m_model, &DvkModel::commandFailed, this,
            [this](const QString& verb, int id, uint /*code*/, const QString& message) {
        // Re-drive the buttons from the current (unchanged) status so the
        // failed momentary press is visually released.  This must run *first*:
        // onStatusChanged() rewrites m_statusLabel ("Idle"), so set the
        // failure text afterwards or it gets clobbered before the event loop
        // returns and the user never sees the rejection. (#3377)
        onStatusChanged(static_cast<int>(m_model->status()), m_model->activeId());
        announceStatus(QString("%1 failed (slot %2): %3")
                           .arg(verbLabel(verb)).arg(id).arg(message), true);
    });

    // F1-F12 hotkeys (only play if slot has a recording).  Registered as
    // Qt::ApplicationShortcut on window() and created disabled — MainWindow
    // flips enable state based on the active slice's mode (mutually
    // exclusive with CwxPanel's F1-F12 set) so the keys fire regardless of
    // panel visibility while Qt still sees at most one enabled shortcut
    // per key and never emits activatedAmbiguously. (#2464, #2582)
    for (int i = 0; i < 12; ++i) {
        auto* sc = new QShortcut(QKeySequence(Qt::Key_F1 + i), window());
        sc->setContext(Qt::ApplicationShortcut);
        sc->setEnabled(false);
        m_shortcuts.append(sc);
        connect(sc, &QShortcut::activated, this, [this, i]() {
            int id = i + 1;
            selectSlot(id);
            togglePlayback(id);
        });
    }

    // Escape: cancel rename if active, otherwise stop DVK operation.
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), window());
    esc->setContext(Qt::ApplicationShortcut);
    esc->setEnabled(false);
    m_shortcuts.append(esc);
    connect(esc, &QShortcut::activated, this, [this]() {
        if (m_renameEdit) {
            cancelRename();
            return;
        }
        stopActiveOperation();
    });

    // Elapsed timer for recording/playback/preview progress
    m_elapsedTimer = new QTimer(this);
    m_elapsedTimer->setInterval(100);
    connect(m_elapsedTimer, &QTimer::timeout, this, &DvkPanel::onElapsedTick);

    m_selectedSlot = 1;
    selectSlot(1);
}

QString DvkPanel::verbLabel(const QString& verb)
{
    static const QHash<QString, QString> kLabels{
        {QStringLiteral("rec_start"), QStringLiteral("Record")},
        {QStringLiteral("rec_stop"), QStringLiteral("Stop recording")},
        {QStringLiteral("preview_start"), QStringLiteral("Preview")},
        {QStringLiteral("preview_stop"), QStringLiteral("Stop preview")},
        {QStringLiteral("playback_start"), QStringLiteral("Play")},
        {QStringLiteral("playback_stop"), QStringLiteral("Stop playback")},
        {QStringLiteral("set_name"), QStringLiteral("Rename")},
        {QStringLiteral("clear"), QStringLiteral("Clear")},
    };
    return kLabels.value(verb, verb);
}

QString DvkPanel::styleTemplate()
{
    return QString::fromLatin1(kPanelStyle);
}

void DvkPanel::updateSlotAccessibility(int id, const QString& name, int durationMs)
{
    const int idx = id - 1;
    const QString length = durationMs > 0 ? formatDuration(durationMs) : QStringLiteral("empty");
    m_rowFrames[idx]->setAccessibleName(QString("Slot %1: %2, %3").arg(id).arg(name, length));

    auto* play = m_fkeyBtns[idx];
    const QString playName = QString("Play slot %1: %2").arg(id).arg(name);
    const bool changed = play->accessibleName() != playName;
    play->setAccessibleName(playName);
    play->setAccessibleDescription(durationMs > 0
        ? QString("%1 recording. Transmits on the transmit slice (F%2).").arg(length).arg(id)
        : QStringLiteral("Empty slot, nothing to play."));
    // Announce a real change the operator can see; slot loads and resets on
    // connect and disconnect would otherwise be a burst of a dozen.
    if (changed && play->isVisible()) {
        QAccessibleEvent ev(play, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&ev);
    }
}

void DvkPanel::announceStatus(const QString& text, bool error)
{
    setStyleProperty(m_statusLabel, "tone", error ? QStringLiteral("error") : QString());
    if (m_statusLabel->text() == text) {
        return;
    }
    m_statusLabel->setText(text);
    QAccessibleEvent ev(m_statusLabel, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&ev);
}

void DvkPanel::togglePlayback(int id)
{
    if (m_model->status() == DvkModel::Playback && m_model->activeId() == id) {
        m_model->playbackStop();
    } else if (m_model->canStartOperation() && durationForSlot(id) > 0) {
        // An empty slot can leave the radio keyed, so only a recorded one plays.
        m_model->playbackStart(id);
    }
}

void DvkPanel::stopActiveOperation()
{
    // Before the radio has echoed a start (or before any status at all), stop
    // what we asked for: STOP must never be weaker than the start it follows.
    DvkModel::Status active = m_model->status();
    if (active != DvkModel::Recording && active != DvkModel::Playback
        && active != DvkModel::Preview) {
        active = m_model->pendingOperation();
    }
    switch (active) {
    case DvkModel::Recording: m_model->recStop(); break;
    case DvkModel::Playback:  m_model->playbackStop(); break;
    case DvkModel::Preview:   m_model->previewStop(); break;
    default: break;
    }
}

void DvkPanel::setShortcutsEnabled(bool enabled)
{
    for (auto* sc : m_shortcuts) sc->setEnabled(enabled);
}

void DvkPanel::selectSlot(int id)
{
    m_selectedSlot = id;
    for (int i = 0; i < m_rowFrames.size(); ++i) {
        setStyleProperty(m_rowFrames[i], "selected", i + 1 == id);
    }
    refreshTransport();
}

void DvkPanel::refreshTransport()
{
    // Only what can act now is enabled: a start control while idle (PLAY and
    // PREV need audio in the selected slot), the running one to stop itself,
    // and STOP while something runs.
    const DvkModel::Status s = m_model->status();
    const bool idle = m_model->canStartOperation();
    const bool hasAudio = durationForSlot(m_selectedSlot) > 0;
    m_recBtn->setEnabled(idle || s == DvkModel::Recording);
    m_playBtn->setEnabled((idle && hasAudio) || s == DvkModel::Playback);
    m_prevBtn->setEnabled((idle && hasAudio) || s == DvkModel::Preview);
    m_stopBtn->setEnabled(!idle);
}

int DvkPanel::selectedSlot() const
{
    return m_selectedSlot;
}

void DvkPanel::onStatusChanged(int status, int id)
{
    auto s = static_cast<DvkModel::Status>(status);

    m_recBtn->blockSignals(true);
    m_playBtn->blockSignals(true);
    m_prevBtn->blockSignals(true);

    m_recBtn->setChecked(s == DvkModel::Recording);
    m_playBtn->setChecked(s == DvkModel::Playback);
    m_prevBtn->setChecked(s == DvkModel::Preview);

    m_recBtn->blockSignals(false);
    m_playBtn->blockSignals(false);
    m_prevBtn->blockSignals(false);

    bool isActive = (s == DvkModel::Recording || s == DvkModel::Playback || s == DvkModel::Preview);
    // On air is MOX amber; recording and preview are live but not keyed.
    const QString live = !isActive ? QString()
                       : (s == DvkModel::Playback ? QStringLiteral("air") : QStringLiteral("live"));

    for (int i = 0; i < m_fkeyBtns.size(); ++i) {
        setStyleProperty(m_fkeyBtns[i], "live", i + 1 == id ? live : QString());
    }
    setStyleProperty(m_statusDot, "state", live);
    refreshTransport();

    if (isActive) {
        // Start or restart elapsed timer
        if (m_timerSlotId != id || m_timerStatus != status) {
            m_elapsedMs = 0;
            m_timerSlotId = id;
            m_timerStatus = status;

            // Hide any previous progress bar
            for (auto* bar : m_progressBars) bar->hide();

            // Show and configure progress bar on active slot
            if (id >= 1 && id <= 12) {
                auto* bar = m_progressBars[id - 1];
                int totalMs = durationForSlot(id);

                setStyleProperty(bar, "mode", live);

                if (s == DvkModel::Recording) {
                    // The radio stops recording on its own at the limit.
                    bar->setRange(0, DvkModel::kMaxRecordingMs);
                    bar->setValue(0);
                    bar->show();
                } else if (totalMs > 0) {
                    bar->setRange(0, totalMs);
                    bar->setValue(0);
                    bar->show();
                } else {
                    bar->setRange(0, 0);
                    bar->show();
                }
            }

            if (!m_elapsedTimer->isActive())
                m_elapsedTimer->start();

            // Announce the new operation once; the 100 ms tick that follows
            // only rewrites the elapsed time and stays silent.
            setStyleProperty(m_statusLabel, "tone", QString());
            onElapsedTick();
            QAccessibleEvent ev(m_statusLabel, QAccessible::NameChanged);
            QAccessible::updateAccessibility(&ev);
        } else {
            onElapsedTick();
        }
    } else {
        // Stop timer and hide progress bars
        m_elapsedTimer->stop();
        m_timerSlotId = -1;
        m_timerStatus = 0;
        m_elapsedMs = 0;
        for (auto* bar : m_progressBars) bar->hide();

        switch (s) {
        case DvkModel::Disabled: announceStatus("Disabled · SmartSDR+ required"); break;
        default:                 announceStatus("Idle"); break;
        }
    }
}

void DvkPanel::onRecordingChanged(int id)
{
    if (id < 1 || id > 12) return;
    int idx = id - 1;
    // A slot the model no longer holds (deleted, or the connection reset)
    // shows as the radio's default empty slot, not its last known contents.
    QString name = DvkModel::defaultName(id);
    int durationMs = 0;
    for (const auto& r : m_model->recordings()) {
        if (r.id == id) {
            name = r.name;
            durationMs = r.durationMs;
            break;
        }
    }
    m_nameLabels[idx]->setText(name);
    m_durLabels[idx]->setText(durationMs > 0 ? formatDuration(durationMs) : "Empty");
    updateSlotAccessibility(id, name, durationMs);
    setStyleProperty(m_nameLabels[idx], "empty", durationMs <= 0);
    setStyleProperty(m_fkeyBtns[idx], "empty", durationMs <= 0);
    if (id == m_selectedSlot) {
        refreshTransport();
    }
}

void DvkPanel::onElapsedTick()
{
    m_elapsedMs += 100;

    auto s = static_cast<DvkModel::Status>(m_timerStatus);
    QString elapsed = formatDuration(m_elapsedMs);
    int totalMs = durationForSlot(m_timerSlotId);

    switch (s) {
    case DvkModel::Recording:
        m_statusLabel->setText(QString("Recording · slot %1 · %2 of %3")
            .arg(m_timerSlotId).arg(elapsed, formatDuration(DvkModel::kMaxRecordingMs)));
        break;
    case DvkModel::Playback:
    case DvkModel::Preview: {
        const QString label = (s == DvkModel::Playback) ? "On air" : "Previewing";
        const QString of = totalMs > 0 ? QString(" of %1").arg(formatDuration(totalMs)) : QString();
        m_statusLabel->setText(QString("%1 · slot %2 · %3%4")
            .arg(label).arg(m_timerSlotId).arg(elapsed, of));
        break;
    }
    default: break;
    }

    // Update progress bar
    if (m_timerSlotId >= 1 && m_timerSlotId <= 12) {
        if (s == DvkModel::Recording) {
            m_progressBars[m_timerSlotId - 1]->setValue(
                qMin(m_elapsedMs, DvkModel::kMaxRecordingMs));
        } else if (totalMs > 0) {
            m_progressBars[m_timerSlotId - 1]->setValue(qMin(m_elapsedMs, totalMs));
        }
    }
}

int DvkPanel::durationForSlot(int id) const
{
    for (const auto& r : m_model->recordings())
        if (r.id == id) return r.durationMs;
    return 0;
}

QString DvkPanel::formatDuration(int ms)
{
    int secs = ms / 1000;
    int frac = (ms % 1000) / 100;
    return QString("%1.%2s").arg(secs).arg(frac);
}

// ── Event filter (double-click name label → rename) ────────────────────────

bool DvkPanel::eventFilter(QObject* obj, QEvent* event)
{
    int id = obj->property("slotId").toInt();
    if (id < 1 || id > 12)
        return QWidget::eventFilter(obj, event);

    if (event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            selectSlot(id);
            return false;  // don't consume — let double-click still work
        }
    }

    if (event->type() == QEvent::MouseButtonDblClick) {
        // Only name labels trigger rename (not the row frame itself)
        if (qobject_cast<QLabel*>(obj)) {
            selectSlot(id);
            startRename(id);
            return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}

// ── Context menu ───────────────────────────────────────────────────────────

void DvkPanel::setWavTransfer(DvkWavTransfer* transfer)
{
    m_wavTransfer = transfer;
    connect(m_wavTransfer, &DvkWavTransfer::statusChanged, this, [this](const QString& text) {
        setStyleProperty(m_statusLabel, "tone", QString());
        m_statusLabel->setText(text);
    });
    connect(m_wavTransfer, &DvkWavTransfer::finished,
            this, [this](bool success, const QString& msg) {
        announceStatus(success ? msg : QString("Transfer failed: %1").arg(msg), !success);
    });
}

void DvkPanel::showContextMenu(int id, const QPoint& globalPos)
{
    QMenu menu;

    auto* renameAct = menu.addAction("Rename…");
    menu.addSeparator();
    auto* clearAct = menu.addAction("Clear");
    menu.addSeparator();
    auto* importAct = menu.addAction("Import WAV…");
    auto* exportAct = menu.addAction("Export WAV…");

    int dur = durationForSlot(id);
    bool hasRecording = dur > 0;
    bool notBusy = m_wavTransfer && !m_wavTransfer->isBusy();
    // Clearing or loading a slot mid-operation can leave the DVK inconsistent.
    const bool idle = m_model->canStartOperation();
    clearAct->setEnabled(hasRecording && idle && notBusy);
    importAct->setEnabled(notBusy && idle);
    exportAct->setEnabled(notBusy && hasRecording);

    connect(renameAct, &QAction::triggered, this, [this, id]() { startRename(id); });
    connect(clearAct, &QAction::triggered, this, [this, id]() { m_model->clear(id); });

    connect(importAct, &QAction::triggered, this, [this, id]() {
        QString path = QFileDialog::getOpenFileName(this,
            "Import WAV to DVK Slot",
            QDir::homePath(),
            "WAV Files (*.wav)");
        if (path.isEmpty()) return;

        m_wavTransfer->upload(id, path);
    });

    connect(exportAct, &QAction::triggered, this, [this, id]() {
        QString name;
        for (const auto& r : m_model->recordings()) {
            if (r.id == id) { name = r.name; break; }
        }
        if (name.isEmpty()) name = QString("Recording_%1").arg(id);
        name.replace(QRegularExpression("[^\\w\\s-]"), "_");

        QString path = QFileDialog::getSaveFileName(this,
            "Export DVK Recording",
            QDir::homePath() + "/" + name + ".wav",
            "WAV Files (*.wav)");
        if (path.isEmpty()) return;

        m_wavTransfer->download(id, path);
    });

    // The title bar's menu, so every menu in the canon reads the same. Its
    // shared rules give disabled items no colour of their own, and Clear and
    // Export are routinely disabled here.
    AetherSDR::ThemeManager::instance().applyStyleSheet(&menu,
        kRoundedMenuRules + QStringLiteral("QMenu::item:disabled { color: {{color.canon.muted}}; }"));
    roundMenuTree(&menu);

    menu.exec(globalPos);
}

// ── Inline rename ──────────────────────────────────────────────────────────

void DvkPanel::startRename(int id)
{
    if (m_renameEdit) cancelRename();

    int idx = id - 1;
    auto* label = m_nameLabels[idx];
    auto* rowLayout = qobject_cast<QHBoxLayout*>(
        m_rowFrames[idx]->layout()->itemAt(0)->layout());
    if (!rowLayout) return;

    m_renameSlot = id;
    m_renameEdit = new QLineEdit;
    m_renameEdit->setProperty("dvkRole", QStringLiteral("rename"));
    m_renameEdit->setFixedHeight(22);
    m_renameEdit->setText(label->text());
    m_renameEdit->selectAll();
    m_renameEdit->setMaxLength(DvkModel::kMaxNameBytes);
    m_renameEdit->setAccessibleName(QString("Slot %1 name").arg(id));
    m_renameEdit->setAccessibleDescription(QStringLiteral(
        "Up to 61 UTF-8 bytes (61 plain letters, fewer with accents or symbols). "
        "Quotes and | are removed. Enter saves, Escape cancels."));

    // Swap label out, edit in (same layout position)
    int labelIdx = rowLayout->indexOf(label);
    label->hide();
    rowLayout->insertWidget(labelIdx, m_renameEdit, 1);
    m_renameEdit->setFocus();

    connect(m_renameEdit, &QLineEdit::returnPressed, this, &DvkPanel::commitRename);
    connect(m_renameEdit, &QLineEdit::editingFinished, this, &DvkPanel::commitRename);
}

void DvkPanel::commitRename()
{
    if (!m_renameEdit || m_renameSlot < 1) return;

    int idx = m_renameSlot - 1;
    m_model->setName(m_renameSlot, m_renameEdit->text());

    m_nameLabels[idx]->show();
    m_renameEdit->deleteLater();
    m_renameEdit = nullptr;
    m_renameSlot = -1;
}

void DvkPanel::cancelRename()
{
    if (!m_renameEdit || m_renameSlot < 1) return;

    int idx = m_renameSlot - 1;
    m_nameLabels[idx]->show();
    m_renameEdit->deleteLater();
    m_renameEdit = nullptr;
    m_renameSlot = -1;
}

} // namespace AetherSDR
