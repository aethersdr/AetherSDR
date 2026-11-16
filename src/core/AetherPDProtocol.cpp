#include "AetherPDProtocol.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QList>
#include <QSet>
#include <QtEndian>

#include <bit>
#include <cmath>
#include <limits>

namespace AetherSDR::aetherpd {
namespace {
constexpr qsizetype kMaxReplyBytes = 16 * 1024;
constexpr double kMaxExactJsonCounter = 9'007'199'254'740'991.0;

// QJsonDocument validates syntax but collapses duplicate members. Scan its
// validated input before using the object, including escaped-equivalent keys.
// The protocol has objects and scalar values only, with no arrays.
bool uniqueKeys(const QByteArray& json)
{
    QList<QSet<QString>> objects;
    QList<bool> expectKey;
    for (qsizetype i = 0; i < json.size(); ++i) {
        const char c = json.at(i);
        if (c == '[' || c == ']') {
            return false;
        }
        if (c == '{') {
            if (objects.size() == 4) {
                return false;
            }
            objects.append(QSet<QString>{});
            expectKey.append(true);
        } else if (c == '}') {
            if (objects.isEmpty()) {
                return false;
            }
            objects.removeLast();
            expectKey.removeLast();
        } else if (c == ',' || c == ':') {
            if (expectKey.isEmpty()) {
                return false;
            }
            expectKey.last() = c == ',';
        } else if (c == '-') {
            return false;
        } else if (c >= '0' && c <= '9') {
            while (i + 1 < json.size() && json.at(i + 1) >= '0' && json.at(i + 1) <= '9') {
                ++i;
            }
            if (i + 1 < json.size()) {
                const char next = json.at(i + 1);
                if (next != ',' && next != '}' && next != ' ' && next != '\t' && next != '\r' && next != '\n') {
                    return false;
                }
            }
        } else if (c == '"') {
            const qsizetype start = i;
            for (++i; i < json.size() && json.at(i) != '"'; ++i) {
                if (json.at(i) == '\\') {
                    ++i;
                }
            }
            if (i >= json.size() || objects.isEmpty()) {
                return false;
            }
            if (expectKey.last()) {
                const QByteArray literal = '[' + json.mid(start, i - start + 1) + ']';
                const QString key = QJsonDocument::fromJson(literal).array().at(0).toString();
                if (key.size() > 128 || objects.last().contains(key)) {
                    return false;
                }
                objects.last().insert(key);
            }
        }
    }
    return objects.isEmpty();
}

std::optional<QJsonObject> object(const QByteArray& json)
{
    if (json.isEmpty() || json.size() > kMaxReplyBytes) {
        return std::nullopt;
    }
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject() || !uniqueKeys(json)) {
        return std::nullopt;
    }
    return doc.object();
}

bool captureIdValid(const QString& id)
{
    if (id.size() != 32) { return false; }
    for (const QChar c : id) {
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f'))) { return false; }
    }
    return true;
}

bool timestamp(const QJsonObject& o, const char* key, quint64& result)
{
    const QJsonValue value = o.value(QLatin1String(key));
    if (!value.isString()) { return false; }
    const QString text = value.toString();
    bool ok = false;
    result = text.toULongLong(&ok);
    return ok && result > 0 && result <= quint64(std::numeric_limits<qint64>::max())
        && text == QString::number(result);
}

bool boolean(const QJsonObject& o, const char* key, bool& result)
{
    const QJsonValue value = o.value(QLatin1String(key));
    if (!value.isBool()) {
        return false;
    }
    result = value.toBool();
    return true;
}

bool counter(const QJsonObject& o, const char* key, quint64& result)
{
    const QJsonValue value = o.value(QLatin1String(key));
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0 || number > kMaxExactJsonCounter
        || std::trunc(number) != number) {
        return false;
    }
    result = static_cast<quint64>(number);
    return true;
}

std::optional<State> state(const QJsonValue& value)
{
    if (!value.isString()) {
        return std::nullopt;
    }
    const QString text = value.toString();
    if (text == QLatin1String("unavailable")) { return State::Unavailable; }
    if (text == QLatin1String("stopped")) { return State::Stopped; }
    if (text == QLatin1String("cleanup_unconfirmed")) { return State::CleanupUnconfirmed; }
    if (text == QLatin1String("stopped_receive_unconfirmed")) { return State::StoppedReceiveUnconfirmed; }
    if (text == QLatin1String("starting")) { return State::Starting; }
    if (text == QLatin1String("feedback_binding_pending")) { return State::FeedbackBindingPending; }
    if (text == QLatin1String("feedback_binding_failed")) { return State::FeedbackBindingFailed; }
    if (text == QLatin1String("disabled")) { return State::Disabled; }
    if (text == QLatin1String("blocked_by_radio_apd")) { return State::BlockedByRadioApd; }
    if (text == QLatin1String("measurement_armed")) { return State::MeasurementArmed; }
    if (text == QLatin1String("calibration_armed")) { return State::CalibrationArmed; }
    if (text == QLatin1String("recalibration_required")) { return State::RecalibrationRequired; }
    if (text == QLatin1String("idle")) { return State::Idle; }
    if (text == QLatin1String("correcting")) { return State::Correcting; }
    if (text == QLatin1String("calibrating")) { return State::Calibrating; }
    return std::nullopt;
}

