#include "DvkPanel.h"
#include "models/DvkModel.h"
#include "core/DvkWavTransfer.h"
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

namespace AetherSDR {

static const char* kFKeyStyle =
    "QPushButton { background: #1a2a3a; color: #00b4d8; border: 1px solid #203040; "
    "border-radius: 3px; font-size: 10px; font-weight: bold; padding: 0px 2px; }"
    "QPushButton:hover { background: #253545; }"
    "QPushButton:pressed { background: #00b4d8; color: #000; }";

static const char* kNameStyle =
    "QLabel { color: #c8d8e8; font-size: 10px; }";

static const char* kDurStyle =
    "QLabel { color: #6a8090; font-size: 9px; }";

static const char* kBtnStyle =
    "QPushButton { background: #1a2a3a; color: #c8d8e8; border: 1px solid #203040; "
    "border-radius: 3px; padding: 4px 8px; font-size: 11px; font-weight: bold; }"
    "QPushButton:hover { background: #253545; }"
    "QPushButton:checked { background: #00b4d8; color: #000; }";

DvkPanel::DvkPanel(DvkModel* model, QWidget* parent)
    : QWidget(parent), m_model(model)
{
    auto* outerVbox = new QVBoxLayout(this);
    outerVbox->setContentsMargins(4, 4, 4, 4);
    outerVbox->setSpacing(4);

    // Title
    auto* title = new QLabel("Digital Voice Keyer");
    title->setStyleSheet("QLabel { color: #00b4d8; font-weight: bold; font-size: 12px; }");
    outerVbox->addWidget(title);

    // Grid of slots — each row gets equal stretch
    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(2);

    for (int i = 0; i < 12; ++i) {
        int id = i + 1;
        grid->setRowStretch(i, 1);

        // Inset container per row: VBox with content row + progress bar
        auto* rowFrame = new QFrame;
        rowFrame->setStyleSheet(
            "QFrame { background: #0f1520; border: 1px solid #203040; border-radius: 3px; }");
        rowFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        auto* rowVbox = new QVBoxLayout(rowFrame);
        rowVbox->setContentsMargins(3, 2, 3, 1);
        rowVbox->setSpacing(0);

        auto* rowLayout = new QHBoxLayout;
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(4);

        auto* fkeyBtn = new QPushButton(QString("F%1").arg(id));
        fkeyBtn->setStyleSheet(kFKeyStyle);
        fkeyBtn->setFixedWidth(34);
        fkeyBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        fkeyBtn->setToolTip(QString("Play recording %1 on-air (F%1)").arg(id));
        rowLayout->addWidget(fkeyBtn);

        auto* nameLabel = new QLabel(QString("Recording %1").arg(id));
        nameLabel->setStyleSheet("QLabel { color: #505060; font-size: 10px; }");
        rowLayout->addWidget(nameLabel, 1);

        auto* durLabel = new QLabel("Empty");
        durLabel->setStyleSheet(kDurStyle);
        durLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        durLabel->setFixedWidth(40);
        rowLayout->addWidget(durLabel);

        rowVbox->addLayout(rowLayout, 1);

        auto* progressBar = new QProgressBar;
        progressBar->setFixedHeight(3);
        progressBar->setTextVisible(false);
        progressBar->setRange(0, 100);
        progressBar->setValue(0);
        progressBar->setStyleSheet(
            "QProgressBar { background: transparent; border: none; }"
            "QProgressBar::chunk { background: #33aa33; border-radius: 1px; }");
        progressBar->hide();
        rowVbox->addWidget(progressBar);

        grid->addWidget(rowFrame, i, 0);

        m_rowFrames.append(rowFrame);
        m_fkeyBtns.append(fkeyBtn);
        m_nameLabels.append(nameLabel);
        m_durLabels.append(durLabel);
        m_progressBars.append(progressBar);

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
            if (m_model->status() == DvkModel::Playback && m_model->activeId() == id)
                m_model->playbackStop(id);
            else if (durationForSlot(id) > 0)
                m_model->playbackStart(id);
        });
    }

    outerVbox->addLayout(grid, 1);

    // Control buttons: REC | STOP | PLAY | PREV (matches SmartSDR layout)
    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(3);

    m_recBtn = new QPushButton(QString::fromUtf8("\u25CF REC"));
    m_recBtn->setCheckable(true);
    m_recBtn->setStyleSheet(QString(kBtnStyle) +
        "QPushButton:checked { background: #cc3333; color: #fff; }");
    btnRow->addWidget(m_recBtn);

    m_stopBtn = new QPushButton(QString::fromUtf8("\u25A0 STOP"));
    m_stopBtn->setStyleSheet(kBtnStyle);
    btnRow->addWidget(m_stopBtn);

    m_playBtn = new QPushButton(QString::fromUtf8("\u25B6 PLAY"));
    m_playBtn->setCheckable(true);
    m_playBtn->setStyleSheet(QString(kBtnStyle) +
        "QPushButton:checked { background: #33aa33; color: #fff; }");
    btnRow->addWidget(m_playBtn);

    m_prevBtn = new QPushButton(QString::fromUtf8("\u25C0 PREV"));
    m_prevBtn->setCheckable(true);
    m_prevBtn->setStyleSheet(QString(kBtnStyle) +
        "QPushButton:checked { background: #3388cc; color: #fff; }");
    btnRow->addWidget(m_prevBtn);

    outerVbox->addLayout(btnRow);

    // Status label
    m_statusLabel = new QLabel("Status: Idle");
    m_statusLabel->setStyleSheet("QLabel { color: #6a8090; font-size: 10px; }");
    outerVbox->addWidget(m_statusLabel);

    // Wire buttons
    connect(m_recBtn, &QPushButton::clicked, this, [this](bool checked) {
        if (m_selectedSlot < 1) return;
        if (checked) m_model->recStart(m_selectedSlot);
        else         m_model->recStop(m_selectedSlot);
    });

    connect(m_stopBtn, &QPushButton::clicked, this, [this]() {
        int id = m_model->activeId();
        if (id < 0) id = m_selectedSlot;
        if (id < 1) return;
        switch (m_model->status()) {
        case DvkModel::Recording: m_model->recStop(id); break;
        case DvkModel::Playback:  m_model->playbackStop(id); break;
        case DvkModel::Preview:   m_model->previewStop(id); break;
        default: break;
        }
    });

    connect(m_playBtn, &QPushButton::clicked, this, [this](bool checked) {
        if (m_selectedSlot < 1) return;
        if (checked && durationForSlot(m_selectedSlot) > 0)
            m_model->playbackStart(m_selectedSlot);
        else if (checked) { m_playBtn->blockSignals(true); m_playBtn->setChecked(false); m_playBtn->blockSignals(false); }
        else m_model->playbackStop(m_selectedSlot);
    });

    connect(m_prevBtn, &QPushButton::clicked, this, [this](bool checked) {
        if (m_selectedSlot < 1) return;
        if (checked && durationForSlot(m_selectedSlot) > 0)
            m_model->previewStart(m_selectedSlot);
        else if (checked) { m_prevBtn->blockSignals(true); m_prevBtn->setChecked(false); m_prevBtn->blockSignals(false); }
        else m_model->previewStop(m_selectedSlot);
    });

    // Wire model signals
    connect(m_model, &DvkModel::statusChanged, this, &DvkPanel::onStatusChanged);
    connect(m_model, &DvkModel::recordingChanged, this, &DvkPanel::onRecordingChanged);

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
            if (m_model->status() == DvkModel::Playback && m_model->activeId() == id)
                m_model->playbackStop(id);
            else if (durationForSlot(id) > 0)
                m_model->playbackStart(id);
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
        int id = m_model->activeId();
        if (id < 0) return;
        switch (m_model->status()) {
        case DvkModel::Recording: m_model->recStop(id); break;
        case DvkModel::Playback:  m_model->playbackStop(id); break;
        case DvkModel::Preview:   m_model->previewStop(id); break;
        default: break;
        }
    });

    // Elapsed timer for recording/playback/preview progress
    m_elapsedTimer = new QTimer(this);
    m_elapsedTimer->setInterval(100);
    connect(m_elapsedTimer, &QTimer::timeout, this, &DvkPanel::onElapsedTick);

    m_selectedSlot = 1;
    selectSlot(1);
}

