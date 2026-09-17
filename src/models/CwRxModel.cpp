#include "CwRxModel.h"
#include "core/CwDecoder.h"
#include <QPointer>
#ifdef HAVE_DEEPFIST
#include "DeepFistCwModel.h"
#endif
namespace AetherSDR {
namespace {
// Not advertised; see StubRxBackend below.
constexpr auto kTestBackendKey = "stub";
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
    float estimatedSpeed() const override { return m_decoder.estimatedSpeed(); }
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
// Inert second backend. The facade's lock-retention and no-overlap invariants
// are properties of CwRxModel, not of DeepFist, but asserting them needs SOME
// second entry in the catalog — and without one they could only be compiled
// with the optional DeepFist experiment enabled, which no CI job does.
// Selectable by key so a test can switch to it; deliberately absent from
// availableBackends() so no UI can ever offer it to an operator.
class StubRxBackend final : public CwRxBackend {
public:
    void start() override { m_running = true; emit statusChanged(); }
    void stop() override { m_running = false; }
    void reset() override {}
    void feed(const PcmFrame&) override {}
    bool isRunning() const override { return m_running; }
private:
    bool m_running{false};
};
std::shared_ptr<CwRxBackend> makeBackend(const QString& key)
{
    if (key == "ggmorse") { return std::make_shared<GgmorseRxBackend>(); }
    if (key == kTestBackendKey) { return std::make_shared<StubRxBackend>(); }
#ifdef HAVE_DEEPFIST
    if (key == "deepfist") { return std::make_shared<DeepFistRxBackend>(); }
#endif
    return {};
}
}
CwRxModel::CwRxModel(QObject* parent)
    : QObject(parent), m_ggmorse(makeBackend(QStringLiteral("ggmorse"))), m_backend(m_ggmorse)
{
    bind();
}
CwRxModel::~CwRxModel()
{
    ++m_generation;
    m_backend->disconnect(this);
    m_backend->stop();
}
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
    std::shared_ptr<CwRxBackend> next = key == "ggmorse" ? m_ggmorse : makeBackend(key);
    if (!next) { return false; }
    ++m_generation;
    m_backend->disconnect(this);
    m_backend->stop();
    m_backend = std::move(next);
    m_key = key;
    bind();
    queueState();
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
    // Asset/status callbacks can run inside start/stop/ensure. Defer outward
    // publication until those operations unwind; stale backend posts die here.
    connect(m_backend.get(), &CwRxBackend::statusChanged, this, [this, generation] {
        if (generation == m_generation) { emit statusChanged(); }
    }, Qt::QueuedConnection);
}
void CwRxModel::start() { if (!isRunning()) { bind(); m_backend->start(); } }
void CwRxModel::stop()
{
    ++m_generation;
    m_backend->stop();
    bind();
    queueState();
}
void CwRxModel::reset()
{
    m_backend->reset();
    bind();
    queueState();
}
void CwRxModel::queueState()
{
    const quint64 generation = m_generation;
    QMetaObject::invokeMethod(this, [this, generation] {
        if (generation != m_generation) { return; }
        const QPointer<CwRxModel> guard(this);
        emit statsUpdated(estimatedPitch(), estimatedSpeed());
        if (guard && generation == m_generation) { emit statusChanged(); }
    }, Qt::QueuedConnection);
}
void CwRxModel::feed(const PcmFrame& frame) { if (isRunning()) { m_backend->feed(frame); } }
}
