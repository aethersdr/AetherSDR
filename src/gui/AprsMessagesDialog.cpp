#include "AprsMessagesDialog.h"

#include "core/ThemeManager.h"
#include "core/aprs/AprsMessenger.h"

#include <QColor>
#include <QHeaderView>
#include <QShowEvent>
#include <QTableWidget>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// Visual language matches kAetherModemStyle in Ax25HfPacketDecodeDialog.cpp —
// this window is an extension of the AetherModem APRS tab.
constexpr const char* kMessagesStyle = R"(
QWidget {
    color: #aeb9cc;
    background: #07101c;
    font-size: 14px;
}
QTableWidget {
    color: #c2ccdb;
    background: #050b13;
    border: 1px solid #233246;
    border-radius: 7px;
    gridline-color: #14202f;
    font-family: "SF Mono", "Menlo", "Consolas", monospace;
    font-size: 13px;
    selection-background-color: #1b3650;
}
QHeaderView::section {
    color: #8d99ad;
    background: #0d1825;
    border: none;
    border-bottom: 1px solid #233246;
    padding: 6px 8px;
    font-size: 11px;
    font-weight: 700;
}
QTableCornerButton::section {
    background: #0d1825;
    border: none;
}
QScrollBar:vertical {
    background: #07101c;
    width: 12px;
    margin: 8px 2px 8px 2px;
    border-radius: 6px;
}
QScrollBar::handle:vertical {
    background: #25364d;
    border-radius: 5px;
    min-height: 34px;
}
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical {
    height: 0px;
}
)";

QString stateText(const AprsMessenger::Message& m)
{
    if (!m.outgoing)
        return m.read ? QString() : QStringLiteral("new");
    switch (m.state) {
    case AprsMessenger::State::Pending:  return QStringLiteral("queued");
    case AprsMessenger::State::Sent:     return QStringLiteral("sent (try %1)").arg(m.tries);
    case AprsMessenger::State::Acked:    return QStringLiteral("✓ acked");
    case AprsMessenger::State::Rejected: return QStringLiteral("✗ rejected");
    case AprsMessenger::State::Failed:   return QStringLiteral("✗ no ack");
    case AprsMessenger::State::Received: return QString();
    }
    return QString();
}

} // namespace

AprsMessagesDialog::AprsMessagesDialog(AprsMessenger* messenger, QWidget* parent)
    : PersistentDialog(QStringLiteral("APRS Messages"),
                       QStringLiteral("AprsMessagesDialogGeometry"),
                       parent)
    , m_messenger(messenger)
{
    theme::setContainer(this, QStringLiteral("dialog/ax25Decode"));
    setMinimumSize(620, 320);
    bodyWidget()->setStyleSheet(QString::fromLatin1(kMessagesStyle));

    auto* root = new QVBoxLayout(bodyWidget());
    m_table = new QTableWidget(bodyWidget());
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("TIME (UTC)"), QStringLiteral(""),
        QStringLiteral("STATION"), QStringLiteral("MESSAGE"),
        QStringLiteral("STATUS"),
    });
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->setShowGrid(false);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->setWordWrap(false);
    root->addWidget(m_table);

    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (auto* item = m_table->item(row, 2))
            emit replyRequested(item->text());
    });
    if (m_messenger) {
        connect(m_messenger, &AprsMessenger::messagesChanged,
                this, &AprsMessagesDialog::rebuild);
    }
    rebuild();
}

void AprsMessagesDialog::showEvent(QShowEvent* event)
{
    PersistentDialog::showEvent(event);
    // Opening the envelope is what "reading" means here.
    if (m_messenger)
        m_messenger->markAllRead();
}

void AprsMessagesDialog::rebuild()
{
    if (!m_messenger)
        return;
    const QVector<AprsMessenger::Message> messages = m_messenger->messages();
    m_table->setRowCount(messages.size());
    for (int i = 0; i < messages.size(); ++i) {
        const AprsMessenger::Message& m = messages.at(i);
        auto set = [&](int col, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            if (m.outgoing)
                item->setForeground(QColor(0x8d, 0xc5, 0xff));
            else if (!m.read)
                item->setForeground(QColor(0x80, 0xed, 0x91));
            m_table->setItem(i, col, item);
        };
        set(0, m.utc.toString(QStringLiteral("MM/dd HH:mm:ss")));
        set(1, m.outgoing ? QStringLiteral("→") : QStringLiteral("←"));
        set(2, m.counterpart);
        set(3, m.text);
        set(4, stateText(m));
    }
    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->scrollToBottom();
}

} // namespace AetherSDR