std::optional<CalibrationState> calibrationState(const QJsonValue& value)
{
    if (!value.isString()) { return std::nullopt; }
    const QString text = value.toString();
    if (text == QLatin1String("idle")) { return CalibrationState::Idle; }
    if (text == QLatin1String("completed")) { return CalibrationState::Completed; }
    if (text == QLatin1String("canceled")) { return CalibrationState::Canceled; }
    if (text == QLatin1String("expired")) { return CalibrationState::Expired; }
    if (text == QLatin1String("armed")) { return CalibrationState::Armed; }
    if (text == QLatin1String("running")) { return CalibrationState::Running; }
    return std::nullopt;
}

std::optional<quint64> measurementId(const QJsonValue& value)
{
    if (!value.isString()) { return std::nullopt; }
    const QString id = value.toString();
    if (id.size() != 16) { return std::nullopt; }
    for (const QChar c : id) {
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f'))) { return std::nullopt; }
    }
    bool valid = false;
    const quint64 result = id.toULongLong(&valid, 16);
    return valid ? std::optional<quint64>(result) : std::nullopt;
}

std::optional<MeasurementSource> measurementSource(const QJsonValue& value)
{
    if (!value.isObject()) { return std::nullopt; }
    const QJsonObject o = value.toObject();
    const std::optional<quint64> id = measurementId(o.value(QStringLiteral("source_id")));
    if (o.size() != 2 || !id) { return std::nullopt; }
    MeasurementSource result{*id, false};
    if (!boolean(o, "tones", result.tones) || (result.tones && !result.sourceId)) { return std::nullopt; }
    return result;
}

std::optional<MeasurementToneConfig> toneConfig(const QJsonValue& value)
{
    if (!value.isObject()) { return std::nullopt; }
    const QJsonObject o = value.toObject();
    const std::optional<quint64> id = measurementId(o.value(QStringLiteral("source_id")));
    quint64 minimum = 0, maximum = 0, center = 0, spacing = 0;
    if (o.size() != 5 || !id || !counter(o, "min_spacing_hz", minimum) || minimum != kMinToneSpacingHz
        || !counter(o, "max_spacing_hz", maximum) || maximum != kMaxToneSpacingHz
        || !counter(o, "center_hz", center) || center != quint64(kToneCenterHz)
        || !counter(o, "spacing_hz", spacing) || spacing < kMinToneSpacingHz || spacing > kMaxToneSpacingHz) {
        return std::nullopt;
    }
    return MeasurementToneConfig{*id, quint32(spacing)};
}

std::optional<FeedbackBinding> feedbackBinding(const QJsonObject& object)
{
    FeedbackBinding binding;
    quint64 channel = 0, pan = 0, gui = 0;
    if (object.size() != 6 || object.value(QStringLiteral("transport")) != QLatin1String("local_dax_iq")
        || !counter(object, "channel", channel) || channel < 1 || channel > 4
        || !counter(object, "pan", pan) || pan == 0 || pan > std::numeric_limits<quint32>::max()
        || !counter(object, "gui_handle", gui) || gui == 0 || gui > std::numeric_limits<quint32>::max()
        || !boolean(object, "valid", binding.valid) || !boolean(object, "failed", binding.failed)
        || (binding.valid && binding.failed)) { return std::nullopt; }
    binding.channel = quint32(channel); binding.pan = quint32(pan); binding.guiHandle = quint32(gui);
    return binding;
}

