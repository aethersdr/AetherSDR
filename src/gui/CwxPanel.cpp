#include "CwxPanel.h"
#include "models/CwxModel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTextEdit>
#include <QSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QStackedWidget>
#include <QScrollArea>
#include <QDateTime>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTimer>

namespace AetherSDR {

// ── Painted chat bubble widget ──────────────────────────────────────────
class CwxBubble : public QWidget {
public:
    CwxBubble(const QString& text, const QString& time, QWidget* parent = nullptr)
        : QWidget(parent), m_text(text), m_time(time)
    {
        recalcSize();
    }

    void resizeEvent(QResizeEvent*) override { recalcSize(); }

    void recalcSize()
    {
        QFont textFont("monospace", 12);
        QFont timeFont("monospace", 8);
        QFontMetrics tfm(textFont);
        QFontMetrics sfm(timeFont);
        int availW = (parentWidget() ? parentWidget()->width() : 240) - 28;
        QRect textBound = tfm.boundingRect(QRect(0, 0, availW, 10000),
                                           Qt::TextWordWrap | Qt::AlignLeft, m_text);
        int h = textBound.height() + sfm.height() + 18;
        setFixedHeight(h);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        QRect r(4, 2, width() - 12, height() - 4);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x00, 0xb4, 0xd8));
        p.drawRoundedRect(r, 10, 10);

        // CW text — 12pt, left aligned, word wrap
        QFont textFont("monospace", 12);
        p.setFont(textFont);
        p.setPen(QColor(0, 0, 0));
        QFontMetrics tfm(textFont);
        QRect textRect = r.adjusted(10, 4, -10, 0);
        QRect textBound = tfm.boundingRect(textRect, Qt::TextWordWrap | Qt::AlignLeft, m_text);
        p.drawText(textRect.adjusted(0, 0, 0, textBound.height()),
                   Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, m_text);

        // Timestamp — 8pt, right aligned, below text
        QFont timeFont("monospace", 8);
        p.setFont(timeFont);
        p.setPen(QColor(0x00, 0x30, 0x40));
        QRect timeRect = r.adjusted(10, textBound.height() + 6, -6, -2);
        p.drawText(timeRect, Qt::AlignRight | Qt::AlignTop, m_time);
    }

private:
    QString m_text, m_time;
};

static const char* kBtnStyle =
    "QPushButton { background: #1a2a3a; border: 1px solid #304050; "
    "border-radius: 3px; color: #c8d8e8; font-size: 11px; font-weight: bold; "
    "padding: 4px 10px; }"
    "QPushButton:checked { background: #00b4d8; color: #000; border: 1px solid #00d4f8; }"
    "QPushButton:hover { background: #203040; }";

static const char* kEditStyle =
    "QLineEdit { background: #ffffff; color: #000000; border: 1px solid #304050; "
    "border-radius: 2px; padding: 4px; font-size: 11px; }";

static const char* kTextStyle =
    "QTextEdit { background: #0a0a14; color: #c8d8e8; border: none; "
    "font-family: monospace; font-size: 13px; padding: 8px; }";

