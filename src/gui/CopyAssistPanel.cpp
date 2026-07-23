#include "CopyAssistPanel.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>

namespace AetherSDR {

CopyAssistPanel::CopyAssistPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("CopyAssistPanel"));
    setAccessibleName(tr("Copy Assist"));
    setAccessibleDescription(tr("Speech-to-text transcript of received voice, "
                                "color-coded by recognition confidence."));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(4);

    // --- Controls row -------------------------------------------------------
    auto* controls = new QHBoxLayout;

    // A checkable button (not a checkbox) whose label shows the state
    // (Enabled/Disabled); the app layer gives it the applet-toggle style so the
    // enabled state reads as a distinct colour.
    m_enable = new QPushButton(tr("Disabled"), this);
    m_enable->setCheckable(true);
    m_enable->setAccessibleName(tr("Enable Copy Assist"));
    m_enable->setToolTip(tr("Transcribe received voice to text"));
    connect(m_enable, &QPushButton::toggled, this, [this](bool on) {
        m_enable->setText(on ? tr("Enabled") : tr("Disabled"));
        emit enableToggled(on);
    });
    controls->addWidget(m_enable);

    // Settings (⚙) button — opens the modeless Copy Assist settings dialog
    // (model + compute-device pickers, and more). Modeled on the band-stack
    // panel's gear button; the controller applies the themed style so it sizes
    // to match the other controls in this row.
    m_settings = new QPushButton(QString::fromUtf8("⚙"), this);
    m_settings->setAccessibleName(tr("Copy Assist settings"));
    m_settings->setToolTip(tr("Model, compute device, and other options"));
    connect(m_settings, &QPushButton::clicked, this, &CopyAssistPanel::settingsRequested);
    controls->addWidget(m_settings);

    controls->addSpacing(40); // gap between the settings button and the sliders

    // Compact inline tuning sliders on the same row (mirrors the CW decode bar).
    m_buffer = addSliderInline(controls, tr("Buffer:"), tr("Decode buffer seconds"),
                               1, 20, 20, &m_bufferValue);
    // Left-align so the value hugs the slider instead of sitting at the far right
    // of its fixed-width box.
    m_bufferValue->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_bufferValue->setText(tr("%1 s").arg(m_buffer->value()));
    connect(m_buffer, &QSlider::valueChanged, this, [this](int s) {
        m_bufferValue->setText(tr("%1 s").arg(s));
        emit bufferMsChanged(s * 1000);
    });

    controls->addSpacing(40); // gap between the tuning controls
    m_sensitivity = addSliderInline(controls, tr("Sens:"), tr("VAD sensitivity percent"),
                                    1, 100, 80, &m_sensitivityValue);
    m_sensitivityValue->setAlignment(Qt::AlignLeft | Qt::AlignVCenter); // hug the slider
    m_sensitivityValue->setText(tr("%1%").arg(m_sensitivity->value()));
    connect(m_sensitivity, &QSlider::valueChanged, this, [this](int pct) {
        m_sensitivityValue->setText(tr("%1%").arg(pct));
        emit sensitivityChanged(pct);
    });

    controls->addSpacing(40); // gap between the tuning controls
    m_silence = addSliderInline(controls, tr("Silence:"), tr("Silence duration milliseconds"),
                                100, 2000, 300, &m_silenceValue);
    m_silence->setSingleStep(50);
    m_silenceValue->setText(tr("%1 ms").arg(m_silence->value()));
    connect(m_silence, &QSlider::valueChanged, this, [this](int ms) {
        m_silenceValue->setText(tr("%1 ms").arg(ms));
        emit silenceMsChanged(ms);
    });

    controls->addStretch(1);

    // Transcript font size (mirrors the CW decode bar's A-/A+).
    auto* fontDown = new QPushButton(tr("A−"), this);
    fontDown->setToolTip(tr("Smaller transcript text"));
    fontDown->setAccessibleName(tr("Decrease transcript font size"));
    connect(fontDown, &QPushButton::clicked, this, [this] { adjustFont(-1); });
    controls->addWidget(fontDown);
    auto* fontUp = new QPushButton(tr("A+"), this);
    fontUp->setToolTip(tr("Larger transcript text"));
    fontUp->setAccessibleName(tr("Increase transcript font size"));
    connect(fontUp, &QPushButton::clicked, this, [this] { adjustFont(1); });
    controls->addWidget(fontUp);

    // Newline-on-silence toggle (↵). When on, each utterance (emitted when the
    // VAD detects end-of-speech silence) starts on a fresh line. Checkable, and
    // the controller styles it like the applet toggle so the on state is obvious.
    m_newline = new QPushButton(QString::fromUtf8("↵"), this);
    m_newline->setCheckable(true);
    m_newline->setToolTip(tr("Start a new line after each pause (silence)"));
    m_newline->setAccessibleName(tr("New line on silence"));
    connect(m_newline, &QPushButton::toggled, this, [this](bool on) {
        m_newlineOnSilence = on;
        emit newlineOnSilenceChanged(on);
    });
    controls->addWidget(m_newline);

    m_clear = new QPushButton(tr("Clear"), this);
    m_clear->setAccessibleName(tr("Clear transcript"));
    connect(m_clear, &QPushButton::clicked, this, [this] {
        clearText();
        emit clearRequested();
    });
    controls->addWidget(m_clear);

    // No close button: the panel is shown/hidden by the status-bar "ASR" toggle.

    root->addLayout(controls);

    // --- Transcript (mirrors the CW decode QTextEdit) -----------------------
    m_text = new QTextEdit(this);
    m_text->setObjectName(QStringLiteral("CopyAssistTranscript"));
    m_text->setAccessibleName(tr("Copy Assist transcript"));
    m_text->setReadOnly(true);
    m_text->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_text->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_text->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_text->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_text, &QTextEdit::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu* menu = m_text->createStandardContextMenu();
        menu->addSeparator();
        QAction* clear = menu->addAction(tr("Clear"));
        connect(clear, &QAction::triggered, this, [this] {
            clearText();
            emit clearRequested();
        });
        menu->exec(m_text->mapToGlobal(pos));
        delete menu;
    });
    root->addWidget(m_text, 1);

    // --- Status line (with an indeterminate loading indicator) --------------
    auto* statusRow = new QHBoxLayout;
    m_busy = new QProgressBar(this);
    m_busy->setRange(0, 0); // indeterminate "busy" animation
    m_busy->setTextVisible(false);
    m_busy->setFixedSize(90, 12);
    m_busy->setAccessibleName(tr("Copy Assist loading"));
    m_busy->hide();
    statusRow->addWidget(m_busy);

    m_status = new QLabel(tr("Disabled"), this);
    m_status->setObjectName(QStringLiteral("CopyAssistStatus"));
    m_status->setAccessibleName(tr("Copy Assist status"));
    statusRow->addWidget(m_status, 1);

    // Transcription backlog — how much received audio is still waiting to be
    // transcribed. Always visible so a user on slow hardware (e.g. a Pi) can see
    // ASR falling behind. Grows when the engine can't keep up with real time.
    m_backlog = new QLabel(this);
    m_backlog->setObjectName(QStringLiteral("CopyAssistBacklog"));
    m_backlog->setAccessibleName(tr("Transcription backlog"));
    m_backlog->setToolTip(tr("Seconds of received audio still waiting to be transcribed"));
    m_backlog->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    setBacklog(0.0);
    statusRow->addWidget(m_backlog);

    root->addLayout(statusRow);

    applyFont();
}