bool consistent(const Status& s)
{
    const bool armed = s.calibrationState == CalibrationState::Armed;
    const bool running = s.calibrationState == CalibrationState::Running;
    if (s.feedbackBinding && !s.feedbackBinding->valid && (armed || running || s.correcting)) { return false; }
    if (armed || running) {
        if (!s.calibrationControl || s.calibrationSamplesLeft == 0
            || s.calibrationSamplesLeft > 192'000 || running != s.keyed) {
            return false;
        }
    } else if (s.calibrationSamplesLeft != 0) {
        return false;
    }
    if (s.correcting && (!s.registered || !s.keyed || !s.correctionEnabled
                         || s.calibrationRequired || s.state == State::BlockedByRadioApd)) {
        return false;
    }
    if (s.measurement && s.measurement->sourceId
        && (!s.measurementControl || s.sourcePurpose != SourcePurpose::Measurement)) { return false; }
    if (s.measurement && s.measurement->tones && (!s.keyed || !running)) { return false; }
    if (s.measurementControl && !s.calibrationControl) { return false; }
    if (s.sourcePurpose == SourcePurpose::Measurement && !s.measurementControl) { return false; }
    if (s.calibrationRequired && !s.faults.any()) { return false; }
    State expected = State::Calibrating;
    if (!s.registered) { expected = State::Starting; }
    else if (s.feedbackBinding && s.feedbackBinding->failed) { expected = State::FeedbackBindingFailed; }
    else if (s.feedbackBinding && !s.feedbackBinding->valid) { expected = State::FeedbackBindingPending; }
    else if (!s.correctionEnabled) { expected = State::Disabled; }
    else if (s.state == State::BlockedByRadioApd) { expected = State::BlockedByRadioApd; }
    else if (armed) { expected = s.sourcePurpose == SourcePurpose::Measurement ? State::MeasurementArmed : State::CalibrationArmed; }
    else if (s.calibrationRequired) { expected = State::RecalibrationRequired; }
    else if (!s.keyed) { expected = State::Idle; }
    else if (s.correcting) { expected = State::Correcting; }
    return s.state == expected;
}
} // namespace

bool MeasurementToneConfig::valid() const
{
    return spacingHz >= kMinToneSpacingHz && spacingHz <= kMaxToneSpacingHz;
}

bool Faults::any() const
{
    return dispatchDeadlines != 0 || sendErrors != 0 || rejectedInput != 0 || staleIq != 0;
}

