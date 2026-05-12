#include "AudioDeviceChangeDialog.h"

#include <QAudioDevice>
#include <QColor>
#include <QCursor>
#include <QFrame>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMediaDevices>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <algorithm>

namespace AetherSDR {

namespace {

constexpr int kDeviceIdRole = Qt::UserRole + 1;

const char* kDialogStyle =
    "QDialog { background: #0f0f1a; }"
    "QFrame#audioDeviceHeader {"
    "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
    "    stop:0 #103626, stop:1 #10283a);"
    "  border-bottom: 1px solid #00b4d8;"
    "}"
    "QLabel { color: #c8d8e8; background: transparent; }"
    "QLabel#eyebrow { color: #40ff80; background: transparent; font-weight: bold; }"
    "QLabel#title { color: #ffffff; background: transparent; font-size: 18px; font-weight: bold; }"
    "QLabel#body { color: #c8d8e8; background: transparent; }"
    "QLabel#sectionLabel { color: #00b4d8; background: transparent; font-weight: bold; }"
    "QListWidget {"
    "  background: #0a0a14;"
    "  border: 1px solid #304050;"
    "  border-radius: 3px;"
    "  color: #c8d8e8;"
    "  alternate-background-color: #101826;"
    "  outline: none;"
    "}"
    "QListWidget::item { padding: 6px 8px; border-bottom: 1px solid #182838; }"
    "QListWidget::item:selected { background: #12384a; color: #ffffff; }"
    "QListWidget::item:hover { background: #203040; }"
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
    "QScrollBar:vertical { background: #0a0a14; width: 8px; }"
    "QScrollBar::handle:vertical { background: #304050; border-radius: 4px; }"
    "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }";

bool containsDeviceId(const QList<QByteArray>& ids, const QByteArray& id)
{
    return std::any_of(ids.cbegin(), ids.cend(),
                       [&id](const QByteArray& candidate) {
                           return candidate == id;
                       });
}

QString defaultDeviceLabel(const QAudioDevice& device, const QString& kind)
{
    if (device.isNull())
        return QStringLiteral("System default (%1 unavailable)").arg(kind);
    return QStringLiteral("System default (%1)").arg(device.description());
}

QString formatRange(int minimum, int maximum, const QString& unit)
{
    if (minimum <= 0 && maximum <= 0)
        return {};
    if (minimum == maximum)
        return QStringLiteral("%1 %2").arg(minimum).arg(unit);
    return QStringLiteral("%1-%2 %3").arg(minimum).arg(maximum).arg(unit);
}

QString deviceToolTip(const QAudioDevice& device)
{
    QStringList parts;
    const QString rate = formatRange(device.minimumSampleRate(),
                                     device.maximumSampleRate(),
                                     QStringLiteral("Hz"));
    const QString channels = formatRange(device.minimumChannelCount(),
                                         device.maximumChannelCount(),
                                         QStringLiteral("ch"));
    if (!rate.isEmpty())
        parts << rate;
    if (!channels.isEmpty())
        parts << channels;
    if (!device.id().isEmpty())
        parts << QStringLiteral("id: %1").arg(QString::fromUtf8(device.id()));
    return parts.join(QStringLiteral("\n"));
}

QListWidgetItem* addDeviceItem(QListWidget* list,
                               const QString& baseText,
                               const QByteArray& id,
                               bool current,
                               bool isDefault,
                               bool isNew,
                               const QString& toolTip = {})
{
    QStringList badges;
    if (current)
        badges << QStringLiteral("Current");
    if (isNew)
        badges << QStringLiteral("New");
    if (isDefault)
        badges << QStringLiteral("Default");

    QString text = baseText;
    if (!badges.isEmpty())
        text += QStringLiteral("  [%1]").arg(badges.join(QStringLiteral("] [")));

    auto* item = new QListWidgetItem(text, list);
    item->setData(kDeviceIdRole, id);
    if (!toolTip.isEmpty())
        item->setToolTip(toolTip);

    if (current) {
        QFont font = item->font();
        font.setBold(true);
        item->setFont(font);
    }
    if (isNew) {
        item->setForeground(QColor(QStringLiteral("#7ee787")));
    } else if (isDefault) {
        item->setForeground(QColor(QStringLiteral("#9fb3c8")));
    }

    return item;
}

QWidget* buildDeviceSection(const QString& title,
                            const QString& kind,
                            QListWidget* list,
                            const QList<QAudioDevice>& devices,
                            const QAudioDevice& currentDevice,
                            const QAudioDevice& defaultDevice,
                            const QList<QByteArray>& newDeviceIds,
                            QWidget* parent)
{
    auto* section = new QWidget(parent);
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* label = new QLabel(title, section);
    label->setObjectName("sectionLabel");
    layout->addWidget(label);

    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setMinimumHeight(132);
    list->setAlternatingRowColors(true);
    layout->addWidget(list);

    QListWidgetItem* currentItem = nullptr;
    QListWidgetItem* firstNewItem = nullptr;

    const bool followsDefault = currentDevice.isNull();
    auto* defaultItem = addDeviceItem(list,
                                      defaultDeviceLabel(defaultDevice, kind),
                                      QByteArray{},
                                      followsDefault,
                                      true,
                                      false,
                                      defaultDevice.isNull()
                                          ? QString{}
                                          : deviceToolTip(defaultDevice));
    if (followsDefault)
        currentItem = defaultItem;

    for (const QAudioDevice& device : devices) {
        const bool isCurrent = !currentDevice.isNull()
            && device.id() == currentDevice.id();
        const bool isDefault = !defaultDevice.isNull()
            && device.id() == defaultDevice.id();
        const bool isNew = containsDeviceId(newDeviceIds, device.id());

        auto* item = addDeviceItem(list,
                                   device.description(),
                                   device.id(),
                                   isCurrent,
                                   isDefault,
                                   isNew,
                                   deviceToolTip(device));
        if (isCurrent)
            currentItem = item;
        if (isNew && !firstNewItem)
            firstNewItem = item;
    }

    if (firstNewItem)
        list->setCurrentItem(firstNewItem);
    else if (currentItem)
        list->setCurrentItem(currentItem);
    else
        list->setCurrentRow(0);

    return section;
}

} // namespace

AudioDeviceChangeDialog::AudioDeviceChangeDialog(
    const QList<QAudioDevice>& inputDevices,
    const QList<QAudioDevice>& outputDevices,
    const QAudioDevice& currentInputDevice,
    const QAudioDevice& currentOutputDevice,
    const QList<QByteArray>& newInputDeviceIds,
    const QList<QByteArray>& newOutputDeviceIds,
    QWidget* parent)
    : QDialog(parent)
    , m_inputDevices(inputDevices)
    , m_outputDevices(outputDevices)
{
    setWindowTitle(QStringLiteral("Audio Device Detected"));
    setModal(true);
    setWindowModality(Qt::ApplicationModal);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setSizeGripEnabled(false);
    setStyleSheet(kDialogStyle);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* header = new QFrame(this);
    header->setObjectName("audioDeviceHeader");
    auto* headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(18, 14, 18, 14);
    headerLayout->setSpacing(4);

    auto* eyebrow = new QLabel(tr("Audio device"), header);
    eyebrow->setObjectName("eyebrow");
    headerLayout->addWidget(eyebrow);

    auto* title = new QLabel(tr("New device detected"), header);
    title->setObjectName("title");
    headerLayout->addWidget(title);
    outer->addWidget(header);

    auto* content = new QWidget(this);
    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(18, 16, 18, 18);
    root->setSpacing(10);

    auto* message = new QLabel(tr(
        "Select the PC microphone input and speaker output AetherSDR should use."),
        content);
    message->setObjectName("body");
    message->setWordWrap(true);
    root->addWidget(message);

    m_inputList = new QListWidget(content);
    root->addWidget(buildDeviceSection(QStringLiteral("Input Device"),
                                       QStringLiteral("input"),
                                       m_inputList,
                                       m_inputDevices,
                                       currentInputDevice,
                                       QMediaDevices::defaultAudioInput(),
                                       newInputDeviceIds,
                                       content));

    m_outputList = new QListWidget(content);
    root->addWidget(buildDeviceSection(QStringLiteral("Output Device"),
                                       QStringLiteral("output"),
                                       m_outputList,
                                       m_outputDevices,
                                       currentOutputDevice,
                                       QMediaDevices::defaultAudioOutput(),
                                       newOutputDeviceIds,
                                       content));

    auto* buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 4, 0, 0);
    buttonRow->setSpacing(8);

