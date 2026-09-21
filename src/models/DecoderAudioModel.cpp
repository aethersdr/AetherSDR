#include "DecoderAudioModel.h"
#include "RadioModel.h"
#include "SliceModel.h"
#include "core/LogManager.h"

#include <QMutex>
#include <QMutexLocker>

#include <array>
#include <deque>
#include <utility>

namespace AetherSDR {

struct DecoderAudioModel::Impl {
    // Direct producer callbacks retain this small inbox, never an unbounded Qt
    // event queue of PCM. Only one drain event is posted; the mutex protects its
    // target pointer against rebind/destruction. No decoder runs on the producer.
    struct Inbox {
        QMutex mutex;
        DecoderAudioModel* target = nullptr;
        std::deque<PcmFrame> frames;
        qsizetype frameCount = 0;
        bool scheduled = false;
        bool overflow = false;
        static constexpr qsizetype kMaxFrames = 65536;
        static constexpr std::size_t kMaxBlocks = 256;
    };
    DecoderAudioModel* owner;
    QPointer<RadioModel> radio;
    QPointer<SliceModel> slice;
    Consumer consumer;
    bool enabled = false;
    RouteStatus routeStatus = RouteStatus::Inactive;
    // Acquisition emits an external create request. Keep old and new holds
    // visible together until it returns, including during nested rebinding.
    std::array<bool, 8> heldChannels{};
    quint64 bindingRevision = 0;
    QMetaObject::Connection audioConnection;
    QList<QMetaObject::Connection> sliceConnections;
    std::shared_ptr<Inbox> inbox;
    DecoderPcmAdapter adapter;

    void setRouteStatus(RouteStatus status)
    {
        if (routeStatus == status) {
            return;
        }
        routeStatus = status;
        const QPointer<DecoderAudioModel> guard(owner);
        const quint64 revision = bindingRevision;
        emit owner->routeStatusChanged();
        // A status listener can repair the route, disable it or destroy us.
        // Do not publish a warning for a binding superseded by that callback.
        if (!guard || revision != bindingRevision) {
            return;
        }
        if (status == RouteStatus::SharedRxAudio) {
            // Not a warning: with no DAX assignment this is the ordinary Flex
            // path and the decoder still runs, just on the shared mix.
            qCInfo(lcDsp) << (consumer == Consumer::Cw ? "CW" : "RTTY")
                            << "receive decoder: no DAX RX channel assigned to the selected slice;"
                            << "decoding the shared receive audio (speaker gain and mute apply)";
        } else if (status == RouteStatus::DaxTransportUnavailable) {
            qCWarning(lcDsp) << (consumer == Consumer::Cw ? "CW" : "RTTY")
                            << "receive decoder: selected DAX transport unavailable; no input route";
        }
    }

    PanadapterStream::DaxConsumer daxConsumer() const
    {
        return consumer == Consumer::Cw ? PanadapterStream::DaxConsumer::CwDecoder
                                        : PanadapterStream::DaxConsumer::RttyDecoder;
    }

    void closeInbox()
    {
        QObject::disconnect(audioConnection);
        if (inbox) {
            QMutexLocker lock(&inbox->mutex);
            inbox->target = nullptr;
            inbox->frames.clear();
        }
        inbox.reset();
    }

    void releaseHold(int channel)
    {
        if (std::exchange(heldChannels[static_cast<std::size_t>(channel - 1)], false) && radio) {
            radio->releaseDaxChannel(channel, daxConsumer());
        }
    }

    void releaseAllHolds()
    {
        for (int channel = 1; channel <= 8; ++channel) {
            releaseHold(channel);
        }
    }

    static void enqueue(const std::shared_ptr<Inbox>& box, const PcmFrame& frame)
    {
        if (!frame.current()) {
            return;
        }
        QMutexLocker lock(&box->mutex);
        if (!box->target) {
            return;
        }
        if (box->frames.size() == Inbox::kMaxBlocks
            || box->frameCount + frame.frameCount() > Inbox::kMaxFrames) {
            box->frames.clear();
            box->frameCount = 0;
            box->overflow = true;
        }
        box->frames.push_back(frame);
        box->frameCount += frame.frameCount();
        if (!box->scheduled) {
            box->scheduled = true;
            DecoderAudioModel* target = box->target;
            QMetaObject::invokeMethod(target, [target, box] {
                target->m_impl->drain(box);
            }, Qt::QueuedConnection);
        }
    }

