#pragma once

// Test double for the QtKeychain 0.17.0 job contract (not an OS-vault emulator):
// https://github.com/frankosterfeld/qtkeychain/tree/0.17.0/qtkeychain
// Models error values, finished(Job*) delivery and auto-delete cleanup. Recheck
// those contracts against keychain.h/keychain.cpp when upgrading QtKeychain.
// Deliberate differences: tests control completion, start() records immediately,
// and insecureFallback starts TRUE (upstream defaults false), so the test proves
// the adapter explicitly disables fallback rather than relying on a default.
// Native scheduling, prompts, storage and backend availability are not modeled.

#include <QObject>
#include <QByteArray>
#include <QString>

namespace QKeychain {

enum Error {
    NoError = 0,
    EntryNotFound,
    CouldNotDeleteEntry,
    AccessDeniedByUser,
    AccessDenied,
    NoBackendAvailable,
    NotImplemented,
    OtherError,
};

class Job : public QObject {
    Q_OBJECT

public:
    explicit Job(const QString& service, QObject* parent = nullptr)
        : QObject(parent)
        , m_service(service)
    {
    }

    void start() { started(); }
    void setAutoDelete(bool autoDelete) { m_autoDelete = autoDelete; }
    void setKey(const QString& key) { m_key = key; }
    void setInsecureFallback(bool allowed) { m_insecureFallback = allowed; }
    [[nodiscard]] bool insecureFallback() const { return m_insecureFallback; }
    [[nodiscard]] bool autoDelete() const { return m_autoDelete; }
    [[nodiscard]] QString service() const { return m_service; }
    [[nodiscard]] QString key() const { return m_key; }
    [[nodiscard]] Error error() const { return m_error; }
    [[nodiscard]] QString errorString() const { return m_errorString; }

signals:
    void finished(QKeychain::Job* job);

protected:
    virtual void started() { }

    void complete(Error error, const QString& errorString)
    {
        m_error = error;
        m_errorString = errorString;
        emit finished(this);
        if (m_autoDelete) {
            deleteLater();
        }
    }

private:
    QString m_service;
    QString m_key;
    Error m_error{NoError};
    QString m_errorString;
    bool m_autoDelete{true};
    bool m_insecureFallback{true};
};

class ReadPasswordJob;
class WritePasswordJob;

namespace TestControl {
inline int readStartCount{0};
inline ReadPasswordJob* pendingRead{nullptr};
inline int writeStartCount{0};
inline WritePasswordJob* pendingWrite{nullptr};

void reset();
void completeRead(const QString& value);
void failRead(Error error, const QString& errorString);
} // namespace TestControl

class ReadPasswordJob : public Job {
public:
    explicit ReadPasswordJob(const QString& service, QObject* parent = nullptr)
        : Job(service, parent)
    {
    }

    [[nodiscard]] QString textData() const { return m_textData; }
    [[nodiscard]] QByteArray binaryData() const { return m_binaryData; }

    void completeBinary(const QByteArray& value)
    {
        m_binaryData = value;
        complete(NoError, {});
    }

    void completeRead(const QString& value)
    {
        m_textData = value;
        complete(NoError, {});
    }

    void failRead(Error error, const QString& errorString)
    {
        complete(error, errorString);
    }

protected:
    void started() override
    {
        ++TestControl::readStartCount;
        TestControl::pendingRead = this;
    }

private:
    QString m_textData;
    QByteArray m_binaryData;
};

class WritePasswordJob : public Job {
public:
    using Job::Job;
    void setTextData(const QString&) { }
    void setBinaryData(const QByteArray& bytes) { m_binaryData = bytes; }
    [[nodiscard]] QByteArray binaryData() const { return m_binaryData; }
    void finish(Error error = NoError) { complete(error, {}); }
protected:
    void started() override
    {
        ++TestControl::writeStartCount;
        TestControl::pendingWrite = this;
    }
private:
    QByteArray m_binaryData;
};

class DeletePasswordJob : public Job {
public:
    using Job::Job;
};

inline void TestControl::reset()
{
    readStartCount = 0;
    pendingRead = nullptr;
    writeStartCount = 0;
    pendingWrite = nullptr;
}

inline void TestControl::completeRead(const QString& value)
{
    ReadPasswordJob* job = pendingRead;
    pendingRead = nullptr;
    job->completeRead(value);
}

inline void TestControl::failRead(Error error, const QString& errorString)
{
    ReadPasswordJob* job = pendingRead;
    pendingRead = nullptr;
    job->failRead(error, errorString);
}

} // namespace QKeychain
