#pragma once
#include "core/PcmFrame.h"
#include <QObject>
#include <QStringList>
#include <memory>
namespace AetherSDR {
// Owner-thread interface for receive decoders. Implementations own their workers.
// Future backends join the catalog/factory without changing audio routing or TX.
class CwRxBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void reset() = 0;
    virtual void feed(const PcmFrame&) = 0;
    virtual bool isRunning() const = 0;
    virtual bool supportsTuning() const { return false; }
    virtual void lockPitch(bool) {}
    virtual void lockSpeed(bool) {}
    virtual void setPitchRange(int, int) {}
    virtual void setSpeedRange(int, int) {}
    virtual float estimatedPitch() const { return 0; }
    virtual float estimatedSpeed() const { return 0; }
    virtual QString status() const { return {}; }
    virtual QString detail() const { return {}; }
    virtual bool preparing() const { return false; }
    virtual bool canRetry() const { return false; }
    virtual void cancelPreparation() {}
    virtual void retry() {}
signals:
    // Cost-bearing text uses the existing ggmorse contract: lower is better.
    // Backends without a calibrated cost use unscoredTextDecoded instead.
    void textDecoded(const QString& text, float cost);
    void unscoredTextDecoded(const QString& text);
    void statsUpdated(float pitch, float speed);
    void statusChanged();
};
class CwRxModel final : public QObject {
    Q_OBJECT
public:
    explicit CwRxModel(QObject* parent = nullptr);
    ~CwRxModel() override;
    static QStringList availableBackends();
    QString backendKey() const { return m_key; }
    bool selectBackend(const QString& key);
    void start();
    void stop();
    void reset();
    void feed(const PcmFrame& frame);
    bool isRunning() const { return m_backend->isRunning(); }
    bool supportsTuning() const { return m_backend->supportsTuning(); }
    void lockPitch(bool on) { m_backend->lockPitch(on); }
    void lockSpeed(bool on) { m_backend->lockSpeed(on); }
    void setPitchRange(int low, int high) { m_backend->setPitchRange(low, high); }
    void setSpeedRange(int low, int high) { m_backend->setSpeedRange(low, high); }
    float estimatedPitch() const { return m_backend->estimatedPitch(); }
    float estimatedSpeed() const { return m_backend->estimatedSpeed(); }
    QString status() const { return m_backend->status(); }
    QString detail() const { return m_backend->detail(); }
    bool preparing() const { return m_backend->preparing(); }
    bool canRetry() const { return m_backend->canRetry(); }
    void cancelPreparation() { m_backend->cancelPreparation(); }
    void retry() { m_backend->retry(); }
signals:
    void textDecoded(const QString& text, float cost);
    void unscoredTextDecoded(const QString& text);
    void statsUpdated(float pitch, float speed);
    void statusChanged();
private:
    void bind();
    void queueState();
    // Keep the inactive DSP backend's operator locks/ranges, with its worker
    // stopped. A backend selection must not turn a checked lock into auto mode.
    std::shared_ptr<CwRxBackend> m_ggmorse;
    std::shared_ptr<CwRxBackend> m_backend;
    QString m_key = QStringLiteral("ggmorse");
    quint64 m_generation = 0;
};
}