    void drain(const std::shared_ptr<Inbox>& box)
    {
        if (box != inbox || !enabled || !radio || !radio->isConnected()
            || !slice || radio->slice(slice->sliceId()) != slice) {
            // Leaving `scheduled` set on a box that is still the live inbox
            // stops enqueue() ever posting another drain: the route would keep
            // reporting Bound while every later frame piled up to the overflow
            // cap and was discarded. The backlog is stale here by definition.
            QMutexLocker lock(&box->mutex);
            box->frames.clear();
            box->frameCount = 0;
            box->scheduled = false;
            return;
        }
        const QPointer<DecoderAudioModel> guard(owner);
        std::deque<PcmFrame> frames;
        bool overflow = false;
        {
            QMutexLocker lock(&box->mutex);
            frames.swap(box->frames);
            box->frameCount = 0;
            box->scheduled = false;
            overflow = std::exchange(box->overflow, false);
        }
        // adapter.reset() makes the next accepted block a discontinuity, which
        // publishes its own reset below; without this the decoder would take two
        // generation bumps and two neutral stat clears for one overflow.
        bool resetPublished = false;
        if (overflow) {
            adapter.reset();
            emit owner->sourceReset();
            resetPublished = true;
            if (!guard || box != inbox) {
                return;
            }
        }
        for (const PcmFrame& frame : frames) {
            // A consumer can synchronously change selection from a notification.
            if (box != inbox) {
                return;
            }
            const std::optional<DecoderPcmBlock> block = adapter.accept(frame);
            if (!block || !block->current()) {
                continue;
            }
            if (block->discontinuity && !std::exchange(resetPublished, false)) {
                emit owner->sourceReset();
                if (!guard || box != inbox) {
                    return;
                }
            }
            if (box == inbox && block->current()) {
                emit owner->nativePcmReady(frame);
                if (!guard || box != inbox || !block->current()) {
                    return;
                }
                emit owner->pcmReady(*block);
                if (!guard || box != inbox) {
                    return;
                }
            }
        }
    }

