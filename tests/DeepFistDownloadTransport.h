#pragma once
// In-memory implementation of the documented HTTP reply contract; no sockets.
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QHash>
#include <cstring>

struct DeepFistReplySpec {
    QByteArray bytes;
    int status = 200;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    bool notifyReadyRead = true;
};
class DeepFistTestReply final : public QNetworkReply {
public:
    DeepFistTestReply(const QNetworkRequest& request, DeepFistReplySpec spec, QObject* parent)
        : QNetworkReply(parent), m_spec(std::move(spec))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, m_spec.status);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] {
            if (isFinished()) { return; }
            m_available = true;
            if (m_spec.error != NoError) { setError(m_spec.error, "Test transport failure"); }
            if (m_spec.notifyReadyRead) { emit readyRead(); }
            if (!isFinished()) { setFinished(true); emit finished(); }
        });
    }
    void abort() override
    {
        if (isFinished()) { return; }
        setError(OperationCanceledError, "Canceled");
        setFinished(true);
        emit finished();
    }
    qint64 bytesAvailable() const override
    {
        return (m_available ? m_spec.bytes.size() - m_offset : 0) + QNetworkReply::bytesAvailable();
    }
protected:
    qint64 readData(char* data, qint64 capacity) override
    {
        const qint64 count = std::min(capacity, m_spec.bytes.size() - m_offset);
        if (!m_available || count <= 0) { return -1; }
        std::memcpy(data, m_spec.bytes.constData() + m_offset, count);
        m_offset += count;
        return count;
    }
private:
    DeepFistReplySpec m_spec;
    qint64 m_offset = 0;
    bool m_available = false;
};
class DeepFistTestNetwork final : public QNetworkAccessManager {
public:
    QHash<QString, DeepFistReplySpec> files;
    int requests = 0;
protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        ++requests;
        const QString name = request.url().fileName();
        return new DeepFistTestReply(request, files.value(name, {{}, 404, QNetworkReply::ContentNotFoundError}), this);
    }
};
