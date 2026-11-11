#include "asr/LivePreview.h"

#include <QCoreApplication>
#include <QByteArray>

#if defined(AETHER_APPLE_LIVE_PREVIEW)
#include "asr/apple/AppleLivePreviewC.h"
#endif

namespace AetherSDR {

#if defined(AETHER_APPLE_LIVE_PREVIEW)

namespace {

class AppleLivePreview final : public ILivePreview {
public:
    AppleLivePreview(const QString& language, LivePreviewCallbacks callbacks)
        : m_callbacks(std::move(callbacks))
    {
        const QByteArray lang = language.toUtf8();
        m_session = aether_lp_create(lang.constData(), this, &AppleLivePreview::onText,
                                     &AppleLivePreview::onState);
    }

    ~AppleLivePreview() override
    {
        // aether_lp_destroy() returns only once no callback can run, so `this`
        // is never reached from the Swift side after this point.
        aether_lp_destroy(m_session);
    }

    bool valid() const { return m_session != nullptr; }

    void feed(const float* samples, int count, qint64 startSample) override
    {
        aether_lp_feed(m_session, samples, count, startSample);
    }

    void finalizeThrough(qint64 sample) override
    {
        aether_lp_finalize_through(m_session, sample);
    }

private:
    static void onText(void* ctx, const char* text, int64_t start, int64_t end, int32_t isFinal)
    {
        auto* self = static_cast<AppleLivePreview*>(ctx);
        if (self->m_callbacks.onResult) {
            self->m_callbacks.onResult(
                {static_cast<qint64>(start), static_cast<qint64>(end),
                 QString::fromUtf8(text), isFinal != 0});
        }
    }

    static void onState(void* ctx, int32_t state, const char* message)
    {
        auto* self = static_cast<AppleLivePreview*>(ctx);
        if (!self->m_callbacks.onState) {
            return;
        }
        const LivePreviewState s = state == 1   ? LivePreviewState::Ready
                                   : state == 2 ? LivePreviewState::Failed
                                                : LivePreviewState::Preparing;
        self->m_callbacks.onState(s, QString::fromUtf8(message));
    }

    LivePreviewCallbacks m_callbacks;
    void* m_session = nullptr;
};

} // namespace

bool appleLivePreviewSupported()
{
    return aether_lp_is_supported() != 0;
}

QString appleLivePreviewUnavailableReason()
{
    return appleLivePreviewSupported()
               ? QString()
               : QCoreApplication::translate(
                     "AetherSDR::LivePreview",
                     "Live preview needs macOS 26 or later on a Mac that supports "
                     "Apple's on-device speech recognition.");
}

LivePreviewFactory appleLivePreviewFactory(const QString& language)
{
    return [language](LivePreviewCallbacks callbacks) -> std::unique_ptr<ILivePreview> {
        auto preview = std::make_unique<AppleLivePreview>(language, std::move(callbacks));
        if (!preview->valid()) {
            return nullptr;
        }
        return preview;
    };
}

#else

bool appleLivePreviewSupported()
{
    return false;
}

QString appleLivePreviewUnavailableReason()
{
    return QCoreApplication::translate(
        "AetherSDR::LivePreview",
        "Live preview uses Apple's on-device speech recognition and is available "
        "only on macOS 26 or later.");
}

LivePreviewFactory appleLivePreviewFactory(const QString&)
{
    return {};
}

#endif

} // namespace AetherSDR
