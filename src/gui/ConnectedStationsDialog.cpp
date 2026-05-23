#include "ConnectedStationsDialog.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCursor>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWindow>

namespace MasterSDR {

namespace {

const char* kStyle =
    "QDialog { background: #0f0f1a; }"
    "QFrame#radioSection {"
    "  background: #121220;"
    "  border-bottom: 1px solid #1e2840;"
    "}"
    "QFrame#stationsSection {"
    "  background: #0f0f1a;"
    "}"
    "QLabel#sectionHeader {"
    "  color: #ffffff;"
    "  font-size: 13px;"
    "  font-weight: bold;"
    "  background: transparent;"
    "}"
    "QLabel#radioModel {"
    "  color: #e0eaf8;"
    "  font-size: 14px;"
    "  font-weight: bold;"
    "  background: transparent;"
    "}"
    "QLabel#radioStation {"
    "  color: #00b4d8;"
    "  font-size: 12px;"
    "  background: transparent;"
    "}"
    "QLabel#infoLabel {"
    "  color: #8aa8c0;"
    "  font-size: 11px;"
    "  background: transparent;"
    "}"
    "QFrame#stationRow {"
    "  background: #171728;"
    "  border: 1px solid #1e2840;"
    "  border-radius: 3px;"
    "}"
    "QFrame#stationRow:hover {"
    "  background: #1a2035;"
    "  border-color: #304060;"
    "}"
    "QRadioButton {"
    "  color: #c8d8e8;"
    "  background: transparent;"
    "  spacing: 8px;"
    "}"
    "QRadioButton::indicator { width: 14px; height: 14px; }"
    "QRadioButton::indicator:checked { color: #00b4d8; }"
    "QPushButton {"
    "  background: #1a2a3a;"
    "  border: 1px solid #304050;"
    "  border-radius: 3px;"
    "  color: #c8d8e8;"
    "  padding: 6px 16px;"
    "}"
    "QPushButton:hover { background: #203040; border-color: #00b4d8; }"
    "QPushButton:pressed { background: #102030; }"
    "QPushButton#disconnectButton {"
    "  background: #c0392b;"
    "  border-color: #e74c3c;"
    "  color: #ffffff;"
    "  font-weight: bold;"
    "}"
    "QPushButton#disconnectButton:hover { background: #e74c3c; }"
    "QPushButton#disconnectButton:disabled {"
    "  background: #2a1a1a;"
    "  border-color: #3a2a2a;"
    "  color: #604040;"
    "}"
    "QPushButton#cancelButton { color: #8aa8c0; }";

} // namespace

ConnectedStationsDialog::ConnectedStationsDialog(const RadioMeta& radio,
                                                 const QList<Client>& clients,
                                                 QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Connected Stations"));
    setModal(true);
    setWindowModality(Qt::ApplicationModal);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setSizeGripEnabled(false);
    setStyleSheet(kStyle);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ── Radio section ──────────────────────────────────────────────────────
    auto* radioFrame = new QFrame(this);
    radioFrame->setObjectName("radioSection");
    auto* radioLayout = new QVBoxLayout(radioFrame);
    radioLayout->setContentsMargins(18, 14, 18, 14);
    radioLayout->setSpacing(4);

    auto* radioHeader = new QLabel(tr("Radio"), radioFrame);
    radioHeader->setObjectName("sectionHeader");
    radioLayout->addWidget(radioHeader);

    auto* radioModel = new QLabel(radio.model, radioFrame);
    radioModel->setObjectName("radioModel");
    radioLayout->addWidget(radioModel);

    const QString stationLine = QStringList({radio.nickname, radio.callsign})
                                    .join(QStringLiteral("   "))
                                    .trimmed();
    if (!stationLine.isEmpty()) {
        auto* radioStation = new QLabel(stationLine, radioFrame);
        radioStation->setObjectName("radioStation");
        radioLayout->addWidget(radioStation);
    }

    outer->addWidget(radioFrame);

    // ── Connected Stations section ─────────────────────────────────────────
    auto* stationsFrame = new QFrame(this);
    stationsFrame->setObjectName("stationsSection");
    auto* stationsLayout = new QVBoxLayout(stationsFrame);
    stationsLayout->setContentsMargins(18, 14, 18, 18);
    stationsLayout->setSpacing(10);

    auto* stationsHeader = new QLabel(tr("Connected Stations"), stationsFrame);
    stationsHeader->setObjectName("sectionHeader");
    stationsLayout->addWidget(stationsHeader);

    auto* infoLabel = new QLabel(
        tr("multiFLEX is disabled on this radio. Select a station to disconnect "
           "before connecting MasterSDR."),
        stationsFrame);
    infoLabel->setObjectName("infoLabel");
    infoLabel->setWordWrap(true);
    stationsLayout->addWidget(infoLabel);

    auto* group = new QButtonGroup(this);
    auto* disconnectBtn = new QPushButton(tr("Disconnect Station"), stationsFrame);
    disconnectBtn->setObjectName("disconnectButton");
    disconnectBtn->setEnabled(false);
    disconnectBtn->setCursor(Qt::PointingHandCursor);
    disconnectBtn->setMinimumHeight(32);

    for (const Client& client : clients) {
        auto* rowFrame = new QFrame(stationsFrame);
        rowFrame->setObjectName("stationRow");
        auto* rowLayout = new QHBoxLayout(rowFrame);
        rowLayout->setContentsMargins(10, 6, 10, 6);

        QString label = client.program.trimmed();
        const QString station = client.station.trimmed();
        if (!station.isEmpty() && station != label)
            label = label.isEmpty() ? station : QStringLiteral("%1: %2").arg(label, station);
        if (label.isEmpty())
            label = QStringLiteral("client 0x%1").arg(client.handle, 8, 16, QChar('0')).toUpper();

        auto* rb = new QRadioButton(label, rowFrame);
        // Store the full quint32 handle as a property — avoids the truncation /
        // sign-bit ambiguity that arises when casting to QButtonGroup's int id.
        rb->setProperty("handle", QVariant::fromValue(client.handle));
        group->addButton(rb);
        rowLayout->addWidget(rb);
        stationsLayout->addWidget(rowFrame);

        connect(rb, &QRadioButton::toggled, this, [disconnectBtn](bool checked) {
            if (checked)
                disconnectBtn->setEnabled(true);
        });
    }

    connect(disconnectBtn, &QPushButton::clicked, this, [this, group] {
        QAbstractButton* checked = group->checkedButton();
        if (checked) {
            m_selectedHandle = checked->property("handle").value<quint32>();
            accept();
        }
    });

    auto* cancelBtn = new QPushButton(tr("Cancel"), stationsFrame);
    cancelBtn->setObjectName("cancelButton");
    cancelBtn->setCursor(Qt::PointingHandCursor);
    cancelBtn->setMinimumHeight(32);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

    stationsLayout->addWidget(disconnectBtn);
    stationsLayout->addWidget(cancelBtn);
    outer->addWidget(stationsFrame);

    resize(380, sizeHint().height());
}

void ConnectedStationsDialog::showEvent(QShowEvent* event)
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

} // namespace MasterSDR
