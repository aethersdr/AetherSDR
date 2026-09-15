#include "CwRxModel.h"
#include "core/CwDecoder.h"
#ifdef HAVE_DEEPFIST
#include "DeepFistCwModel.h"
#endif
namespace AetherSDR {
namespace {
class GgmorseRxBackend final : public CwRxBackend {
public:
    ~GgmorseRxBackend() override { stop(); }
    void start() override {
        if (isRunning()) { return; }
        const quint64 generation = ++m_generation;
        connect(&m_decoder, &CwDecoder::textDecoded, this,
            [this, generation](const QString& text, float cost) {
                if (generation == m_generation && isRunning()) { emit textDecoded(text, cost); }
            });
        connect(&m_decoder, &CwDecoder::statsUpdated, this,
            [this, generation](float pitch, float speed) {
                if (generation == m_generation && isRunning()) { emit statsUpdated(pitch, speed); }
            });
        m_decoder.start();
    }
    void stop() override {
        ++m_generation;
        m_decoder.disconnect(this);
        m_decoder.stop();
    }
    void reset() override { const bool running = isRunning(); stop(); if (running) { start(); } }
    void feed(const PcmFrame& frame) override {
        const QByteArray pcm = frame.legacyStereo24();
        if (!pcm.isEmpty()) { m_decoder.feedAudio(pcm); }
    }
    bool isRunning() const override { return m_decoder.isRunning(); }
    bool supportsTuning() const override { return true; }
    void lockPitch(bool on) override { m_decoder.lockPitch(on); }
    void lockSpeed(bool on) override { m_decoder.lockSpeed(on); }
    void setPitchRange(int low, int high) override { m_decoder.setPitchRange(low, high); }
    void setSpeedRange(int low, int high) override { m_decoder.setSpeedRange(low, high); }
    float estimatedPitch() const override { return m_decoder.estimatedPitch(); }
private:
    CwDecoder m_decoder;
    quint64 m_generation{0};
};
#ifdef HAVE_DEEPFIST
class DeepFistRxBackend final : public CwRxBackend {
public:
    DeepFistRxBackend() {
        connect(&m_decoder, &DeepFistCwModel::textDecoded, this, &CwRxBackend::unscoredTextDecoded);
        connect(&m_decoder, &DeepFistCwModel::statusChanged, this, &CwRxBackend::statusChanged);
    }
    ~DeepFistRxBackend() override { stop(); }
    void start() override { m_decoder.start(); }
    void stop() override { m_decoder.stop(); }
    void reset() override { m_decoder.reset(); }
    void feed(const PcmFrame& frame) override { m_decoder.feed(frame); }
    bool isRunning() const override { return m_decoder.isRunning(); }
    QString status() const override { return m_decoder.status(); }
    QString detail() const override { return m_decoder.detail(); }
    bool preparing() const override { return m_decoder.preparing(); }
    bool canRetry() const override { return m_decoder.canRetry(); }
    void cancelPreparation() override { m_decoder.cancelPreparation(); }
    void retry() override { m_decoder.retry(); }
private:
    DeepFistCwModel m_decoder;
};
#endif
std::unique_ptr<CwRxBackend> makeBackend(const QString& key)
{
    if (key == "ggmorse") { return std::make_unique<GgmorseRxBackend>(); }
#ifdef HAVE_DEEPFIST
    if (key == "deepfist") { return std::make_unique<DeepFistRxBackend>(); }
#endif
    return {};
}
}
CwRxModel::CwRxModel(QObject* parent) : QObject(parent), m_backend(makeBackend(QStringLiteral("ggmorse"))) { bind(); }
CwRxModel::~CwRxModel() { stop(); }
QStringList CwRxModel::availableBackends()
{
    QStringList result{QStringLiteral("ggmorse")};
#ifdef HAVE_DEEPFIST
    result.append(QStringLiteral("deepfist"));
#endif
    return result;
}
bool CwRxModel::selectBackend(const QString& key)
{
    if (key == m_key) { return true; }
    std::unique_ptr<CwRxBackend> next = makeBackend(key);
    if (!next) { return false; }
    stop();
    m_backend = std::move(next);
    m_key = key;
    bind();
    emit statsUpdated(0, 0);
    emit statusChanged();
    return true;
}
void CwRxModel::bind()
{
    const quint64 generation = ++m_generation;
    m_backend->disconnect(this);
    connect(m_backend.get(), &CwRxBackend::textDecoded, this,
        [this, generation](const QString& text, float cost) {
            if (generation == m_generation && isRunning()) { emit textDecoded(text, cost); }
        });
    connect(m_backend.get(), &CwRxBackend::unscoredTextDecoded, this,
        [this, generation](const QString& text) {
            if (generation == m_generation && isRunning()) { emit unscoredTextDecoded(text); }
        });
    connect(m_backend.get(), &CwRxBackend::statsUpdated, this,
        [this, generation](float pitch, float speed) {
            if (generation == m_generation && isRunning()) { emit statsUpdated(pitch, speed); }
        });
    connect(m_backend.get(), &CwRxBackend::statusChanged, this, &CwRxModel::statusChanged);
}
void CwRxModel::start() { if (!isRunning()) { bind(); m_backend->start(); } }
void CwRxModel::stop() { ++m_generation; m_backend->stop(); }
void CwRxModel::reset() { m_backend->reset(); bind(); }
void CwRxModel::feed(const PcmFrame& frame) { if (isRunning()) { m_backend->feed(frame); } }
}