void CopyAssistPanel::applyFont()
{
    QFont f = m_text->font();
    f.setPixelSize(m_fontPx);
    m_text->setFont(f);                    // default for newly appended text
    m_text->document()->setDefaultFont(f);

    // Resize text already inserted via insertHtml. Merging only the font onto the
    // whole document leaves the per-utterance confidence colours untouched.
    QTextCursor cursor(m_text->document());
    cursor.select(QTextCursor::Document);
    QTextCharFormat fmt;
    fmt.setFont(f);
    cursor.mergeCharFormat(fmt);

    QTextCursor atEnd(m_text->document());
    atEnd.movePosition(QTextCursor::End);
    m_text->setTextCursor(atEnd);          // clear the selection, keep scrolled to end
}

void CopyAssistPanel::adjustFont(int deltaPx)
{
    const int next = std::clamp(m_fontPx + deltaPx, 8, 32);
    if (next == m_fontPx) {
        return;
    }
    m_fontPx = next;
    applyFont();
    emit fontPxChanged(m_fontPx);
}

void CopyAssistPanel::setFontPx(int px)
{
    m_fontPx = std::clamp(px, 8, 32);
    applyFont();
}

QSlider* CopyAssistPanel::addSliderInline(QHBoxLayout* bar, const QString& label,
                                          const QString& accessibleName, int lo, int hi,
                                          int value, QLabel** valueLabelOut)
{
    bar->addWidget(new QLabel(label, this));
    auto* slider = new QSlider(Qt::Horizontal, this);
    slider->setRange(lo, hi);
    slider->setValue(value);
    slider->setFixedWidth(90); // compact, like the CW decode bar's sliders
    slider->setAccessibleName(accessibleName);
    bar->addWidget(slider);
    auto* valueLabel = new QLabel(this);
    valueLabel->setMinimumWidth(44);
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bar->addWidget(valueLabel);
    *valueLabelOut = valueLabel;
    return slider;
}

