// `get tracedebug <pan> bins` across Receive Sync on/off, pan and zoom (#6720):
// held frames are refused and restart settling; kSettledFrames unheld recover.
// Real ReceivePresentationSync, queue, FrameCapture and bridge, on the test's
// clock and with no socket. MainWindow's enqueue lambda and SpectrumWidget are
// desktop-only, so the stand-ins below make the calls they make.

#include "core/AutomationServer.h"
#include "core/ReceivePresentationSync.h"
#include "gui/SpectrumTraceBins.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <QWidget>

#include <cstdio>
#include <functional>

namespace AetherSDR {
class AutomationServerTestAccess
{
public:
    static QJsonObject request(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};
}  // namespace AetherSDR

// Found by the bridge by class name. Each method makes the FrameCapture calls
// the SpectrumWidget method of the same name makes.
namespace HeldFrameScenario {
class SpectrumWidget : public QWidget
{
    Q_OBJECT
public:
    void setFrequencyRangeImmediate(double centerMhz, double bandwidthMhz)
    {
        m_centerMhz = centerMhz;
        m_bandwidthMhz = bandwidthMhz;
        m_capture.noteGeometryChange();
    }
    void updateSpectrum(const QVector<float>& bins, bool heldForPresentation)
    {
        m_capture.ingest(bins, ++m_fftFrameSequence, m_centerMhz, m_bandwidthMhz, false,
                         AetherSDR::SpectrumTraceBins::FrameEncoder(), heldForPresentation);
    }
    quint64 lastSequence() const { return m_fftFrameSequence; }

    Q_INVOKABLE QVariantMap traceBinsSnapshot() const
    {
        const QString refusal = m_capture.refusal(m_centerMhz, m_bandwidthMhz, false, false);
        QVariantMap m;
        if (refusal.isEmpty()) {
            m = m_capture.snapshot();
        } else {
            m[QStringLiteral("refusal")] = refusal;
        }
        m[QStringLiteral("panIndex")] = 0;
        return m;
    }

private:
    AetherSDR::SpectrumTraceBins::FrameCapture m_capture;
    double m_centerMhz{0.0};
    double m_bandwidthMhz{0.0};
    quint64 m_fftFrameSequence{0};
};
}  // namespace HeldFrameScenario

namespace {
using namespace AetherSDR;

int failures = 0;

void check(bool ok, const QString& description)
{
    std::fprintf(stderr, "%s: %s\n", ok ? "PASS" : "FAIL", qPrintable(description));
    if (!ok) {
        ++failures;
    }
}

constexpr quint32 kStreamId = 0x40000000;

// The Flex spectrum path from FFT frame to widget: the held flag is the
// enqueue-time delay, as MainWindow_Session.cpp computes it, and a frame with
// a delay goes through the ordered visual queue and is delivered later.
class Pipeline
{
public:
    explicit Pipeline(HeldFrameScenario::SpectrumWidget& widget) : m_widget(widget) {}

    ReceivePresentationSync sync;
    qint64 nowMs{1'000'000};

    int spectrumDelayMs() const
    {
        return sync.delayMs(ReceivePresentationSource::Flex, ReceivePresentationSurface::Spectrum);
    }

    void frameArrives(const QVector<float>& bins)
    {
        const int delayMs = spectrumDelayMs();
        const bool heldForPresentation = delayMs > 0;
        std::function<void()> apply = [this, bins, heldForPresentation]() {
            m_widget.updateSpectrum(bins, heldForPresentation);
        };
        if (delayMs <= 0) {
            apply();
            return;
        }
        m_queue.enqueue({.source = ReceivePresentationSource::Flex,
                         .surface = ReceivePresentationSurface::Spectrum,
                         .sourceId = receivePresentationVisualQueueKey(
                             ReceivePresentationSource::Flex,
                             ReceivePresentationSurface::Spectrum, QString::number(kStreamId)),
                         .arrivalMs = nowMs,
                         .dueMs = nowMs + delayMs,
                         .sequence = ++m_sequence,
                         .payload = std::move(apply)});
    }