CwxPanel::CwxPanel(CwxModel* model, QWidget* parent)
    : QWidget(parent), m_model(model)
{
    setFixedWidth(250);
    setStyleSheet("QWidget { background: #0f0f1a; }");

    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // Title
    auto* title = new QLabel("CWX");
    title->setStyleSheet("QLabel { color: #00b4d8; font-size: 14px; font-weight: bold; "
                         "padding: 6px 8px; background: #0a0a14; }");
    vbox->addWidget(title);

    // Stacked widget for Send/Live vs Setup
    m_stack = new QStackedWidget;
    vbox->addWidget(m_stack, 1);

    buildSendView();
    buildSetupView();

    m_stack->addWidget(m_sendPage);
    m_stack->addWidget(m_setupPage);
    m_stack->setCurrentWidget(m_sendPage);

    // ── Bottom bar ─────────────────────────────────────────────
    auto* bar = new QWidget;
    bar->setStyleSheet("QWidget { background: #0a0a14; }");
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(4, 4, 4, 4);
    barLayout->setSpacing(4);

    m_sendBtn = new QPushButton("Send");
    m_sendBtn->setStyleSheet(kBtnStyle);
    barLayout->addWidget(m_sendBtn);

    m_liveBtn = new QPushButton("Live");
    m_liveBtn->setCheckable(true);
    m_liveBtn->setStyleSheet(kBtnStyle);
    barLayout->addWidget(m_liveBtn);

    m_setupBtn = new QPushButton("Setup");
    m_setupBtn->setCheckable(true);
    m_setupBtn->setStyleSheet(QString(kBtnStyle) +
        " QPushButton { padding: 4px 6px; }");
    barLayout->addWidget(m_setupBtn);

    auto* speedLabel = new QLabel("Speed:");
    speedLabel->setStyleSheet("QLabel { color: #c8d8e8; font-size: 11px; }");
    barLayout->addWidget(speedLabel);

    m_speedSpin = new QSpinBox;
    m_speedSpin->setRange(5, 100);
    m_speedSpin->setValue(20);
    m_speedSpin->setFixedWidth(50);
    m_speedSpin->setStyleSheet(
        "QSpinBox { background: #1a2a3a; color: #c8d8e8; border: 1px solid #304050; "
        "border-radius: 2px; font-size: 11px; padding: 2px; }");
    barLayout->addWidget(m_speedSpin);

    vbox->addWidget(bar);

    // ── Connections ─────────────────────────────────────────────

    // Send submits the buffer when Live is off.  If Live is on, it first
    // returns the panel to safe non-live typing without retransmitting text
    // that may already have been keyed character-by-character.
    connect(m_sendBtn, &QPushButton::clicked, this, [this]() {
        const bool wasLive = m_model ? m_model->isLive()
                                     : (m_liveBtn && m_liveBtn->isChecked());
        if (m_model)
            m_model->setLive(false);
        else if (m_liveBtn)
            m_liveBtn->setChecked(false);
        m_setupBtn->setChecked(false);
        showSendView();
        if (!wasLive)
            sendBuffer();
    });
    connect(m_liveBtn, &QPushButton::clicked, this, [this](bool on) {
        m_setupBtn->setChecked(false);
        if (m_model) m_model->setLive(on);
        showSendView();
    });
    connect(m_setupBtn, &QPushButton::clicked, this, [this]() {
        if (m_model)
            m_model->setLive(false);
        else
            m_liveBtn->setChecked(false);
        m_setupBtn->setChecked(true);
        showSetupView();
    });

    // Speed
    connect(m_speedSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) { if (m_model) m_model->setSpeed(v); });

    // Wire model signals
    // ── F1-F12 hotkeys — active app-wide when the active slice is in a CW
    //    mode (CW or CWL).  Guard prevents collisions with a future DVK
    //    macro panel or other function-key users. (#1552)
    //
    //    Created disabled; MainWindow flips enable state based on the
    //    active slice's mode (mutually exclusive with DvkPanel's F1-F12
    //    set) so the keys fire regardless of panel visibility, while
    //    Qt still sees at most one enabled ApplicationShortcut per key
    //    and never emits activatedAmbiguously. (#2464, #2582)
    for (int i = 0; i < 12; ++i) {
        auto* sc = new QShortcut(Qt::Key_F1 + i, window());
        sc->setContext(Qt::ApplicationShortcut);
        sc->setEnabled(false);
        m_shortcuts.append(sc);
        connect(sc, &QShortcut::activated, this, [this, i]() {
            if (!m_model) return;
            if (m_activeModeProvider) {
                const QString mode = m_activeModeProvider();
                if (mode != QLatin1String("CW") && mode != QLatin1String("CWL"))
                    return;
            }
            m_model->sendMacro(i + 1);
        });
    }

    // ── ESC — abort CW transmission.  Fires unconditionally: during a CW
    //    macro the interlock state flickers TRANSMITTING↔READY every
    //    dit/dah (~150ms), so gating on "is the radio TXing right now"
    //    misses most key-presses.  clearBuffer() on an idle CWX is a
    //    harmless no-op, and Qt's widget-level ESC handling (dialog
    //    reject, text unfocus) runs before our ApplicationShortcut
    //    anyway, so normal UI ESC behavior is preserved. (#1552)
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), window());
    esc->setContext(Qt::ApplicationShortcut);
    esc->setEnabled(false);
    m_shortcuts.append(esc);
    connect(esc, &QShortcut::activated, this, [this]() {
        if (m_model)
            m_model->clearBuffer();
    });

    if (m_model) setModel(m_model);
}

