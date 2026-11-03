#pragma once
// An in-memory QNetworkAccessManager for UpdateChecker tests: createRequest
// returns a reply the test finishes by hand, so a check can be held in flight.
// No sockets, no peer.

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>

#include <algorithm>
#include <cstring>

class HeldReply final : public QNetworkReply {
public:
    HeldReply(const QNetworkRequest& request, QObject* parent)
        : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    void finishWithError(NetworkError error)
    {
        setError(error, QStringLiteral("test transport failure"));
        setFinished(true);
        emit finished();
    }
    void finishWithBody(const QByteArray& body, int status = 200)
    {
        m_body = body;
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        emit readyRead();
        setFinished(true);
        emit finished();
    }
    void abort() override { finishWithError(OperationCanceledError); }
    qint64 bytesAvailable() const override
    {
        return m_body.size() - m_offset + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char* data, qint64 capacity) override
    {
        const qint64 count = std::min(capacity, m_body.size() - m_offset);
        if (count <= 0) {
            return -1;
        }
        std::memcpy(data, m_body.constData() + m_offset, count);
        m_offset += count;
        return count;
    }

private:
    QByteArray m_body;
    qint64 m_offset = 0;
};

class HeldNetwork final : public QNetworkAccessManager {
public:
    QPointer<HeldReply> last;
    QNetworkRequest lastRequest;
    int requests = 0;

protected:
    QNetworkReply* createRequest(Operation, const QNetworkRequest& request, QIODevice*) override
    {
        ++requests;
        lastRequest = request;
        auto* reply = new HeldReply(request, this);
        last = reply;
        return reply;
    }
};
