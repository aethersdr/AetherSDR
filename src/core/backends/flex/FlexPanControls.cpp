#include "FlexBackend.h"
#include "models/RadioStatusOwnership.h"

#include <QPointer>
#include <QStringList>
#include <memory>

namespace AetherSDR {
namespace {

QString displayId(const QString& id)
{
    return id.size() <= 16 ? RadioStatusOwnership::normalizedFlexId(id) : QString();
}

bool radioRejected(const RadioControlReply& reply)
{
    // Local command-funnel refusals are not firmware compatibility evidence.
    return reply.kind == RadioControlReply::Kind::Failed && reply.code > 0
        && reply.code != 0x50000061 && reply.code != 0x50000063;
}

} // namespace

ReceiveDispatch FlexBackend::requestPanDisplay(const QString& panId, const QString& waterfallId,
    const PanDisplayRequest& request, const RadioControlCompletion& completion,
    const RadioControlFence& current)
{
    using Field = PanDisplayField;
    if (!request.valid() || !m_setupSink || (current && !current())) { return ReceiveDispatch::Unsupported; }
    const QString id = displayId(request.targetsWaterfall() ? waterfallId : panId);
    if (id.isEmpty()) { return ReceiveDispatch::Unsupported; }
    QString fields;
    switch (request.field) {
    case Field::Geometry: {
        const PanGeometryRequest geometry = std::get<PanGeometryRequest>(request.value);
        QStringList parts;
        if (geometry.centerMhz) { parts.append(QStringLiteral("center=%1").arg(*geometry.centerMhz, 0, 'f', 6)); }
        if (geometry.bandwidthMhz) { parts.append(QStringLiteral("bandwidth=%1").arg(*geometry.bandwidthMhz, 0, 'f', 6)); }
        fields = parts.join(QLatin1Char(' '));
        break;
    }
    case Field::Band: fields = QStringLiteral("band=%1").arg(std::get<QString>(request.value)); break;
    case Field::LevelRange: {
        const PanLevelRange range = std::get<PanLevelRange>(request.value);
        fields = QStringLiteral("min_dbm=%1 max_dbm=%2").arg(range.minimum, 0, 'f', 2).arg(range.maximum, 0, 'f', 2);
        break;
    }
    case Field::Pixels: {
        const PanPixelDimensions pixels = std::get<PanPixelDimensions>(request.value);
        fields = QStringLiteral("xpixels=%1 ypixels=%2").arg(pixels.width).arg(pixels.height);
        break;
    }
    case Field::Average: fields = QStringLiteral("average=%1").arg(std::get<int>(request.value)); break;
    case Field::FrameRate: fields = QStringLiteral("fps=%1").arg(std::get<int>(request.value)); break;
    case Field::WeightedAverage: fields = QStringLiteral("weighted_average=%1").arg(std::get<bool>(request.value) ? 1 : 0); break;
    case Field::RfGain: fields = QStringLiteral("rfgain=%1").arg(std::get<int>(request.value)); break;
    case Field::Wnb: fields = QStringLiteral("wnb=%1").arg(std::get<bool>(request.value) ? 1 : 0); break;
    case Field::WnbLevel: fields = QStringLiteral("wnb_level=%1").arg(std::get<int>(request.value)); break;
    case Field::LoopA: fields = QStringLiteral("loopa=%1").arg(std::get<bool>(request.value) ? 1 : 0); break;
    case Field::LoopB: fields = QStringLiteral("loopb=%1").arg(std::get<bool>(request.value) ? 1 : 0); break;
    case Field::NoiseFloorPosition: fields = QStringLiteral("pan_position=%1").arg(std::get<int>(request.value)); break;
    case Field::NoiseFloorEnabled: fields = QStringLiteral("pan_position_enable=%1").arg(std::get<bool>(request.value) ? 1 : 0); break;
    case Field::WaterfallRate: fields = QStringLiteral("line_duration=%1").arg(std::get<int>(request.value)); break;
    case Field::WaterfallColorGain: fields = QStringLiteral("color_gain=%1").arg(std::get<int>(request.value)); break;
    case Field::WaterfallBlackLevel: fields = QStringLiteral("black_level=%1").arg(std::get<int>(request.value)); break;
    case Field::WaterfallAutoBlack: fields = QStringLiteral("auto_black=%1").arg(std::get<bool>(request.value) ? 1 : 0); break;
    case Field::WaterfallDefaults: fields = QStringLiteral("auto_black=0 black_level=15 color_gain=50"); break;
    default: return ReceiveDispatch::Unsupported;
    }
    const QString command = QStringLiteral("display %1 set %2 %3")
        .arg(request.targetsWaterfall() ? QStringLiteral("panafall") : QStringLiteral("pan"), id, fields);
    const auto sink = m_setupSink;
    const QPointer<FlexBackend> alive(this);
    const auto finished = std::make_shared<bool>(false);
    const RadioControlCompletion complete = [completion, finished](const RadioControlReply& result) {
        if (*finished) { return; }
        *finished = true;
        if (completion) { completion(result); }
    };
    const RadioControlCompletion reply = [alive, sink, current, complete, request, id, fields]
        (const RadioControlReply& result) {
        if (!alive || (current && !current())) {
            complete({RadioControlReply::Kind::Cancelled, -1, QStringLiteral("original pan request retired")});
            return;
        }
        if (request.field == Field::WaterfallDefaults && radioRejected(result)) {
            const RadioControlCompletion fallback = [alive, current, complete](const RadioControlReply& final) {
                if (!alive || (current && !current())) { return; }
                complete(final);
            };
            if (!sink(QStringLiteral("display waterfall set %1 %2").arg(id, fields), fallback)) {
                complete({RadioControlReply::Kind::Failed, -1, QStringLiteral("waterfall fallback not dispatched")});
            }
            return;
        }
        complete(result);
    };
    return sink(command, (completion || request.field == Field::WaterfallDefaults) ? reply : RadioControlCompletion{})
        ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

ReceiveDispatch FlexBackend::requestPanLifecycle(const PanLifecycleRequest& request,
    const PanLifecycleCompletion& completion, const RadioControlFence& current)
{
    if (!m_setupSink || (current && !current())) { return ReceiveDispatch::Unsupported; }
    const auto sink = m_setupSink;
    const QPointer<FlexBackend> alive(this);
    const auto finished = std::make_shared<bool>(false);
    const PanLifecycleCompletion complete = [alive, current, completion, finished, action = request.action](const PanLifecycleReply& result) {
        if (*finished) { return; }
        *finished = true;
        if (!alive || (action != PanLifecycleAction::Remove && current && !current())) {
            if (completion) { completion({{RadioControlReply::Kind::Cancelled, -1, QStringLiteral("original pan request retired")}, result.panId, {}}); }
        } else if (completion) { completion(result); }
    };
    if (request.action == PanLifecycleAction::Create) {
        const auto decode = [complete](const RadioControlReply& result) {
            QString id;
            if (result.kind == RadioControlReply::Kind::Accepted && result.message.size() <= 4096) {
                id = RadioStatusOwnership::parsePanafallCreatePanId(result.message);
            }
            if (result.kind == RadioControlReply::Kind::Accepted && id.isEmpty()) {
                complete({{RadioControlReply::Kind::Failed, -1, QStringLiteral("invalid pan creation reply")}, {}, {}});
            } else { complete({result, id, {}}); }
        };
        const RadioControlCompletion created = [alive, sink, current, decode, complete](const RadioControlReply& result) {
            if (!alive || (current && !current())) { decode(result); return; }
            if (radioRejected(result)) {
                if (!sink(QStringLiteral("panadapter create"), decode)) {
                    complete({{RadioControlReply::Kind::Failed, -1, QStringLiteral("legacy pan creation not dispatched")}, {}, {}});
                }
            } else { decode(result); }
        };
        return sink(QStringLiteral("display panafall create x=100 y=100"), created)
            ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    if (request.action == PanLifecycleAction::RefreshInventory) {
        return sink(QStringLiteral("sub pan all"), {}) ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    const QString panId = displayId(request.panId);
    if (panId.isEmpty()) { return ReceiveDispatch::Unsupported; }
    if (request.action == PanLifecycleAction::RfGainInfo) {
        return sink(QStringLiteral("display pan rfgain_info %1").arg(panId),
            [complete, panId](const RadioControlReply& result) {
                std::optional<PanRfGainRange> range;
                if (result.kind == RadioControlReply::Kind::Accepted && result.message.size() <= 256) {
                    const QStringList parts = result.message.split(QLatin1Char(','));
                    if (parts.size() >= 3) {
                        bool lowOk = false, highOk = false, stepOk = false;
                        const int low = parts[0].trimmed().toInt(&lowOk);
                        const int high = parts[1].trimmed().toInt(&highOk);
                        const int step = parts[2].trimmed().toInt(&stepOk);
                        if (lowOk && highOk && stepOk && low >= -1000 && high <= 1000 && low <= high && step > 0 && step <= 2000) {
                            range = PanRfGainRange{low, high, step};
                        }
                    }
                }
                complete({result, panId, range});
            }) ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
    }
    if (request.action != PanLifecycleAction::Remove) { return ReceiveDispatch::Unsupported; }
    const QString waterfallId = displayId(request.waterfallId);
    if (!request.waterfallId.isEmpty() && waterfallId.isEmpty()) { return ReceiveDispatch::Unsupported; }
    struct RemovalReplies { std::optional<RadioControlReply> pan; std::optional<RadioControlReply> waterfall; };
    const auto replies = std::make_shared<RemovalReplies>();
    const auto finishRemoval = [replies, complete, panId, waterfallId] {
        if (!replies->pan || (!waterfallId.isEmpty() && !replies->waterfall)) { return; }
        const RadioControlReply result = replies->pan->kind != RadioControlReply::Kind::Accepted
            || waterfallId.isEmpty() ? *replies->pan : *replies->waterfall;
        complete({result, panId, {}});
    };
    if (!sink(QStringLiteral("display pan remove %1").arg(panId),
        [replies, finishRemoval](const RadioControlReply& result) {
            replies->pan = result;
            finishRemoval();
        })) { return ReceiveDispatch::Unsupported; }
    // The pan's removal can synchronously retire its model. The composition's
    // fence retains only the original pairing, not a requirement for a live pan.
    if (!alive || (current && !current())) {
        complete({{RadioControlReply::Kind::Cancelled, -1, QStringLiteral("original teardown pairing retired")}, panId, {}});
        return ReceiveDispatch::Dispatched;
    }
    if (!waterfallId.isEmpty() && !sink(QStringLiteral("display panafall remove %1").arg(waterfallId),
        [replies, finishRemoval](const RadioControlReply& result) {
            replies->waterfall = result;
            finishRemoval();
        })) {
        replies->waterfall = RadioControlReply{RadioControlReply::Kind::Failed, -1, QStringLiteral("waterfall removal not dispatched")};
        finishRemoval();
    }
    return ReceiveDispatch::Dispatched;
}

} // namespace AetherSDR
