#include "KiwiSdrApplet.h"

#include "SliceColorManager.h"
#include "SliceLabel.h"
#include "Theme.h"
#include "core/ThemeManager.h"
#include "models/SliceModel.h"

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QStringList>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

QString listStyle()
{
    return ThemeManager::instance().resolve(
        QStringLiteral(
            "QListWidget { background: transparent; border: none; "
            "color: {{color.text.primary}}; font-size: 10px; }"
            "QListWidget::item { padding: 0; margin: 0; }"));
}

QString emptyStyle()
{
    return ThemeManager::instance().resolve(
        QStringLiteral(
            "QLabel { color: {{color.text.label}}; font-size: 10px; "
            "padding: 4px 2px; }"));
}

QString labelStyle()
{
    return ThemeManager::instance().resolve(
        QStringLiteral("QLabel { color: {{color.text.label}}; font-size: 10px; }"));
}

QString primaryLabelStyle()
{
    return ThemeManager::instance().resolve(
        QStringLiteral(
            "QLabel { color: {{color.text.primary}}; font-size: 10px; "
            "font-weight: bold; }"));
}

QString receiverRowStyle()
{
    return ThemeManager::instance().resolve(
        QStringLiteral(
            "QWidget#kiwiReceiverRow { background: {{color.background.0}}; "
            "border: 1px solid {{color.background.1}}; border-radius: 3px; }"));
}

QString stateToken(KiwiSdrClient::State state)
{
    switch (state) {
    case KiwiSdrClient::State::Connected:
        return QStringLiteral("color.accent.success");
    case KiwiSdrClient::State::Connecting:
        return QStringLiteral("color.accent.warning");
    case KiwiSdrClient::State::Error:
        return QStringLiteral("color.accent.danger");
    case KiwiSdrClient::State::Disconnected:
        return QStringLiteral("color.text.label");
    }
    return QStringLiteral("color.text.label");
}

QString statusStyle(KiwiSdrClient::State state)
{
    return ThemeManager::instance().resolve(
        QStringLiteral("QLabel { color: {{%1}}; font-size: 10px; font-weight: bold; }")
            .arg(stateToken(state)));
}

QString stateText(KiwiSdrClient::State state)
{
    switch (state) {
    case KiwiSdrClient::State::Disconnected:
        return QObject::tr("Disconnected");
    case KiwiSdrClient::State::Connecting:
        return QObject::tr("Connecting");
    case KiwiSdrClient::State::Connected:
        return QObject::tr("Connected");
    case KiwiSdrClient::State::Error:
        return QObject::tr("Error");
    }
    return QObject::tr("Disconnected");
}

QString detailStyle()
{
    return ThemeManager::instance().resolve(
        QStringLiteral("QLabel { color: {{color.text.label}}; font-size: 9px; }"));
}

QString sliceBadgeStyle(int colorIdx, bool assigned)
{
    const QString color = SliceColorManager::instance().hexActive(colorIdx);
    if (assigned) {
        return QStringLiteral(
            "QLabel { background: %1; color: #000000; border: 1px solid %1; "
            "border-radius: 3px; font-weight: bold; font-size: 9px; padding: 0; }")
            .arg(color);
    }
    return QStringLiteral(
        "QLabel { background: #2a2a2a; color: %1; border: 1px solid %1; "
        "border-radius: 3px; font-weight: bold; font-size: 9px; padding: 0; }")
        .arg(color);
}

QString receiverAccessibleText(const KiwiSdrReceiverStatus& receiver)
{
    QStringList parts;
    parts << receiver.name
          << stateText(receiver.state);
    if (!receiver.detail.trimmed().isEmpty()) {
        parts << receiver.detail.trimmed();
    }
    if (receiver.assignedSlice) {
        parts << QStringLiteral("Assigned to slice %1")
                     .arg(receiver.assignedSlice->letter());
    } else {
        parts << QStringLiteral("Not assigned to a slice");
    }
    return parts.join(QStringLiteral(", "));
}

} // namespace

KiwiSdrApplet::KiwiSdrApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/kiwisdr"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 2, 4, 4);
    root->setSpacing(4);

    m_emptyLabel = new QLabel(tr("No KiwiSDR RX antennas configured"), this);
    m_emptyLabel->setAccessibleName(tr("KiwiSDR receiver status"));
    m_emptyLabel->setStyleSheet(emptyStyle());
    m_emptyLabel->setWordWrap(true);
    root->addWidget(m_emptyLabel);

    m_receiverList = new QListWidget(this);
    m_receiverList->setAccessibleName(tr("KiwiSDR receiver status"));
    m_receiverList->setAccessibleDescription(
        tr("Shows configured KiwiSDR receive antennas, connection state, and assigned slice."));
    m_receiverList->setFocusPolicy(Qt::NoFocus);
    m_receiverList->setSelectionMode(QAbstractItemView::NoSelection);
    m_receiverList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_receiverList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_receiverList->setMinimumHeight(48);
    m_receiverList->setMaximumHeight(176);
    m_receiverList->setStyleSheet(listStyle());
    root->addWidget(m_receiverList);

    rebuildReceiverList();
}