    // Returns how many queued frames were delivered.
    int advance(qint64 ms)
    {
        nowMs += ms;
        const auto due = m_queue.releaseDue(nowMs);
        for (const auto& item : due) {
            item.payload();
        }
        return static_cast<int>(due.size());
    }

    qsizetype queued() const { return m_queue.size(); }

    // MainWindow::setReceivePresentationSyncEnabled: the setting, then the
    // visual queue is cleared.
    void setReceiveSyncEnabled(bool enabled)
    {
        sync.setEnabled(enabled);
        m_queue.clear();
    }

private:
    HeldFrameScenario::SpectrumWidget& m_widget;
    ReceivePresentationQueue<std::function<void()>> m_queue;
    quint64 m_sequence{0};
};
}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    AetherSDR::AutomationServer server(nullptr);
    const auto bins = [&server]() {
        return AetherSDR::AutomationServerTestAccess::request(
            server, R"({"cmd":"get","model":"tracedebug","selector":"0","property":"bins"})");
    };
    const auto error = [](const QJsonObject& r) { return r.value("error").toString(); };
    const auto heldRefusal = [&error](const QJsonObject& r) {
        return !r.value("ok").toBool() && error(r).contains(QLatin1String("held by Receive Sync"));
    };

    constexpr int kSettled =
        static_cast<int>(SpectrumTraceBins::FrameCapture::kSettledFrames);
    int frameNo = 0;
    const auto frame = [&frameNo]() {
        ++frameNo;
        QVector<float> f(128);
        for (int i = 0; i < f.size(); ++i) {
            f[i] = -125.0f + 0.2f * static_cast<float>(i) + static_cast<float>(frameNo);
        }
        return f;
    };
    const auto sameBins = [](const QJsonObject& r, const QVector<float>& want) {
        const QJsonArray got = r.value("pan").toObject().value("bins").toArray();
        if (got.size() != want.size()) {
            return false;
        }
        for (int i = 0; i < want.size(); ++i) {
            if (got.at(i).toDouble() != static_cast<double>(want.at(i))) {
                return false;
            }
        }
        return true;
    };
    const auto panHz = [](const QJsonObject& r, const char* key) {
        return r.value("pan").toObject().value(QLatin1String(key)).toDouble();
    };

    HeldFrameScenario::SpectrumWidget widget;
    widget.setObjectName(QStringLiteral("held-frame-scenario"));
    widget.setFrequencyRangeImmediate(14.100, 0.200);
    Pipeline pipe(widget);

    // Receive Sync off: frames arrive unheld and settle.
    check(pipe.spectrumDelayMs() == 0, "off: the Flex spectrum has no presentation delay");
    QVector<float> last;
    for (int i = 0; i < kSettled; ++i) {
        last = frame();
        pipe.frameArrives(last);
    }
    QJsonObject r = bins();
    check(r.value("ok").toBool() && sameBins(r, last) && panHz(r, "centerHz") == 14'100'000.0,
          "off: the settled frame is returned as fed, at its centre");

    // Receive Sync on: frames are queued, and held when delivered.
    pipe.setReceiveSyncEnabled(true);
    const int delayMs = pipe.spectrumDelayMs();
    check(delayMs > 0, QStringLiteral("on: the Flex spectrum is delayed (%1 ms)").arg(delayMs));
    for (int i = 0; i < kSettled; ++i) {
        pipe.frameArrives(frame());
    }
    check(pipe.queued() == kSettled && pipe.advance(delayMs - 1) == 0,
          "on: frames wait in the visual queue until their due time");
    check(bins().value("ok").toBool(),
          "on: before any held frame is delivered, the earlier settled frame still answers");

    // A pan while those frames are queued.
    widget.setFrequencyRangeImmediate(14.160, 0.200);
    r = bins();
    check(!r.value("ok").toBool() && !heldRefusal(r),
          "on, pan: refused for the geometry change before the queued frames arrive");
    pipe.frameArrives(frame());
    pipe.advance(1);  // the kSettled frames queued before the pan are due
    r = bins();
    check(heldRefusal(r) && error(r).contains(QLatin1String("no age or geometry")),
          "on, pan: frames queued before the pan and delivered after it are refused as held");
    check(!error(r).contains(QLatin1String("frames ago")),
          "on, pan: and the settle count is zero, not counted up by the held frames");

