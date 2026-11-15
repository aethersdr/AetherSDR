#pragma once

#include "core/AetherPDClient.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QIODevice>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace AetherPDTest {
class Reply final : public QNetworkReply {
public:
    Reply(const QNetworkRequest& request, QObject* parent) : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    void abort() override
    {
        if (!isFinished()) {
            aborted = true;
            setError(OperationCanceledError, QStringLiteral("canceled"));
            setFinished(true);
            emit finished();
        }
    }
    void complete(int code, const QByteArray& data, NetworkError error = NoError)
    {
        payload = data;
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, code);
        if (error != NoError) {
            setError(error, QStringLiteral("injected transport error"));
        }
        emit readyRead();
        if (!isFinished()) {
            setFinished(true);
            emit finished();
        }
    }
    void announceLength(qint64 length)
    {
        setHeader(QNetworkRequest::ContentLengthHeader, length);
        emit metaDataChanged();
    }
    qint64 bytesAvailable() const override { return payload.size() - position; }
    bool aborted{false};
protected:
    qint64 readData(char* target, qint64 size) override
    {
        const qint64 count = qMin(size, bytesAvailable());
        if (count <= 0) {
            return -1;
        }
        std::memcpy(target, payload.constData() + position, static_cast<size_t>(count));
        position += count;
        return count;
    }
private:
    QByteArray payload;
    qint64 position{0};
};
class Network final : public QNetworkAccessManager {
public:
    struct Sent { Operation method; QNetworkRequest request; QByteArray body; QPointer<Reply> reply; };
    std::vector<Sent> sent;
    QPointer<Reply> last;
    Reply* captureChunk(int index) const
    {
        const QString suffix = QStringLiteral("/%1").arg(index);
        for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
            const QString path = it->request.url().path();
            if (path.startsWith(QStringLiteral("/v1/aetherpd/capture/")) && path.endsWith(suffix)) {
                return it->reply;
            }
        }
        return nullptr;
    }
protected:
    QNetworkReply* createRequest(Operation method, const QNetworkRequest& request, QIODevice* body) override
    {
        sent.push_back({method, request, body ? body->readAll() : QByteArray{}, {}});
        std::unique_ptr<Reply> reply = std::make_unique<Reply>(request, this);
        last = reply.get();
        sent.back().reply = last;
        return reply.release(); // QObject parent owns the returned reply.
    }
};
} // namespace AetherPDTest