void CwxPanel::setModel(CwxModel* model)
{
    m_model = model;
    if (!m_model) return;

    if (m_liveBtn) {
        QSignalBlocker b(m_liveBtn);
        m_liveBtn->setChecked(m_model->isLive());
    }

    connect(m_model, &CwxModel::charSent, this, &CwxPanel::onCharSent);
    connect(m_model, &CwxModel::speedChanged, this, &CwxPanel::onSpeedChanged);
    connect(m_model, &CwxModel::macroChanged, this, [this](int idx, const QString& text) {
        if (idx >= 0 && idx < 12 && m_macroEdits[idx]) {
            QSignalBlocker b(m_macroEdits[idx]);
            m_macroEdits[idx]->setPlainText(text);
        }
    });
    connect(m_model, &CwxModel::delayChanged, this, [this](int ms) {
        if (m_delaySpin) {
            QSignalBlocker b(m_delaySpin);
            m_delaySpin->setValue(ms);
        }
    });
    connect(m_model, &CwxModel::qskChanged, this, [this](bool on) {
        if (m_qskBtn) {
            QSignalBlocker b(m_qskBtn);
            m_qskBtn->setChecked(on);
        }
    });
    connect(m_model, &CwxModel::liveChanged, this, [this](bool on) {
        if (m_liveBtn) {
            QSignalBlocker b(m_liveBtn);
            m_liveBtn->setChecked(on);
        }
        if (on) {
            if (m_setupBtn)
                m_setupBtn->setChecked(false);
            showSendView();
        }
    });
}

void CwxPanel::buildSendView()
{
    m_sendPage = new QWidget;
    auto* vbox = new QVBoxLayout(m_sendPage);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(2);

    // History — scroll area with painted bubbles, scrolls from bottom up
    m_historyScroll = new QScrollArea;
    m_historyScroll->setWidgetResizable(true);
    m_historyScroll->setStyleSheet("QScrollArea { background: #0a0a14; border: none; }");
    m_historyScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_historyContainer = new QWidget;
    m_historyContainer->setStyleSheet("QWidget { background: #0a0a14; }");
    m_historyLayout = new QVBoxLayout(m_historyContainer);
    m_historyLayout->setContentsMargins(0, 0, 0, 4);
    m_historyLayout->setSpacing(4);
    m_historyLayout->addStretch(1);  // push bubbles to bottom
    m_historyScroll->setWidget(m_historyContainer);
    vbox->addWidget(m_historyScroll, 1);

    // Input area at the bottom (editable, where user types)
    m_textEdit = new QTextEdit;
    m_textEdit->setStyleSheet(kTextStyle +
        QString(" QTextEdit { border-top: 1px solid #304050; }"));
    m_textEdit->setPlaceholderText("Type CW message...");
    m_textEdit->setAcceptRichText(false);
    m_textEdit->setFixedHeight(60);
    m_textEdit->installEventFilter(this);
    vbox->addWidget(m_textEdit, 0);
}