    void rebind()
    {
        const QPointer<DecoderAudioModel> guard(owner);
        const quint64 revision = ++bindingRevision;
        closeInbox();
        adapter.clearRoute();
        emit owner->sourceReset();
        // Direct listeners may delete us or synchronously install a different
        // binding. Never resume the superseded operation over that new inbox.
        if (!guard || revision != bindingRevision) {
            return;
        }
        const bool live = enabled && radio && radio->isConnected() && slice
            && radio->slice(slice->sliceId()) == slice;
        const bool dax = live && radio->hasDaxStreams();
        const int channel = dax ? slice->daxChannel() : 0;
        const int wantedHold = channel >= 1 && channel <= 8 ? channel : 0;
        bool acquired = true;
        if (wantedHold && !heldChannels[static_cast<std::size_t>(wantedHold - 1)]) {
            // Publish the new hold BEFORE acquisition's synchronous callback;
            // a nested rebind or destructor owns both channels and can release
            // either. Acquire first, then retire old holds below.
            heldChannels[static_cast<std::size_t>(wantedHold - 1)] = true;
            acquired = radio->acquireDaxChannel(wantedHold, daxConsumer());
            if (!guard || revision != bindingRevision) {
                return;
            }
            if (!acquired) {
                heldChannels[static_cast<std::size_t>(wantedHold - 1)] = false;
            }
        }
        for (int held = 1; held <= 8; ++held) {
            if (held != wantedHold || !acquired) {
                releaseHold(held);
                if (!guard || revision != bindingRevision) {
                    return;
                }
            }
        }
        if (!live) {
            setRouteStatus(RouteStatus::Inactive);
            return;
        }
        // Only a DAX channel that exists and could not be acquired is fail-closed:
        // the shared receive stream rides the same PanadapterStream, so when the
        // transport is gone there is nothing to fall back to either.
        if (dax && wantedHold && !acquired) {
            setRouteStatus(RouteStatus::DaxTransportUnavailable);
            return;
        }
        // A DAX-capable radio with no assignment on the selected slice: decode
        // the radio's shared receive stream, the pre-A5 input. It mixes every
        // audible slice and follows speaker gain/mute, so it is disclosed as a
        // distinct status rather than reported as an isolated route.
        const bool sharedRxAudio = dax && !wantedHold;
        inbox = std::make_shared<Inbox>();
        inbox->target = owner;
        const std::shared_ptr<Inbox> box = inbox;
        if (sharedRxAudio) {
            adapter.selectRoute(DecoderPcmAdapter::RouteLane::RxDemod, 0);
            audioConnection = QObject::connect(radio, &RadioModel::rxDemodAudioReady,
                owner, [box](const PcmFrame& frame) {
                    if (frame.stream().purpose == PcmPurpose::Speaker) {
                        enqueue(box, frame);
                    }
                }, Qt::DirectConnection);
        } else if (dax) {
            adapter.selectRoute(DecoderPcmAdapter::RouteLane::Dax, channel);
            if (auto* stream = radio->panStream()) {
                audioConnection = QObject::connect(stream, &PanadapterStream::daxPcmReady,
                    owner, [box, channel](int incomingChannel, const PcmFrame& frame) {
                        if (incomingChannel == channel
                            && frame.stream().purpose == PcmPurpose::Auxiliary) {
                            enqueue(box, frame);
                        }
                    }, Qt::DirectConnection);
            }
        } else {
            const int id = slice->sliceId();
            adapter.selectRoute(DecoderPcmAdapter::RouteLane::NativeSlice, id);
            audioConnection = QObject::connect(radio, &RadioModel::backendSliceAudioFrameReady,
                owner, [box, id](int incomingId, const PcmFrame& frame) {
                    if (incomingId == id && frame.stream().purpose == PcmPurpose::Slice
                        && frame.stream().sliceId == id) {
                        enqueue(box, frame);
                    }
                }, Qt::DirectConnection);
        }
        setRouteStatus(sharedRxAudio ? RouteStatus::SharedRxAudio : RouteStatus::Bound);
    }
};

DecoderAudioModel::DecoderAudioModel(RadioModel& radio, Consumer consumer, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>())
{
    m_impl->owner = this;
    m_impl->radio = &radio;
    m_impl->consumer = consumer;
    connect(&radio, &RadioModel::connectionStateChanged, this, [this](bool connected) {
        if (!connected) {
            setSlice(nullptr);
        } else {
            m_impl->rebind();
        }
    });
    connect(&radio, &RadioModel::backendRebuilt, this, [this] { setSlice(nullptr); });
    connect(&radio, &RadioModel::sliceRemoved, this, [this](int id) {
        m_impl->adapter.retireRoute(DecoderPcmAdapter::RouteLane::NativeSlice, id);
        if (m_impl->slice && m_impl->slice->sliceId() == id) {
            setSlice(nullptr);
        }
    });
}

DecoderAudioModel::~DecoderAudioModel()
{
    m_impl->closeInbox();
    m_impl->releaseAllHolds();
}

void DecoderAudioModel::setSlice(SliceModel* slice)
{
    Impl& d = *m_impl;
    if (d.slice == slice) {
        return;
    }
    if (d.slice && slice && d.slice->sliceId() == slice->sliceId()) {
        d.adapter.retireRoute(DecoderPcmAdapter::RouteLane::NativeSlice, slice->sliceId());
    }
    for (const QMetaObject::Connection& connection : std::as_const(d.sliceConnections)) {
        disconnect(connection);
    }
    d.sliceConnections.clear();
    d.slice = slice;
    if (slice) {
        auto reset = [this] { m_impl->rebind(); };
        d.sliceConnections.append(connect(slice, &SliceModel::frequencyChanged, this, reset));
        d.sliceConnections.append(connect(slice, &SliceModel::modeChanged, this, reset));
        d.sliceConnections.append(connect(slice, &SliceModel::daxChannelChanged, this, reset));
        d.sliceConnections.append(connect(slice, &QObject::destroyed, this, [this] {
            m_impl->slice = nullptr;
            m_impl->rebind();
        }));
    }
    d.rebind();
}

void DecoderAudioModel::setEnabled(bool enabled)
{
    if (m_impl->enabled == enabled) {
        return;
    }
    m_impl->enabled = enabled;
    m_impl->rebind();
}

DecoderAudioModel::RouteStatus DecoderAudioModel::routeStatus() const
{
    return m_impl->routeStatus;
}
} // namespace AetherSDR