void KiwiSdrApplet::setReceivers(const QVector<KiwiSdrReceiverStatus>& receivers)
{
    for (const QMetaObject::Connection& connection : m_sliceConnections) {
        disconnect(connection);
    }
    m_sliceConnections.clear();
    m_receivers = receivers;

    for (const KiwiSdrReceiverStatus& receiver : m_receivers) {
        SliceModel* slice = receiver.assignedSlice.data();
        if (!slice) {
            continue;
        }
        m_sliceConnections.append(connect(slice, &SliceModel::letterChanged,
                                          this, &KiwiSdrApplet::rebuildReceiverList));
        m_sliceConnections.append(connect(slice, &SliceModel::frequencyChanged,
                                          this, &KiwiSdrApplet::rebuildReceiverList));
        m_sliceConnections.append(connect(slice, &SliceModel::modeChanged,
                                          this, &KiwiSdrApplet::rebuildReceiverList));
        m_sliceConnections.append(connect(slice, &SliceModel::filterChanged,
                                          this, &KiwiSdrApplet::rebuildReceiverList));
        m_sliceConnections.append(connect(slice, &SliceModel::panIdChanged,
                                          this, &KiwiSdrApplet::rebuildReceiverList));
    }

    rebuildReceiverList();
}

void KiwiSdrApplet::rebuildReceiverList()
{
    if (!m_receiverList || !m_emptyLabel) {
        return;
    }

    m_receiverList->clear();
    const bool hasReceivers = !m_receivers.isEmpty();
    m_emptyLabel->setVisible(!hasReceivers);
    m_receiverList->setVisible(hasReceivers);

    for (const KiwiSdrReceiverStatus& receiver : m_receivers) {
        auto* item = new QListWidgetItem(m_receiverList);
        item->setData(Qt::UserRole, receiver.id);
        QWidget* row = buildReceiverRow(receiver);
        item->setSizeHint(row->sizeHint());
        m_receiverList->setItemWidget(item, row);
    }
}

QWidget* KiwiSdrApplet::buildReceiverRow(const KiwiSdrReceiverStatus& receiver)
{
    auto* row = new QWidget(m_receiverList);
    row->setObjectName(QStringLiteral("kiwiReceiverRow"));
    row->setStyleSheet(receiverRowStyle());
    row->setAccessibleName(receiverAccessibleText(receiver));

    auto* layout = new QVBoxLayout(row);
    layout->setContentsMargins(5, 4, 5, 4);
    layout->setSpacing(3);

    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(4);

    auto* name = new QLabel(receiver.name, row);
    name->setTextFormat(Qt::PlainText);
    name->setStyleSheet(primaryLabelStyle());
    name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    topRow->addWidget(name, 1);

    auto* status = new QLabel(stateText(receiver.state), row);
    status->setTextFormat(Qt::PlainText);
    status->setStyleSheet(statusStyle(receiver.state));
    status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    topRow->addWidget(status, 0, Qt::AlignRight);
    layout->addLayout(topRow);

    const QString detail = receiver.detail.trimmed();
    if (!detail.isEmpty()) {
        auto* detailLabel = new QLabel(detail, row);
        detailLabel->setTextFormat(Qt::PlainText);
        detailLabel->setStyleSheet(detailStyle());
        detailLabel->setWordWrap(true);
        layout->addWidget(detailLabel);
    }

    layout->addWidget(buildSliceAssignmentRow(receiver.assignedSlice.data(), row));
    return row;
}

QWidget* KiwiSdrApplet::buildSliceAssignmentRow(SliceModel* slice, QWidget* parent)
{
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    if (!slice) {
        auto* label = new QLabel(tr("Not assigned"), row);
        label->setTextFormat(Qt::PlainText);
        label->setStyleSheet(labelStyle());
        layout->addWidget(label, 1);
        return row;
    }

    auto* badge = new QLabel(row);
    badge->setFixedSize(18, 19);
    badge->setAlignment(Qt::AlignCenter);
    badge->setTextFormat(Qt::PlainText);
    badge->setText(SliceLabel::unicodeForm(slice->sliceId(), slice->letter()));
    const int colorIdx = SliceLabel::displayColorIndex(slice->sliceId(),
                                                       slice->letter());
    badge->setStyleSheet(sliceBadgeStyle(colorIdx, true));
    layout->addWidget(badge, 0, Qt::AlignVCenter);

    auto* text = new QLabel(sliceText(slice), row);
    text->setTextFormat(Qt::PlainText);
    text->setStyleSheet(labelStyle());
    text->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    text->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(text, 1);
    return row;
}

QString KiwiSdrApplet::sliceText(SliceModel* slice) const
{
    if (!slice) {
        return QString();
    }

    const QString filter = QStringLiteral("%1..%2 Hz")
        .arg(slice->filterLow())
        .arg(slice->filterHigh());
    return QStringLiteral("%1 MHz  %2  %3")
        .arg(slice->frequency(), 0, 'f', 6)
        .arg(slice->mode())
        .arg(filter);
}

} // namespace AetherSDR