void CwxPanel::buildSetupView()
{
    m_setupPage = new QWidget;
    auto* vbox = new QVBoxLayout(m_setupPage);
    vbox->setContentsMargins(4, 4, 4, 4);
    vbox->setSpacing(4);

    // Delay + QSK
    auto* topRow = new QHBoxLayout;
    topRow->addWidget(new QLabel("Delay:"));
    m_delaySpin = new QSpinBox;
    m_delaySpin->setRange(0, 2000);
    m_delaySpin->setValue(5);
    m_delaySpin->setFixedWidth(60);
    m_delaySpin->setStyleSheet(
        "QSpinBox { background: #1a2a3a; color: #c8d8e8; border: 1px solid #304050; "
        "border-radius: 2px; font-size: 11px; }");
    topRow->addWidget(m_delaySpin);

    m_qskBtn = new QPushButton("QSK");
    m_qskBtn->setCheckable(true);
    m_qskBtn->setStyleSheet(kBtnStyle);
    topRow->addWidget(m_qskBtn);
    topRow->addStretch(1);
    vbox->addLayout(topRow);

    connect(m_delaySpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) { if (m_model) m_model->setDelay(v); });
    connect(m_qskBtn, &QPushButton::toggled,
            this, [this](bool on) { if (m_model) m_model->setQsk(on); });

    // Style labels
    for (auto* lbl : m_setupPage->findChildren<QLabel*>())
        lbl->setStyleSheet("QLabel { color: #c8d8e8; font-size: 11px; }");

    // F1-F12 macro slots — each stretches to fill available height
    auto* macroWidget = new QWidget;
    auto* macroGrid = new QGridLayout(macroWidget);
    macroGrid->setContentsMargins(0, 0, 0, 0);
    macroGrid->setSpacing(2);

    for (int i = 0; i < 12; ++i) {
        auto* label = new QPushButton(QString("F%1").arg(i + 1));
        label->setFixedWidth(28);
        label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        label->setStyleSheet(
            "QPushButton { background: #1a2a3a; border: 1px solid #304050; "
            "border-radius: 2px; color: #c8d8e8; font-size: 10px; font-weight: bold; "
            "padding: 2px; }"
            "QPushButton:hover { background: #203040; }");
        macroGrid->addWidget(label, i, 0);

        m_macroEdits[i] = new QTextEdit;
        m_macroEdits[i]->setStyleSheet(
            "QTextEdit { background: #ffffff; color: #000000; border: 1px solid #304050; "
            "border-radius: 2px; padding: 2px; font-size: 11px; }");
        m_macroEdits[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_macroEdits[i]->setPlaceholderText(QString("F%1 macro...").arg(i + 1));
        m_macroEdits[i]->setAcceptRichText(false);
        m_macroEdits[i]->setLineWrapMode(QTextEdit::WidgetWidth);
        macroGrid->addWidget(m_macroEdits[i], i, 1);

        macroGrid->setRowStretch(i, 1);

        // Click F-key label → send macro
        connect(label, &QPushButton::clicked, this, [this, i]() {
            if (m_model) m_model->sendMacro(i + 1);
        });

        // Edit → save macro (debounced — save when focus leaves)
        connect(m_macroEdits[i], &QTextEdit::textChanged, this, [this, i]() {
            if (m_model && m_macroEdits[i])
                m_model->saveMacro(i, m_macroEdits[i]->toPlainText().trimmed());
        });
    }

    vbox->addWidget(macroWidget, 1);

    // Prosign legend
    auto* legend = new QLabel("Prosigns: = (BT)  + (AR)  ( (KN)  & (BK)  $ (SK)");
    legend->setStyleSheet("QLabel { color: #607080; font-size: 9px; padding: 4px; }");
    legend->setWordWrap(true);
    vbox->addWidget(legend);
}

void CwxPanel::showSendView()
{
    m_stack->setCurrentWidget(m_sendPage);
    m_textEdit->setFocus();
}

void CwxPanel::showSetupView()
{
    m_stack->setCurrentWidget(m_setupPage);
}

void CwxPanel::sendBuffer()
{
    if (!m_model || !m_textEdit) return;
    QString text = m_textEdit->toPlainText().trimmed();
    if (text.isEmpty()) return;

    // Move text to history — painted bubble in scroll area
    if (m_historyLayout) {
        // Add painted bubble to history scroll area
        QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
        auto* bubble = new CwxBubble(text, ts, m_historyContainer);
        m_historyLayout->addWidget(bubble);
        // Keep scrolled to bottom
        QTimer::singleShot(10, this, [this]() {
            auto* sb = m_historyScroll->verticalScrollBar();
            sb->setValue(sb->maximum());
        });
    }
    m_textEdit->clear();

    m_model->send(text);
}

void CwxPanel::setShortcutsEnabled(bool enabled)
{
    for (auto* sc : m_shortcuts) sc->setEnabled(enabled);
}

void CwxPanel::onCharSent(int /*index*/)
{
    // TODO: highlight individual characters in bubbles as they're keyed
}

void CwxPanel::onSpeedChanged(int wpm)
{
    QSignalBlocker b(m_speedSpin);
    m_speedSpin->setValue(wpm);
}

} // namespace AetherSDR

// Event filter for text edit — handle Enter and per-key sending
bool AetherSDR::CwxPanel::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_textEdit && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);

        if (ke->key() == Qt::Key_Escape) {
            if (m_model) m_model->clearBuffer();
            m_textEdit->clear();
            return true;
        }

        if (m_model && m_model->isLive()) {
            // Live mode: send each character immediately
            QString text = ke->text();
            if (!text.isEmpty() && ke->key() != Qt::Key_Return && ke->key() != Qt::Key_Enter) {
                if (ke->key() == Qt::Key_Backspace) {
                    m_model->erase(1);
                } else {
                    m_model->sendChar(text);
                }
            }
            // Still let the text edit display the character
            return false;
        }

        // Send mode: Enter sends the buffer
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            sendBuffer();
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}
