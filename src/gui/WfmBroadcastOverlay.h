#pragma once
#include <QObject>
#include <QMetaType>
#include <QPointer>
#include <QString>
#include <QVector>

namespace AetherSDR {
class RadioModel;

// Local presentation only: no spot index, cluster publication, lifetime store,
// DXCC lookup, smart filtering or click-to-tune semantics.
struct WfmBroadcastOverlayRecord {
    int sliceId{-1};
    quint64 sessionId{0};
    quint64 receiverEpoch{0};
    quint64 revision{0};
    qint64 frequencyHz{0};
    int program{-1};
    QString stationName;
    QString title;
    QString artist;
    QString displayText() const
    {
        QString text = QStringLiteral("HD Radio · HD%1").arg(program + 1);
        for (const QString& part : {stationName, title, artist}) {
            if (!part.isEmpty()) { text += QStringLiteral(" · ") + part; }
        }
        return text;
    }
    bool operator==(const WfmBroadcastOverlayRecord&) const = default;
};

class WfmBroadcastOverlay final : public QObject {
    Q_OBJECT
public:
    explicit WfmBroadcastOverlay(QObject* parent = nullptr);
    void bind(RadioModel* model, const QString& panId);
    const QVector<WfmBroadcastOverlayRecord>& records() const { return m_records; }
signals:
    void overlaysChanged(const QVector<AetherSDR::WfmBroadcastOverlayRecord>& records);
private:
    void rebindSlices();
    void refresh();
    QPointer<RadioModel> m_model;
    QString m_panId;
    QVector<QMetaObject::Connection> m_modelConnections;
    QVector<QMetaObject::Connection> m_sliceConnections;
    QVector<WfmBroadcastOverlayRecord> m_records;
};
}

Q_DECLARE_METATYPE(AetherSDR::WfmBroadcastOverlayRecord)