    // Every held frame restarts settling, however many arrive.
    for (int i = 0; i < 3 * kSettled; ++i) {
        pipe.frameArrives(frame());
        pipe.advance(delayMs);
        check(heldRefusal(bins()),
              QStringLiteral("on: held frame %1 is refused; held frames never settle").arg(i + 1));
    }

    // A zoom while frames are queued.
    for (int i = 0; i < kSettled; ++i) {
        pipe.frameArrives(frame());
    }
    widget.setFrequencyRangeImmediate(14.160, 0.100);
    r = bins();
    check(!heldRefusal(r) && error(r).contains(QLatin1String("geometry changed after the last frame")),
          "on, zoom: refused for the geometry change right after the zoom");
    pipe.advance(delayMs);
    r = bins();
    check(heldRefusal(r), "on, zoom: frames queued before the zoom are refused as held at the new span");

    // Receive Sync off: the queue is cleared and the next frames are unheld.
    for (int i = 0; i < kSettled; ++i) {
        pipe.frameArrives(frame());
    }
    const quint64 sequenceAtOff = widget.lastSequence();
    pipe.setReceiveSyncEnabled(false);
    check(pipe.spectrumDelayMs() == 0 && pipe.queued() == 0,
          "off again: no delay, and the frames still queued are dropped");
    check(heldRefusal(bins()), "off again: the last delivered frame was held, so it is still refused");
    for (int i = 1; i < kSettled; ++i) {
        pipe.frameArrives(frame());
        r = bins();
        check(!r.value("ok").toBool()
                  && error(r).contains(QStringLiteral("arrived %1 frames ago").arg(i))
                  && error(r).contains(QLatin1String("unheld frames")),
              QStringLiteral("off again: refused after %1 unheld frame(s)").arg(i));
    }
    last = frame();
    pipe.frameArrives(last);
    pipe.advance(10 * delayMs);  // nothing queued before off is delivered late
    r = bins();
    check(r.value("ok").toBool() && sameBins(r, last), "off again: the kSettledFrames-th unheld frame is returned as fed");
    check(panHz(r, "centerHz") == 14'160'000.0 && panHz(r, "spanHz") == 100'000.0,
          "off again: at the geometry after the pan and the zoom");
    check(r.value("pan").toObject().value("fftFrameSequence").toInteger()
              == static_cast<qint64>(sequenceAtOff + kSettled),
          "off again: it is the third frame after off, not a dropped queued one");

    // A pan during recovery restarts the ordinary settle count.
    widget.setFrequencyRangeImmediate(14.200, 0.100);
    for (int i = 0; i < kSettled - 1; ++i) {
        pipe.frameArrives(frame());
    }
    r = bins();
    check(!heldRefusal(r)
              && error(r).contains(QStringLiteral("geometry changed %1 frames ago").arg(kSettled - 1)),
          "off, pan: refused below kSettledFrames frames after the pan, by the ordinary settle count");
    last = frame();
    pipe.frameArrives(last);
    r = bins();
    check(r.value("ok").toBool() && sameBins(r, last) && panHz(r, "centerHz") == 14'200'000.0,
          "off, pan: returned after kSettledFrames frames at the new centre");

    // On again, then off: the same refusal and the same recovery.
    pipe.setReceiveSyncEnabled(true);
    pipe.frameArrives(frame());
    pipe.advance(delayMs);
    check(heldRefusal(bins()), "on again: one held frame is enough to refuse");
    pipe.setReceiveSyncEnabled(false);
    for (int i = 0; i < kSettled; ++i) {
        last = frame();
        pipe.frameArrives(last);
    }
    r = bins();
    check(r.value("ok").toBool() && sameBins(r, last), "off again: recovered after kSettledFrames unheld frames");

    if (failures == 0) {
        std::fprintf(stderr, "tracedebug_bins_held_frame_scenario_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}

#include "tracedebug_bins_held_frame_scenario_test.moc"