    auto* cancel = new QPushButton(tr("Cancel"), content);
    cancel->setObjectName("cancelButton");
    cancel->setCursor(Qt::PointingHandCursor);
    cancel->setMinimumHeight(32);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttonRow->addWidget(cancel);

    auto* ok = new QPushButton(tr("OK"), content);
    ok->setObjectName("primaryButton");
    ok->setCursor(Qt::PointingHandCursor);
    ok->setDefault(true);
    ok->setMinimumHeight(32);
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    buttonRow->addWidget(ok);

    root->addLayout(buttonRow);
    connect(m_inputList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem*) { accept(); });
    connect(m_outputList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem*) { accept(); });
    outer->addWidget(content);

    resize(560, sizeHint().height());
}

void AudioDeviceChangeDialog::showEvent(QShowEvent* event)
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

QAudioDevice AudioDeviceChangeDialog::selectedDevice(
    const QListWidget* list,
    const QList<QAudioDevice>& devices) const
{
    if (!list || !list->currentItem())
        return {};

    const QByteArray id = list->currentItem()->data(kDeviceIdRole).toByteArray();
    if (id.isEmpty())
        return {};

    for (const QAudioDevice& device : devices) {
        if (device.id() == id)
            return device;
    }
    return {};
}

QAudioDevice AudioDeviceChangeDialog::selectedInputDevice() const
{
    return selectedDevice(m_inputList, m_inputDevices);
}

QAudioDevice AudioDeviceChangeDialog::selectedOutputDevice() const
{
    return selectedDevice(m_outputList, m_outputDevices);
}

} // namespace AetherSDR
