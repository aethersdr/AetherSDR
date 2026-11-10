#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace AetherSDR {

struct RigExpertModelInfo {
    QString name;
    QStringList serialPrefixes;
    qint64 minHz{0};
    qint64 maxHz{0};
    bool experimental{false};
};

struct RigExpertUsbDevice {
    QString path;
    QString serial;
    QString product;
};

// Injection boundary: one source-API HID report per send/receive, including
// report ID 0x07. USB access runs on an I/O thread; signals reach the owner.
class RigExpertHidTransport : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void start(const QString& path) = 0;
    virtual void send(const QByteArray& report) = 0;
    // Cancels pending commands, attempts the final report, then closes. A close
    // is host-side evidence only; no supported model has a qualified RF-off receipt.
    virtual void finish(const QByteArray& finalReport) = 0;

    static bool available();
    static const QList<RigExpertModelInfo>& models();
    static RigExpertModelInfo modelForSerial(const QString& serial);
    static QList<RigExpertUsbDevice> enumerate();
    static RigExpertHidTransport* create(QObject* parent);

signals:
    void opened();
    void reportReceived(const QByteArray& report);
    void reportWritten(const QByteArray& report);
    void failed(const QString& error);
    void closed();
};

} // namespace AetherSDR