void CopyAssistPanel::setBufferMs(int ms)
{
    m_buffer->setValue(std::clamp(ms, 1000, 20000) / 1000);
}

int CopyAssistPanel::bufferMs() const
{
    return m_buffer->value() * 1000;
}

void CopyAssistPanel::setSensitivity(int percent)
{
    m_sensitivity->setValue(std::clamp(percent, 1, 100));
}

int CopyAssistPanel::sensitivity() const
{
    return m_sensitivity->value();
}

void CopyAssistPanel::setSilenceMs(int ms)
{
    m_silence->setValue(std::clamp(ms, 100, 2000));
}

int CopyAssistPanel::silenceMs() const
{
    return m_silence->value();
}

void CopyAssistPanel::setBusy(bool on)
{
    m_busy->setVisible(on);
}

void CopyAssistPanel::setBacklog(double seconds)
{
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    m_backlog->setText(tr("Queue: %1 s").arg(seconds, 0, 'f', 1));
    // Escalate colour as it falls behind: theme-default when keeping up, amber,
    // then red once badly behind (e.g. on a Pi that can't hit real time).
    QString color;
    if (seconds > 10.0) {
        color = QStringLiteral("#ff6060");
    } else if (seconds > 2.0) {
        color = QStringLiteral("#e0a020");
    }
    m_backlog->setStyleSheet(color.isEmpty()
                                 ? QString()
                                 : QStringLiteral("QLabel { color: %1; }").arg(color));
}

void CopyAssistPanel::setStatus(const QString& text)
{
    m_status->setText(text);
}

bool CopyAssistPanel::isAsrEnabled() const
{
    return m_enable->isChecked();
}

void CopyAssistPanel::setAsrEnabled(bool on)
{
    m_enable->setChecked(on);
}

QString CopyAssistPanel::colorForConfidence(float confidence)
{
    // Higher whisper confidence = better copy (inverse of the CW cost scale).
    if (confidence >= 0.85f) {
        return QStringLiteral("#00ff88"); // green  — high confidence
    }
    if (confidence >= 0.65f) {
        return QStringLiteral("#e0e040"); // yellow — medium
    }
    if (confidence >= 0.45f) {
        return QStringLiteral("#ff9020"); // orange — low
    }
    return QStringLiteral("#ff4040");     // red    — very low
}

void CopyAssistPanel::appendText(const QString& text, float confidence)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    m_text->moveCursor(QTextCursor::End);
    // Each appendText is one utterance, emitted when the VAD ends it on silence.
    // With newline-on-silence on, start it on a fresh line (skip for the very
    // first utterance so the transcript doesn't open with a blank row).
    if (m_newlineOnSilence && !m_text->document()->isEmpty()) {
        m_text->insertHtml(QStringLiteral("<br>"));
    }
    // Bake the current size into the span so new text renders at the saved size
    // even before any font-change (applyFont on an empty document can't stick).
    m_text->insertHtml(QStringLiteral("<span style=\"color:%1; font-size:%2px\">%3</span> ")
                           .arg(colorForConfidence(confidence), QString::number(m_fontPx),
                                trimmed.toHtmlEscaped()));
    m_text->moveCursor(QTextCursor::End);
}

void CopyAssistPanel::setNewlineOnSilence(bool on)
{
    m_newline->setChecked(on); // fires toggled → updates m_newlineOnSilence + emits
}

void CopyAssistPanel::clearText()
{
    m_text->clear();
}

} // namespace AetherSDR