std::optional<Request> request(Operation operation)
{
    switch (operation) {
    case Operation::EnableCorrection: return Request{"POST", QStringLiteral("/v1/aetherpd/correction/enable"), 8'000};
    case Operation::BypassCorrection: return Request{"POST", QStringLiteral("/v1/aetherpd/correction/bypass"), 8'000};
    case Operation::Status: return Request{"GET", QStringLiteral("/v1/aetherpd/status"), 2'000};
    case Operation::ArmMeasurement: return Request{"POST", QStringLiteral("/v1/aetherpd/measurement/arm"), 8'000};
    case Operation::ArmCalibration: return Request{"POST", QStringLiteral("/v1/aetherpd/calibration/arm"), 8'000};
    case Operation::CancelCalibration: return Request{"POST", QStringLiteral("/v1/aetherpd/calibration/cancel"), 3'000};
    case Operation::Stop: return Request{"POST", QStringLiteral("/v1/aetherpd/stop"), 3'000};
    case Operation::RestoreReceive: return Request{"POST", QStringLiteral("/v1/aetherpd/receive/restore"), 8'000};
    case Operation::Start: return Request{"POST", QStringLiteral("/v1/aetherpd/start"), 22'000};
    }
    return std::nullopt;
}

Request measurementToneRequest()
{
    return {"POST", QStringLiteral("/v2/aetherpd/measurement/arm"), 8'000};
}

std::optional<QByteArray> measurementToneBody(quint32 spacingHz)
{
    if (spacingHz < kMinToneSpacingHz || spacingHz > kMaxToneSpacingHz) { return std::nullopt; }
    return QJsonDocument(QJsonObject{{QStringLiteral("spacing_hz"), qint64(spacingHz)}}).toJson(QJsonDocument::Compact);
}

Request selectedStartRequest()
{
    return {"POST", QStringLiteral("/v2/aetherpd/start"), 22'000};
}

std::optional<QByteArray> startBody(const StartIdentity& identity)
{
    if (!identity.guiHandle || identity.guiClientId.size() != 36 || (identity.iq && !identity.iq->valid())) {
        return std::nullopt;
    }
    for (qsizetype i = 0; i < 36; ++i) {
        const QChar c = identity.guiClientId.at(i);
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != u'-') { return std::nullopt; }
        } else if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f')
                     || (c >= u'A' && c <= u'F'))) {
            return std::nullopt;
        }
    }
    QJsonObject object{{QStringLiteral("gui_client_id"), identity.guiClientId},
        {QStringLiteral("gui_handle"), static_cast<qint64>(identity.guiHandle)}};
    if (identity.iq) {
        object.insert(QStringLiteral("iq_channel"), qint64(identity.iq->channel));
        object.insert(QStringLiteral("feedback_pan"), qint64(identity.iq->pan));
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

std::optional<Status> parseStatus(const QByteArray& json)
{
    const std::optional<QJsonObject> parsed = object(json);
    if (!parsed) { return std::nullopt; }
    const QJsonObject& o = *parsed;
    Status s;
    const std::optional<State> parsedState = state(o.value(QStringLiteral("state")));
    if (!parsedState || !boolean(o, "available", s.available)) { return std::nullopt; }
    s.state = *parsedState;
    if (!s.available) {
        const bool terminal = s.state == State::Stopped || s.state == State::CleanupUnconfirmed
            || s.state == State::StoppedReceiveUnconfirmed;
        return o.size() == 2 && (terminal || s.state == State::Unavailable)
            ? std::optional<Status>(s) : std::nullopt;
    }
    static const QSet<QString> known{
        QStringLiteral("available"), QStringLiteral("state"), QStringLiteral("registered"),
        QStringLiteral("keyed"), QStringLiteral("correction_control"), QStringLiteral("correction_enabled"), QStringLiteral("correcting"),
        QStringLiteral("calibration_required"), QStringLiteral("calibration_control"),
        QStringLiteral("calibration_state"), QStringLiteral("calibration_samples_left"),
        QStringLiteral("measurement_control"), QStringLiteral("source_purpose"), QStringLiteral("measurement"),
        QStringLiteral("measurement_tone_control"), QStringLiteral("measurement_tone_capture_version"), QStringLiteral("measurement_tone_config"),
        QStringLiteral("input_limited_samples"), QStringLiteral("faults"), QStringLiteral("feedback_binding")};
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
        if (!known.contains(it.key())) { return std::nullopt; }
    }
    if (o.contains(QStringLiteral("feedback_binding"))) {
        const QJsonValue binding = o.value(QStringLiteral("feedback_binding"));
        if (!binding.isObject()) { return std::nullopt; }
        s.feedbackBinding = feedbackBinding(binding.toObject());
        if (!s.feedbackBinding) { return std::nullopt; }
    }
    if (!boolean(o, "registered", s.registered) || !boolean(o, "keyed", s.keyed)
        || !boolean(o, "correction_enabled", s.correctionEnabled) || !boolean(o, "correcting", s.correcting)
        || !boolean(o, "calibration_required", s.calibrationRequired)
        || !boolean(o, "calibration_control", s.calibrationControl)
        || !counter(o, "input_limited_samples", s.inputLimitedSamples)) {
        return std::nullopt;
    }
    if (o.contains(QStringLiteral("correction_control"))
        && (!boolean(o, "correction_control", s.correctionControl)
            || (s.correctionControl && !s.calibrationControl))) { return std::nullopt; }
    const QJsonValue faults = o.value(QStringLiteral("faults"));
    if (!faults.isObject() || faults.toObject().size() != 4) { return std::nullopt; }
    const QJsonObject f = faults.toObject();
    if (!counter(f, "dispatch_deadlines", s.faults.dispatchDeadlines)
        || !counter(f, "send_errors", s.faults.sendErrors)
        || !counter(f, "rejected_input", s.faults.rejectedInput)
        || !counter(f, "stale_iq", s.faults.staleIq)) { return std::nullopt; }
    if (o.contains(QStringLiteral("calibration_state"))) {
        const std::optional<CalibrationState> parsedCalibration = calibrationState(o.value(QStringLiteral("calibration_state")));
        if (!parsedCalibration) { return std::nullopt; }
        s.calibrationState = *parsedCalibration;
    } else if (s.calibrationControl) { return std::nullopt; }
    if (o.contains(QStringLiteral("calibration_samples_left"))) {
        quint64 left = 0;
        if (!counter(o, "calibration_samples_left", left) || left > 192'000) { return std::nullopt; }
        s.calibrationSamplesLeft = static_cast<quint32>(left);
    }
    if (o.contains(QStringLiteral("measurement_control"))
        && !boolean(o, "measurement_control", s.measurementControl)) { return std::nullopt; }
    if (o.contains(QStringLiteral("source_purpose"))) {
        const QJsonValue purpose = o.value(QStringLiteral("source_purpose"));
        if (!purpose.isString()) { return std::nullopt; }
        if (purpose.toString() == QLatin1String("measurement")) { s.sourcePurpose = SourcePurpose::Measurement; }
        else if (purpose.toString() != QLatin1String("calibration")) { return std::nullopt; }
    } else if (s.measurementControl) { return std::nullopt; }
    if (o.contains(QStringLiteral("measurement"))) {
        s.measurement = measurementSource(o.value(QStringLiteral("measurement")));
        if (!s.measurement) { return std::nullopt; }
    }
    if (o.contains(QStringLiteral("measurement_tone_config"))) {
        s.measurementToneConfig = toneConfig(o.value(QStringLiteral("measurement_tone_config")));
        quint64 version = 0;
        if (!s.measurementToneConfig || !counter(o, "measurement_tone_capture_version", version) || version != 3
            || !s.measurement || s.measurement->sourceId != s.measurementToneConfig->sourceId
            || (o.contains(QStringLiteral("measurement_tone_control"))
                && !boolean(o, "measurement_tone_control", s.measurementToneControl))
            || (s.measurementToneControl && !s.measurementControl)) { return std::nullopt; }
    } else if (o.contains(QStringLiteral("measurement_tone_control"))
               || o.contains(QStringLiteral("measurement_tone_capture_version"))) { return std::nullopt; }
    return consistent(s) ? std::optional<Status>(s) : std::nullopt;
}

std::optional<Acknowledgment> parseAcknowledgment(Operation operation, int httpStatus, const QByteArray& json)
{
    const std::optional<QJsonObject> parsed = object(json);
    if (!parsed || parsed->size() != 1 || !parsed->value(QStringLiteral("state")).isString()) {
        return std::nullopt;
    }
    const QString value = parsed->value(QStringLiteral("state")).toString();
    switch (operation) {
    case Operation::EnableCorrection:
        if (httpStatus == 200 && value == QLatin1String("correction_enable_accepted")) { return Acknowledgment::CorrectionEnabled; }
        break;
    case Operation::BypassCorrection:
        if (httpStatus == 200 && value == QLatin1String("correction_bypass_accepted")) { return Acknowledgment::CorrectionBypassed; }
        break;
    case Operation::Start:
        if (httpStatus == 202 && value == QLatin1String("start_requested")) { return Acknowledgment::StartRequested; }
        break;
    case Operation::ArmMeasurement:
        if (httpStatus == 200 && value == QLatin1String("measurement_arm_accepted")) { return Acknowledgment::MeasurementArmAccepted; }
        break;
    case Operation::ArmCalibration:
        if (httpStatus == 200 && value == QLatin1String("calibration_arm_accepted")) { return Acknowledgment::ArmAccepted; }
        break;
    case Operation::CancelCalibration:
        if (httpStatus == 200 && value == QLatin1String("calibration_cancel_accepted")) { return Acknowledgment::CancelAccepted; }
        break;
    case Operation::Stop:
        if (httpStatus == 202 && value == QLatin1String("stop_requested")) { return Acknowledgment::StopRequested; }
        break;
    case Operation::RestoreReceive:
        if (httpStatus == 200 && value == QLatin1String("receive_restored")) { return Acknowledgment::ReceiveRestored; }
        break;
    case Operation::Status: break;
    }
    return std::nullopt;
}
Request captureBeginRequest(bool withSourceIdentity)
{
    return {"POST", withSourceIdentity ? QStringLiteral("/v2/aetherpd/capture") : QStringLiteral("/v1/aetherpd/capture"), 4000};
}

Request captureBeginWithTonesRequest()
{
    return {"POST", QStringLiteral("/v3/aetherpd/capture"), 4000};
}

std::optional<Request> captureChunkRequest(const CaptureManifest& manifest, int chunk)
{
    if (!captureIdValid(manifest.captureId) || chunk < 0 || chunk >= kCaptureSamples / kCaptureChunkSamples) {
        return std::nullopt;
    }
    return Request{"GET", QStringLiteral("/v1/aetherpd/capture/%1/%2").arg(manifest.captureId).arg(chunk), 1500};
}

std::optional<CaptureManifest> parseCaptureManifest(const QByteArray& json)
{
    if (json.size() > 2048) { return std::nullopt; }
    const std::optional<QJsonObject> parsed = object(json);
    if (!parsed || parsed->size() != 10 + int(parsed->contains(QStringLiteral("measurement")))
        + int(parsed->contains(QStringLiteral("source")))
        + int(parsed->contains(QStringLiteral("measurement_tone_config")))) { return std::nullopt; }
    const QJsonObject& o = *parsed;
    CaptureManifest m;
    m.captureId = o.value(QStringLiteral("capture_id")).toString();
    quint64 rate = 0, samples = 0, chunk = 0;
    if (!captureIdValid(m.captureId) || o.value(QStringLiteral("format")).toString() != QLatin1String("f32le-iq-v1")
        || !counter(o, "sample_rate_hz", rate) || (rate != 24000 && rate != 48000 && rate != 96000 && rate != 192000)
        || !counter(o, "center_hz", m.centerHz) || m.centerHz == 0 || m.centerHz > 1'000'000'000
        || !counter(o, "sample_count", samples) || samples != kCaptureSamples
        || !counter(o, "chunk_samples", chunk) || chunk != kCaptureChunkSamples
        || !boolean(o, "keyed", m.keyed) || !boolean(o, "correcting", m.correcting) || (m.correcting && !m.keyed)
        || !timestamp(o, "first_receive_ns", m.firstReceiveNs) || !timestamp(o, "last_receive_ns", m.lastReceiveNs)
        || m.lastReceiveNs < m.firstReceiveNs || m.lastReceiveNs - m.firstReceiveNs > 1'000'000'000) {
        return std::nullopt;
    }
    if (o.contains(QStringLiteral("measurement"))) {
        m.measurement = measurementSource(o.value(QStringLiteral("measurement")));
        if (!m.measurement || (m.measurement->tones && !m.keyed)) { return std::nullopt; }
    }
    if (o.contains(QStringLiteral("source"))) {
        if (!o.value(QStringLiteral("source")).isObject()) { return std::nullopt; }
        const QJsonObject source = o.value(QStringLiteral("source")).toObject();
        quint64 stream = 0;
        bool generationValid = false;
        const QJsonValue generationValue = source.value(QStringLiteral("generation"));
        const QString generationText = generationValue.toString();
        const quint64 generation = generationText.toULongLong(&generationValid);
        if (source.size() != 3 || source.value(QStringLiteral("kind")).toString() != QLatin1String("local_dax_iq")
            || !counter(source, "stream_id", stream) || stream == 0 || stream > std::numeric_limits<quint32>::max()
            || !generationValue.isString() || !generationValid || !generation || generationText != QString::number(generation)) {
            return std::nullopt;
        }
        m.source = CaptureSourceIdentity{quint32(stream), generation};
    }
    if (o.contains(QStringLiteral("measurement_tone_config"))) {
        m.measurementToneConfig = toneConfig(o.value(QStringLiteral("measurement_tone_config")));
        if (!m.measurementToneConfig || !m.measurement || !m.source || !m.measurementToneConfig->sourceId
            || m.measurementToneConfig->sourceId != m.measurement->sourceId) { return std::nullopt; }
    }
    m.sampleRateHz = quint32(rate);
    return m;
}

std::optional<QByteArray> parseCaptureChunk(const QByteArray& json, const CaptureManifest& manifest, int chunk)
{
    if (json.size() > 8192 || !captureChunkRequest(manifest, chunk)) { return std::nullopt; }
    const std::optional<QJsonObject> parsed = object(json);
    if (!parsed || parsed->size() != 3) { return std::nullopt; }
    const QJsonObject& o = *parsed;
    quint64 offset = 0;
    if (o.value(QStringLiteral("capture_id")).toString() != manifest.captureId
        || !counter(o, "offset", offset) || offset != quint64(chunk * kCaptureChunkSamples)
        || !o.value(QStringLiteral("iq_f32le")).isString()) { return std::nullopt; }
    const QString encoded = o.value(QStringLiteral("iq_f32le")).toString();
    const QByteArray raw = QByteArray::fromBase64(encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (raw.size() != 8 * kCaptureChunkSamples || QString::fromLatin1(raw.toBase64()) != encoded) {
        return std::nullopt;
    }
    for (qsizetype i = 0; i < raw.size(); i += 4) {
        if (!std::isfinite(std::bit_cast<float>(qFromLittleEndian<quint32>(raw.constData() + i)))) {
            return std::nullopt;
        }
    }
    return raw;
}
std::optional<Request> testRequest(TestOperation operation, const QString& testId)
{
    if (operation == TestOperation::Begin) {
        if (!testId.isEmpty()) { return std::nullopt; }
        return Request{"POST", QStringLiteral("/v1/aetherpd/test-unkey"), 7500};
    }
    if (!captureIdValid(testId) || testId == QString(32, u'0')) { return std::nullopt; }
    const QString path = QStringLiteral("/v1/aetherpd/test-unkey/") + testId;
    if (operation == TestOperation::Status) { return Request{"GET", path, 500}; }
    if (operation == TestOperation::Stop) { return Request{"POST", path + QStringLiteral("/stop"), 7500}; }
    return std::nullopt;
}

std::optional<QByteArray> testBeginBody(int cleanupAfterMs)
{
    if (cleanupAfterMs < 1 || cleanupAfterMs > 15000) { return std::nullopt; }
    return QJsonDocument(QJsonObject{{QStringLiteral("cleanup_after_ms"), cleanupAfterMs}}).toJson(QJsonDocument::Compact);
}

std::optional<TestObservation> parseTestObservation(const QByteArray& json)
{
    if (json.size() > 512) { return std::nullopt; }
    const std::optional<QJsonObject> parsed = object(json);
    if (!parsed || parsed->size() != 4) { return std::nullopt; }
    TestObservation result;
    result.testId = parsed->value(QStringLiteral("test_id")).toString();
    quint64 remaining = 0;
    if (!testRequest(TestOperation::Status, result.testId)
        || !timestamp(*parsed, "cleanup_due_ms", result.cleanupDueMs)
        || !counter(*parsed, "remaining_ms", remaining) || remaining > 15000) {
        return std::nullopt;
    }
    const QString state = parsed->value(QStringLiteral("state")).toString();
    if (state == QLatin1String("observing") && remaining > 0) { result.state = TestState::Observing; }
    else if (state == QLatin1String("retiring") && remaining == 0) { result.state = TestState::Retiring; }
    else if (state == QLatin1String("ended") && remaining == 0) { result.state = TestState::Ended; }
    else { return std::nullopt; }
    result.remainingMs = static_cast<int>(remaining);
    return result;
}
namespace {
bool installationHex(const QString& value)
{
    if (value.size() != 64 || value == QString(64, u'0')) { return false; }
    for (const QChar c : value) {
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f'))) { return false; }
    }
    return true;
}
bool installationText(const QString& value, bool firmware)
{
    static const QRegularExpression firmwarePattern(QStringLiteral("\\A[0-9]+(?:\\.[0-9]+){3}\\z"));
    static const QRegularExpression serialPattern(QStringLiteral("\\A[A-Za-z0-9.-]{1,63}\\z"));
    return firmware ? value.size() < 28 && firmwarePattern.match(value).hasMatch()
                    : serialPattern.match(value).hasMatch();
}
std::optional<QString> installationField(const QByteArray& bytes, bool firmware)
{
    const qsizetype end = bytes.indexOf('\0');
    if (end < 1 || bytes.mid(end) != QByteArray(bytes.size() - end, '\0')) { return std::nullopt; }
    const QString text = QString::fromLatin1(bytes.first(end));
    return installationText(text, firmware) ? std::optional<QString>(text) : std::nullopt;
}
} // namespace

std::optional<InstallationPlan> parseInstallationPlan(const QByteArray& approved)
{
    if (approved.size() != 352) { return std::nullopt; }
    const bool attempt = approved.startsWith("PDUPGR02");
    if (attempt) {
        if (approved.mid(56, 32) == QByteArray(32, '\0')
            || approved.mid(88, 60) != QByteArray(60, '\0')) { return std::nullopt; }
    } else if (!approved.startsWith("PDUPGR01")) { return std::nullopt; }
    InstallationPlan plan;
    for (const qsizetype offset : {qsizetype(8), qsizetype(148)}) {
        QByteArray record = approved.mid(offset, 140);
        if (attempt && offset == 8) { record.replace(48, 92, approved.mid(196, 92)); }
        const quint32 profile = qFromBigEndian<quint32>(record.constData() + 8);
        const std::optional<QString> firmware = installationField(record.mid(48, 28), true);
        const std::optional<QString> serial = installationField(record.mid(76, 64), false);
        const QString digest = QString::fromLatin1(record.mid(16, 32).toHex());
        if (!record.startsWith("PDAPRV02") || (profile != 1 && profile != 2)
            || record.mid(12, 4) != QByteArray(4, '\0') || !firmware || !serial || !installationHex(digest)) {
            return std::nullopt;
        }
        if (offset == 8) {
            plan.profile = profile; plan.firmware = *firmware; plan.serial = *serial; plan.previousDigest = digest;
        } else {
            if (plan.profile != profile || plan.firmware != *firmware || plan.serial != *serial
                || plan.previousDigest == digest) { return std::nullopt; }
            plan.candidateDigest = digest;
        }
    }
    plan.previousKey = QString::fromLatin1(approved.mid(288, 32).toHex());
    plan.candidateKey = QString::fromLatin1(approved.mid(320, 32).toHex());
    if (!installationHex(plan.previousKey) || !installationHex(plan.candidateKey)) { return std::nullopt; }
    plan.planDigest = QString::fromLatin1(QCryptographicHash::hash(approved, QCryptographicHash::Blake2b_256).toHex());
    return plan;
}

std::optional<InstallationPlan> parseFirstInstallationPlan(const QByteArray& approved,
                                                          const QByteArray& publisherKey)
{
    if (approved.size() != 308 || !approved.startsWith("PDFST001")
        || approved.mid(8, 8) != QByteArray("PDAPRV02")
        || qFromBigEndian<quint32>(approved.constData() + 16) != 1
        || approved.mid(20, 4) != QByteArray(4, '\0')
        || approved.mid(276, 32) != QByteArray(32, '\0')
        || publisherKey.size() != 32 || publisherKey == QByteArray(32, '\0')) { return std::nullopt; }
    for (const int offset : {24, 148, 244}) {
        if (approved.mid(offset, 32) == QByteArray(32, '\0')) { return std::nullopt; }
    }
    const std::optional<QString> firmware = installationField(approved.mid(56, 28), true);
    const std::optional<QString> serial = installationField(approved.mid(84, 64), false);
    const QByteArray hostname = approved.mid(180, 64);
    const qsizetype end = hostname.indexOf('\0');
    static const QRegularExpression hostnamePattern(QStringLiteral("\\A[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\\z"));
    if (!firmware || !serial || end < 1 || hostname.mid(end) != QByteArray(64 - end, '\0')
        || !hostnamePattern.match(QString::fromLatin1(hostname.first(end))).hasMatch()) { return std::nullopt; }
    InstallationPlan plan;
    plan.firstInstall = true;
    plan.profile = 1;
    plan.firmware = *firmware;
    plan.serial = *serial;
    plan.candidateDigest = QString::fromLatin1(approved.mid(24, 32).toHex());
    plan.candidateKey = QString::fromLatin1(publisherKey.toHex());
    plan.planDigest = QString::fromLatin1(QCryptographicHash::hash(approved, QCryptographicHash::Blake2b_256).toHex());
    return plan;
}

std::optional<QByteArray> installationAttempt(const QByteArray& plan, const QByteArray& nonce)
{
    if (!parseInstallationPlan(plan) || nonce.size() != 32 || nonce == QByteArray(32, '\0')) {
        return std::nullopt;
    }
    return QByteArray("PDUPGR02") + plan.mid(8, 48) + nonce + QByteArray(60, '\0') + plan.mid(148);
}

std::optional<InstallationObservation> parseInstallationObservation(const QByteArray& json)
{
    const std::optional<QJsonObject> parsed = object(json);
    if (!parsed || parsed->size() != 8) { return std::nullopt; }
    const QJsonObject& o = *parsed;
    InstallationObservation result;
    quint64 profile = 0;
    if (!boolean(o, "installation_committed", result.committed) || !counter(o, "profile", profile)
        || (profile != 1 && profile != 2)) { return std::nullopt; }
    result.profile = static_cast<quint32>(profile);
    const QString state = o.value(QStringLiteral("state")).toString();
    if (state != (result.committed ? QLatin1String("committed") : QLatin1String("rolled_back"))) { return std::nullopt; }
    result.firstInstall = o.contains(QStringLiteral("first_install_plan_blake2b_256"));
    if (result.firstInstall && (!result.committed || profile != 1
        || o.contains(QStringLiteral("upgrade_plan_blake2b_256")))) { return std::nullopt; }
    result.planDigest = o.value(result.firstInstall ? QStringLiteral("first_install_plan_blake2b_256")
                                                   : QStringLiteral("upgrade_plan_blake2b_256")).toString();
    result.selectedDigest = o.value(QStringLiteral("selected_release_blake2b_256")).toString();
    result.selectedKey = o.value(QStringLiteral("selected_release_key")).toString();
    result.firmware = o.value(QStringLiteral("firmware")).toString();
    result.serial = o.value(QStringLiteral("serial")).toString();
    if (!installationHex(result.planDigest) || !installationHex(result.selectedDigest) || !installationHex(result.selectedKey)
        || !installationText(result.firmware, true) || !installationText(result.serial, false)) { return std::nullopt; }
    return result;
}

InstallationMatch matchInstallation(const InstallationPlan& plan, const InstallationObservation& observed)
{
    if (plan.profile != observed.profile || plan.serial != observed.serial || plan.firmware != observed.firmware) {
        return InstallationMatch::Mismatch;
    }
    if (plan.firstInstall != observed.firstInstall || plan.planDigest != observed.planDigest) {
        return InstallationMatch::OtherPlan;
    }
    if (plan.firstInstall && !observed.committed) { return InstallationMatch::Mismatch; }
    if (observed.selectedDigest != (observed.committed ? plan.candidateDigest : plan.previousDigest)
        || observed.selectedKey != (observed.committed ? plan.candidateKey : plan.previousKey)) {
        return InstallationMatch::Mismatch;
    }
    return observed.committed ? InstallationMatch::Committed : InstallationMatch::RolledBack;
}

} // namespace AetherSDR::aetherpd
