#pragma once

// Minimal SmartSDR discovery listener for the Android companion spike.
// Original throwaway code; protocol facts (UDP :4992, space-separated
// key=value datagram) match src/core/RadioDiscovery.{h,cpp}.

#include <QAbstractListModel>
#include <QHostAddress>
#include <QTimer>
#include <QUdpSocket>

#include <vector>

struct SpikeRadio {
    QString serial;
    QString model;
    QString nickname;
    QString callsign;
    QString version;
    QString status;
    QHostAddress address;
    qint64 lastSeenMs{0};
};

class DiscoveryModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool listening READ listening NOTIFY listeningChanged)

public:
    enum Role {
        SerialRole = Qt::UserRole + 1,
        RadioModelRole,
        NicknameRole,
        CallsignRole,
        VersionRole,
        StatusRole,
        AddressRole,
    };

    explicit DiscoveryModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool listening() const { return m_listening; }

    Q_INVOKABLE void start();

signals:
    void listeningChanged();

private:
    void onReadyRead();
    void onStaleSweep();
    void upsert(SpikeRadio radio);

    QUdpSocket m_socket;
    QTimer m_staleTimer;
    std::vector<SpikeRadio> m_radios;
    bool m_listening{false};
};
