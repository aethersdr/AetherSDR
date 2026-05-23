#include "ClientDisconnectDialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCursor>
#include <QFrame>
#include <QGuiApplication>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>

namespace MasterSDR {

namespace {

const char* kDialogStyle =
    "QDialog { background: #0f0f1a; }"
    "QFrame#clientDisconnectHeader {"
    "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
    "    stop:0 #103626, stop:1 #10283a);"
    "  border-bottom: 1px solid #00b4d8;"
    "}"
    "QLabel { color: #c8d8e8; background: transparent; }"
    "QLabel#eyebrow { color: #40ff80; background: transparent; font-weight: bold; }"
    "QLabel#title { color: #ffffff; background: transparent; font-size: 18px; font-weight: bold; }"
    "QLabel#body { color: #c8d8e8; background: transparent; }"
    "QLabel#prompt { color: #8aa8c0; background: transparent; font-weight: bold; }"
    "QPushButton {"
    "  background: #1a2a3a;"
    "  border: 1px solid #304050;"
    "  border-radius: 3px;"
    "  color: #c8d8e8;"
    "  padding: 6px 14px;"
    "}"
    "QPushButton:hover { background: #203040; border-color: #00b4d8; }"
    "QPushButton:pressed { background: #102030; }"
    "QPushButton#primaryButton {"
    "  background: #00b4d8;"
    "  border-color: #00c8f0;"
    "  color: #0f0f1a;"
    "  font-weight: bold;"
    "}"
    "QPushButton#primaryButton:hover { background: #00c8f0; }"
    "QPushButton#cancelButton { color: #8aa8c0; }"
    "QScrollArea { background: transparent; border: none; }"
    "QScrollBar:vertical { background: #0a0a14; width: 8px; }"
    "QScrollBar::handle:vertical { background: #304050; border-radius: 4px; }"
    "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }";

QList<quint32> allHandles(const QList<ClientDisconnectDialog::Client>& clients)
{
    QList<quint32> handles;
    for (const auto& client : clients) {
        if (client.handle != 0 && !handles.contains(client.handle))
            handles.append(client.handle);
    }
    return handles;
}

} // namespace

ClientDisconnectDialog::ClientDisconnectDialog(const QList<Client>& clients,
                                               int maxSlices,
                                               QWidget* parent,
                                               Mode mode)
    : QDialog(parent)
    , m_clients(clients)
{
    const bool remoteMode = mode == Mode::RemoteClientDisconnect;

    setWindowTitle(remoteMode ? tr("Disconnect remote clients") : tr("Radio in use"));
    setModal(true);
    setWindowModality(Qt::ApplicationModal);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setSizeGripEnabled(false);
    setStyleSheet(kDialogStyle);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* header = new QFrame(this);
    header->setObjectName("clientDisconnectHeader");
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(18, 14, 18, 14);
    headerLayout->setSpacing(4);

    auto* eyebrow = new QLabel(remoteMode ? tr("Remote clients") : tr("Radio in use"), header);
    eyebrow->setObjectName("eyebrow");
    headerLayout->addWidget(eyebrow);

    auto* title = new QLabel(remoteMode ? tr("Disconnect clients") : tr("No available slices"), header);
    title->setObjectName("title");
    headerLayout->addWidget(title);
    outer->addWidget(header);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 16, 18, 18);
    layout->setSpacing(10);

    auto* body = new QLabel(remoteMode
            ? tr("Choose a SmartLink client to disconnect from this radio, or disconnect all clients.")
            : tr("All available receiver slices on this radio are currently being used by other clients. "
                 "Disconnect a client to free a slice, or cancel the connection."),
        content);
    body->setObjectName("body");
    body->setWordWrap(true);
    layout->addWidget(body);

    auto* prompt = new QLabel(tr("Please select:"), content);
    prompt->setObjectName("prompt");
    layout->addWidget(prompt);

    auto addButton = [this, layout, content](const QString& text,
                                             const QList<quint32>& handles,
                                             const QString& objectName = QString()) {
        auto* button = new QPushButton(text, content);
        if (!objectName.isEmpty())
            button->setObjectName(objectName);
        button->setCursor(Qt::PointingHandCursor);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(32);
        connect(button, &QPushButton::clicked, this, [this, handles] {
            acceptWithHandles(handles);
        });
        layout->addWidget(button);
    };

    addButton(tr("Disconnect all clients"), allHandles(m_clients), QStringLiteral("primaryButton"));

    auto* scroll = new QScrollArea(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* scrollContents = new QWidget(scroll);
    auto* clientLayout = new QVBoxLayout(scrollContents);
    clientLayout->setContentsMargins(0, 0, 0, 0);
    clientLayout->setSpacing(8);
    scroll->setWidget(scrollContents);

    for (const auto& client : m_clients) {
        auto* button = new QPushButton(tr("Disconnect %1").arg(displayName(client)), scrollContents);
        button->setCursor(Qt::PointingHandCursor);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(32);
        connect(button, &QPushButton::clicked, this, [this, handle = client.handle] {
            acceptWithHandles({handle});
        });
        clientLayout->addWidget(button);
    }

    const int clientCount = static_cast<int>(m_clients.size());
    const int visibleRows = std::min(4, std::max(1, clientCount));
    scroll->setFixedHeight(visibleRows * 34 + std::max(0, visibleRows - 1) * 8);
    layout->addWidget(scroll);

    auto* cancel = new QPushButton(tr("Cancel"), content);
    cancel->setObjectName("cancelButton");
    cancel->setCursor(Qt::PointingHandCursor);
    cancel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    cancel->setMinimumHeight(32);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(cancel);
    outer->addWidget(content);

    const int width = std::clamp(440 + std::max(0, maxSlices - 4) * 10, 440, 560);
    resize(width, sizeHint().height());
}

void ClientDisconnectDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);

    QScreen* screen = nullptr;
    if (parentWidget() && parentWidget()->windowHandle())
        screen = parentWidget()->windowHandle()->screen();
    if (!screen)
        screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;

    const QRect area = screen->availableGeometry();
    move(area.center() - rect().center());
}

QString ClientDisconnectDialog::displayName(const Client& client) const
{
    const QString program = client.program.trimmed();
    const QString station = client.station.trimmed();

    if (!program.isEmpty() && !station.isEmpty() && program != station)
        return tr("%1: %2").arg(program, station);
    if (!station.isEmpty())
        return station;
    if (!program.isEmpty())
        return program;
    return tr("client 0x%1").arg(client.handle, 8, 16, QChar('0')).toUpper();
}

void ClientDisconnectDialog::acceptWithHandles(const QList<quint32>& handles)
{
    m_selectedHandles.clear();
    for (quint32 handle : handles) {
        if (handle != 0 && !m_selectedHandles.contains(handle))
            m_selectedHandles.append(handle);
    }
    accept();
}

} // namespace MasterSDR