void DvkPanel::setShortcutsEnabled(bool enabled)
{
    for (auto* sc : m_shortcuts) sc->setEnabled(enabled);
}

void DvkPanel::selectSlot(int id)
{
    m_selectedSlot = id;
    for (int i = 0; i < m_rowFrames.size(); ++i) {
        bool selected = (i + 1 == id);
        m_rowFrames[i]->setStyleSheet(selected
            ? "QFrame { background: #1a2a4a; border: 1px solid #00b4d8; border-radius: 3px; }"
            : "QFrame { background: #0f1520; border: 1px solid #203040; border-radius: 3px; }");
    }
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

    // Highlight active slot's F-key button
    for (int i = 0; i < m_fkeyBtns.size(); ++i) {
        bool active = (i + 1 == id) && (s == DvkModel::Playback || s == DvkModel::Recording || s == DvkModel::Preview);
        m_fkeyBtns[i]->setStyleSheet(active
            ? "QPushButton { background: #00b4d8; color: #000; border: 1px solid #00b4d8; "
              "border-radius: 3px; font-size: 10px; font-weight: bold; padding: 0px 2px; }"
            : kFKeyStyle);
    }

    bool isActive = (s == DvkModel::Recording || s == DvkModel::Playback || s == DvkModel::Preview);

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

                // Color: red=recording, green=playback, blue=preview
                const char* color = (s == DvkModel::Recording) ? "#cc3333"
                                  : (s == DvkModel::Playback)  ? "#33aa33"
                                  :                               "#3388cc";
                bar->setStyleSheet(QString(
                    "QProgressBar { background: transparent; border: none; }"
                    "QProgressBar::chunk { background: %1; border-radius: 1px; }").arg(color));

                if (totalMs > 0 && s != DvkModel::Recording) {
                    bar->setRange(0, totalMs);
                    bar->setValue(0);
                    bar->show();
                } else {
                    // Recording: indeterminate — show as full bar that stays visible
                    bar->setRange(0, 0);
                    bar->show();
                }
            }

            if (!m_elapsedTimer->isActive())
                m_elapsedTimer->start();
        }

        // Update status label with initial text (tick will update with elapsed)
        onElapsedTick();
    } else {
        // Stop timer and hide progress bars
        m_elapsedTimer->stop();
        m_timerSlotId = -1;
        m_timerStatus = 0;
        m_elapsedMs = 0;
        for (auto* bar : m_progressBars) bar->hide();

        switch (s) {
        case DvkModel::Idle:     m_statusLabel->setText("Status: Idle"); break;
        case DvkModel::Disabled: m_statusLabel->setText("Status: Disabled (SmartSDR+ required)"); break;
        default:                 m_statusLabel->setText("Status: Idle"); break;
        }
    }
}

