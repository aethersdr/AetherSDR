#pragma once

#include "core/KiwiSdrClient.h"

#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QListWidget;

namespace AetherSDR {

class SliceModel;

struct KiwiSdrReceiverStatus {
    QString id;
    QString name;
    KiwiSdrClient::State state{KiwiSdrClient::State::Disconnected};
    QString detail;
    QString metadataSummary;
    QString protocolSummary;
    QPointer<SliceModel> assignedSlice;
};

class KiwiSdrApplet : public QWidget {
    Q_OBJECT

public:
    explicit KiwiSdrApplet(QWidget* parent = nullptr);

    void setReceivers(const QVector<KiwiSdrReceiverStatus>& receivers);

private:
    void rebuildReceiverList();
    QWidget* buildReceiverRow(const KiwiSdrReceiverStatus& receiver);
    QWidget* buildSliceAssignmentRow(SliceModel* slice, QWidget* parent);
    QString sliceText(SliceModel* slice) const;

    QLabel* m_emptyLabel{nullptr};
    QListWidget* m_receiverList{nullptr};
    QVector<KiwiSdrReceiverStatus> m_receivers;
    QList<QMetaObject::Connection> m_sliceConnections;
};

} // namespace AetherSDR
