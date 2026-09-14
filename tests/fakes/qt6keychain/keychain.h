#pragma once

#include <QObject>
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
};

class ReadPasswordJob;

namespace TestControl {
inline int readStartCount{0};
inline ReadPasswordJob* pendingRead{nullptr};

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
};

class WritePasswordJob : public Job {
public:
    using Job::Job;
    void setTextData(const QString&) { }
};

class DeletePasswordJob : public Job {
public:
    using Job::Job;
};

inline void TestControl::reset()
{
    readStartCount = 0;
    pendingRead = nullptr;
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