void DvkPanel::onRecordingChanged(int id)
{
    if (id < 1 || id > 12) return;
    int idx = id - 1;
    const auto& recs = m_model->recordings();
    for (const auto& r : recs) {
        if (r.id == id) {
            m_nameLabels[idx]->setText(r.name);
            m_durLabels[idx]->setText(r.durationMs > 0 ? formatDuration(r.durationMs) : "Empty");
            m_nameLabels[idx]->setStyleSheet(r.durationMs > 0
                ? kNameStyle
                : "QLabel { color: #505060; font-size: 10px; }");
            break;
        }
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
        m_statusLabel->setText(QString("Status: Recording %1 / %2").arg(m_timerSlotId).arg(elapsed));
        break;
    case DvkModel::Playback:
    case DvkModel::Preview: {
        QString label = (s == DvkModel::Playback) ? "Playback" : "Preview";
        if (totalMs > 0)
            m_statusLabel->setText(QString("Status: %1 %2 / %3")
                .arg(label).arg(m_timerSlotId).arg(elapsed));
        else
            m_statusLabel->setText(QString("Status: %1 %2 / %3")
                .arg(label).arg(m_timerSlotId).arg(elapsed));
        break;
    }
    default: break;
    }

    // Update progress bar
    if (m_timerSlotId >= 1 && m_timerSlotId <= 12 && totalMs > 0 && s != DvkModel::Recording) {
        m_progressBars[m_timerSlotId - 1]->setValue(qMin(m_elapsedMs, totalMs));
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
    connect(m_wavTransfer, &DvkWavTransfer::statusChanged,
            m_statusLabel, &QLabel::setText);
    connect(m_wavTransfer, &DvkWavTransfer::finished,
            this, [this](bool success, const QString& msg) {
        m_statusLabel->setText(success ? msg : QString("Export failed: %1").arg(msg));
    });
}

void DvkPanel::showContextMenu(int id, const QPoint& globalPos)
{
    QMenu menu;

    auto* renameAct = menu.addAction("Rename…");
    menu.addSeparator();
    auto* clearAct = menu.addAction("Clear");
    auto* deleteAct = menu.addAction("Delete");
    menu.addSeparator();
    auto* importAct = menu.addAction("Import WAV…");
    auto* exportAct = menu.addAction("Export WAV…");

    int dur = durationForSlot(id);
    bool hasRecording = dur > 0;
    bool notBusy = m_wavTransfer && !m_wavTransfer->isTransferring();
    clearAct->setEnabled(hasRecording);
    deleteAct->setEnabled(hasRecording);
    importAct->setEnabled(notBusy);
    exportAct->setEnabled(notBusy && hasRecording);

    connect(renameAct, &QAction::triggered, this, [this, id]() { startRename(id); });
    connect(clearAct, &QAction::triggered, this, [this, id]() { m_model->clear(id); });
    connect(deleteAct, &QAction::triggered, this, [this, id]() { m_model->remove(id); });

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

    menu.setStyleSheet(
        "QMenu { background: #1a2a3a; color: #c8d8e8; border: 1px solid #203040; }"
        "QMenu::item:selected { background: #00b4d8; color: #000; }"
        "QMenu::item:disabled { color: #505060; }"
        "QMenu::separator { height: 1px; background: #203040; margin: 2px 6px; }");

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
    m_renameEdit->setStyleSheet(
        "QLineEdit { background: #1a2a3a; color: #c8d8e8; border: 1px solid #00b4d8; "
        "border-radius: 2px; font-size: 10px; padding: 0px 2px; }");
    m_renameEdit->setText(label->text());
    m_renameEdit->selectAll();
    m_renameEdit->setMaxLength(40);

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
    QString name = m_renameEdit->text().trimmed();

    // Strip forbidden chars (quotes break protocol parsing)
    name.remove('\'');
    name.remove('"');

    if (!name.isEmpty())
        m_model->setName(m_renameSlot, name);

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
