#include "core/AetherPDProtocol.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QtEndian>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>

using namespace AetherSDR::aetherpd;

namespace {
void check(bool condition, const char* reason)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", reason);
        std::exit(1);
    }
}
QJsonObject idle()
{
    return QJsonDocument::fromJson(R"({"available":true,"state":"idle","registered":true,
        "keyed":false,"correction_enabled":true,"correcting":false,
        "calibration_required":false,"calibration_control":true,"calibration_state":"idle",
        "input_limited_samples":0,"faults":{"dispatch_deadlines":0,"send_errors":0,
        "rejected_input":0,"stale_iq":0}})").object();
}
QByteArray encode(const QJsonObject& o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
void rejects(const QJsonObject& o, const char* reason)
{
    check(!parseStatus(encode(o)), reason);
}

void diagnosticProtocol()
{
    const QJsonObject alignment{{"count", 2}, {"truncated", false},
        {"text", "alignment evidence: reason=tx_dispatch mono_ns=123 delay=-128.000"}};
    const QJsonObject source{{"count", 3}, {"truncated", false},
        {"text", "bounded source ended: reason=feedback_invalidated phase=running keyed=1"}};
    const QJsonObject diagnostics{{"version", 1}, {"alignment", alignment}, {"source_end", source}};
    QJsonObject status = idle();
    status["diagnostics"] = diagnostics;
    const auto parsed = parseStatus(encode(status));
    check(parsed && parsed->diagnostics && parsed->diagnostics->alignment.count == 2
        && parsed->diagnostics->sourceEnd.text == source.value("text").toString()
        && !parsed->correcting && !parsed->keyed, "historical diagnostics lost or changed RF state");
    for (const auto& [key, value] : std::array<std::pair<const char*, QJsonValue>, 10>{{
        {"count", -1}, {"count", 1.5}, {"count", 4294967296.0}, {"count", 0},
        {"text", "wrong producer"}, {"text", "alignment evidence: reason=x\n"},
        {"text", "alignment evidence: reason=\"x\""}, {"text", "alignment evidence: reason=\\x"},
        {"truncated", true}, {"command", "xmit 1"}}}) {
        QJsonObject bad = alignment;
        bad[key] = value;
        QJsonObject changed = diagnostics;
        changed["alignment"] = bad;
        status["diagnostics"] = changed;
        rejects(status, "invalid diagnostic accepted");
    }
    QJsonObject changed = diagnostics;
    changed["version"] = 3;
    status["diagnostics"] = changed;
    rejects(status, "unknown diagnostic version accepted");
    QJsonObject maximum = alignment;
    maximum["count"] = 4294967295.0;
    maximum["truncated"] = true;
    const QString prefix = QStringLiteral("alignment evidence: reason=");
    maximum["text"] = prefix + QString(1023-prefix.size(), u'x');
    changed["version"] = 1;
    changed["alignment"] = maximum;
    status["diagnostics"] = changed;
    check(parseStatus(encode(status)).has_value(), "bounded truncation rejected");
    maximum["text"] = maximum.value("text").toString() + u'x';
    changed["alignment"] = maximum;
    status["diagnostics"] = changed;
    rejects(status, "oversized diagnostic accepted");
    changed = diagnostics;
    changed["version"] = 2;
    status["diagnostics"] = changed;
    rejects(status, "version two without reset record accepted");
    const QJsonObject reset{{"count", 1}, {"truncated", false},
        {"text", "alignment evidence: reason=tx_dispatch mono_ns=123"}};
    QJsonObject acquired = alignment;
    acquired["text"] = "alignment evidence: reason=acquisition mono_ns=456";
    changed["alignment"] = acquired;
    changed["alignment_reset"] = reset;
    status["diagnostics"] = changed;
    const auto retained = parseStatus(encode(status));
    check(retained && retained->diagnostics && retained->diagnostics->alignmentReset
        && retained->diagnostics->alignmentReset->text == reset.value("text").toString()
        && retained->diagnostics->alignment.text == acquired.value("text").toString()
        && !retained->correcting && !retained->keyed, "reset and acquisition not both preserved");
    for (const auto& [key, value] : std::array<std::pair<const char*, QJsonValue>, 8>{{
        {"count", 3}, {"count", 0}, {"count", -1}, {"text", "alignment evidence: reason=acquisition mono_ns=1"},
        {"text", "alignment evidence: reason=tx_dispatch\n"}, {"text", "alignment evidence: reason=\"x\""},
        {"truncated", true}, {"command", "xmit 1"}}}) {
        QJsonObject bad = reset;
        bad[key] = value;
        QJsonObject invalid = changed;
        invalid["alignment_reset"] = bad;
        status["diagnostics"] = invalid;
        rejects(status, "invalid reset accepted");
    }
    changed["alignment_reset"] = QJsonObject{{"count", 0}, {"truncated", false}, {"text", ""}};
    status["diagnostics"] = changed;
    check(parseStatus(encode(status)).has_value(), "new process empty reset rejected");
    changed["version"] = 1;
    status["diagnostics"] = changed;
    rejects(status, "version one accepted reset extension");
    const QString fixturePath = qEnvironmentVariable("AETHER_PD_DIAGNOSTIC_PUBLIC");
    if (!fixturePath.isEmpty()) {
        QFile fixture(fixturePath);
        check(fixture.open(QIODevice::ReadOnly), "native/Go diagnostic fixture missing");
        const QByteArray bytes = fixture.readAll();
        const auto composed = parseStatus(bytes);
        check(composed && composed->diagnostics
            && (composed->diagnostics->sourceEnd.count > 0 || composed->diagnostics->alignment.count > 0)
            && !composed->correcting, "native/Go/Qt composition rejected");
        const QJsonObject expected = QJsonDocument::fromJson(bytes).object().value("diagnostics").toObject();
        if (expected.value("version").toInt() == 2) {
            const QJsonObject resetRecord = expected.value("alignment_reset").toObject();
            check(composed->diagnostics->alignmentReset
                && composed->diagnostics->alignmentReset->count == resetRecord.value("count").toDouble()
                && composed->diagnostics->alignmentReset->text == resetRecord.value("text").toString(),
                "native/Go/Qt reset record changed");
        }
    }
}

void feedbackBindingProtocol()
{
    QJsonObject status = idle();
    QJsonObject binding{{"transport", "local_dax_iq"}, {"channel", 3}, {"pan", 1073741826},
        {"gui_handle", 52540638}, {"valid", true}, {"failed", false}};
    status["feedback_binding"] = binding;
    const std::optional<Status> parsed = parseStatus(encode(status));
    check(parsed && parsed->feedbackBinding && parsed->feedbackBinding->channel == 3
        && parsed->feedbackBinding->pan == 1073741826 && parsed->feedbackBinding->guiHandle == 52540638
        && parsed->feedbackBinding->valid, "observed IQ binding lost");
    for (const auto& [key, value] : std::array<std::pair<const char*, QJsonValue>, 11>{{
        {"transport", "audio"}, {"channel", 0}, {"channel", 5}, {"channel", 1.5},
        {"pan", 0}, {"pan", 4294967296.0}, {"gui_handle", 0}, {"valid", QJsonValue::Null},
        {"failed", true}, {"extra", 1}, {"gui_handle", "52540638"}}}) {
        QJsonObject invalid = status; QJsonObject changed = binding; changed[QLatin1String(key)] = value;
        invalid["feedback_binding"] = changed; rejects(invalid, "invalid IQ binding accepted");
    }
    QByteArray duplicate = encode(status);
    duplicate.replace("\"channel\":3", "\"channel\":3,\"channel\":1");
    check(!parseStatus(duplicate), "duplicate IQ channel accepted");
    for (const bool failed : {false, true}) {
        binding["valid"] = false; binding["failed"] = failed;
        status["feedback_binding"] = binding;
        status["state"] = failed ? "feedback_binding_failed" : "feedback_binding_pending";
        const std::optional<Status> changed = parseStatus(encode(status));
        check(changed && changed->state == (failed ? State::FeedbackBindingFailed : State::FeedbackBindingPending), "IQ wait/failure lost");
        QJsonObject armed = status; armed["calibration_state"] = "armed"; armed["calibration_samples_left"] = 96000;
        rejects(armed, "armed source without binding accepted");
        armed = status; armed["correcting"] = true; rejects(armed, "correction without binding accepted");
        armed = status; armed.remove("feedback_binding"); rejects(armed, "binding state without metadata accepted");
    }
}

void configuredTones()
{
    QJsonObject status = idle();
    status["measurement_control"] = true;
    status["source_purpose"] = "measurement";
    status["measurement"] = QJsonObject{{"source_id", "000000000000001f"}, {"tones", false}};
    status["measurement_tone_control"] = true;
    status["measurement_tone_capture_version"] = 3;
    const QJsonObject profile{{"source_id", "000000000000001f"}, {"min_spacing_hz", 200},
        {"max_spacing_hz", 10000}, {"center_hz", 1305}, {"spacing_hz", 1001}};
    status["measurement_tone_config"] = profile;
    const std::optional<Status> parsed = parseStatus(encode(status));
    check(parsed && parsed->measurementToneControl && parsed->measurementToneConfig
        && parsed->measurementToneConfig->lowerOffsetHz() == 804.5
        && parsed->measurementToneConfig->upperOffsetHz() == 1805.5, "half-Hz configured tones lost precision");
    check(parsed && parsed->captureBatchVersion == 0, "legacy status invented batch support");
    status["measurement_capture_batch_version"] = 1;
    const std::optional<Status> batch = parseStatus(encode(status));
    check(batch && batch->captureBatchVersion == 1, "batch capture capability lost");
    for (const QJsonValue& version : {QJsonValue(0), QJsonValue(2), QJsonValue(1.5),
             QJsonValue("1"), QJsonValue(true), QJsonValue(QJsonValue::Null)}) {
        QJsonObject invalid = status; invalid["measurement_capture_batch_version"] = version;
        rejects(invalid, "invalid batch capability accepted");
    }
    QJsonObject disabled = status;
    disabled["measurement_control"] = false;
    disabled["measurement_tone_control"] = false;
    disabled["source_purpose"] = "calibration";
    disabled["measurement"] = QJsonObject{{"source_id", "0000000000000000"}, {"tones", false}};
    QJsonObject inactiveProfile = profile; inactiveProfile["source_id"] = "0000000000000000";
    disabled["measurement_tone_config"] = inactiveProfile;
    check(parseStatus(encode(disabled)).has_value(), "batch support was confused with source control permission");
    for (const char* missing : {"measurement_tone_config", "measurement_tone_capture_version", "measurement"}) {
        QJsonObject invalid = status; invalid.remove(QLatin1String(missing)); rejects(invalid, "partial tone status accepted");
    }
    for (const auto& [key, value] : std::array<std::pair<const char*, QJsonValue>, 7>{{
        {"source_id", "0000000000000020"}, {"spacing_hz", 199}, {"spacing_hz", 10001},
        {"spacing_hz", 1001.5}, {"spacing_hz", QJsonValue::Null}, {"center_hz", 1306}, {"extra", 1}}}) {
        QJsonObject invalid = status; QJsonObject changed = profile; changed[QLatin1String(key)] = value;
        invalid["measurement_tone_config"] = changed; rejects(invalid, "invalid or mismatched tone configuration accepted");
    }
    QByteArray duplicate = encode(status);
    duplicate.replace("\"spacing_hz\":1001", "\"spacing_hz\":1001,\"spacing_hz\":1190");
    check(!parseStatus(duplicate), "duplicate profile spacing accepted");
    for (const quint32 spacing : {200U, 1001U, 1190U, 2610U, 10000U}) {
        const MeasurementToneConfig tones{31, spacing};
        check(tones.valid() && tones.upperOffsetHz() - tones.lowerOffsetHz() == spacing, "tone spacing changed");
        check(measurementToneBody(spacing).has_value(), "valid spacing request rejected");
    }
    for (const quint32 spacing : {0U, 199U, 10001U, std::numeric_limits<quint32>::max()}) {
        check(!measurementToneBody(spacing), "unbounded spacing request accepted");
    }
    check(measurementToneRequest().path == QStringLiteral("/v2/aetherpd/measurement/arm")
        && captureBeginWithTonesRequest().path == QStringLiteral("/v3/aetherpd/capture"), "configured versions wrong");
    QJsonObject captured{{"capture_id", "0123456789abcdef0123456789abcdef"}, {"format", "f32le-iq-v1"},
        {"sample_rate_hz", 48000}, {"center_hz", 7150000}, {"sample_count", 16384}, {"chunk_samples", 512},
        {"keyed", true}, {"correcting", true}, {"first_receive_ns", "1000000000"}, {"last_receive_ns", "1085000000"},
        {"source", QJsonObject{{"kind", "local_dax_iq"}, {"stream_id", 1}, {"generation", "7"}}},
        {"measurement", QJsonObject{{"source_id", "000000000000001f"}, {"tones", true}}},
        {"measurement_tone_config", profile}};
    const std::optional<CaptureManifest> manifest = parseCaptureManifest(encode(captured));
    check(manifest && manifest->measurementToneConfig == parsed->measurementToneConfig, "capture profile did not survive parsing");
    captured["measurement"] = QJsonObject{{"source_id", "0000000000000020"}, {"tones", true}};
    check(!parseCaptureManifest(encode(captured)), "captured profile adopted another source identity");
}

void captureProtocol()
{
    const QByteArray text = R"({"capture_id":"0123456789abcdef0123456789abcdef","format":"f32le-iq-v1",
        "sample_rate_hz":192000,"center_hz":7150000,"sample_count":16384,"chunk_samples":512,
        "keyed":true,"correcting":true,"first_receive_ns":"1000000000","last_receive_ns":"1085000000"})";
    const std::optional<CaptureManifest> manifest = parseCaptureManifest(text);
    check(manifest && manifest->centerHz == 7150000 && manifest->sampleRateHz == 192000, "capture manifest rejected");
    check(captureBeginRequest().method == "POST", "capture begin is not POST");
    check(captureChunkRequest(*manifest, 31)->path.endsWith(QStringLiteral("/31")), "last chunk request missing");
    check(!captureChunkRequest(*manifest, -1) && !captureChunkRequest(*manifest, 32), "unbounded chunk request");
    const QJsonObject good = QJsonDocument::fromJson(text).object();
    check(captureBeginRequest(true).path == QStringLiteral("/v2/aetherpd/capture")
        && captureBeginRequest().path == QStringLiteral("/v1/aetherpd/capture"), "capture versions mixed");
    QJsonObject identity = good;
    const QJsonObject validSource{{"kind", "local_dax_iq"}, {"stream_id", 1073741825}, {"generation", "18446744073709551615"}};
    identity["source"] = validSource;
    const std::optional<CaptureManifest> identified = parseCaptureManifest(encode(identity));
    check(identified && identified->source && identified->source->streamId == 0x40000001
        && identified->source->generation == std::numeric_limits<quint64>::max(), "sampled stream identity lost precision");
    for (const auto& [key, value] : std::array<std::pair<const char*, QJsonValue>, 8>{{
        {"kind", "audio"}, {"stream_id", 0}, {"stream_id", 4294967296.0}, {"stream_id", "1"},
        {"generation", "0"}, {"generation", "01"}, {"generation", 7}, {"generation", "18446744073709551616"}}}) {
        QJsonObject invalid = validSource;
        invalid[QLatin1String(key)] = value;
        identity["source"] = invalid;
        check(!parseCaptureManifest(encode(identity)), "malformed stream identity accepted");
    }
    identity["source"] = QJsonValue::Null;
    check(!parseCaptureManifest(encode(identity)), "null stream identity accepted");
    QJsonObject measured = good;
    measured["measurement"] = QJsonObject{{"source_id", "000000000000001f"}, {"tones", true}};
    const std::optional<CaptureManifest> sourced = parseCaptureManifest(encode(measured));
    check(sourced && sourced->measurement && sourced->measurement->sourceId == 31 && sourced->measurement->tones,
        "measurement capture provenance lost");
    for (const QJsonValue& invalid : {QJsonValue(QJsonValue::Null), QJsonValue(QJsonObject{{"source_id", "0000000000000000"}, {"tones", true}}),
            QJsonValue(QJsonObject{{"source_id", "000000000000001f"}, {"tones", "yes"}})}) {
        measured["measurement"] = invalid;
        check(!parseCaptureManifest(encode(measured)), "malformed measurement capture provenance accepted");
    }

    for (const QString& key : good.keys()) {
        QJsonObject bad = good;
        bad.remove(key);
        check(!parseCaptureManifest(encode(bad)), "missing capture field accepted");
        bad = good; bad[key] = QJsonValue::Null;
        check(!parseCaptureManifest(encode(bad)), "null capture field accepted");
    }
    for (const auto& [key, value] : std::array<std::pair<const char*, QJsonValue>, 9>{{
        {"capture_id", "../other"}, {"sample_rate_hz", 192001}, {"center_hz", 0},
        {"sample_count", 32768}, {"chunk_samples", 1024}, {"keyed", false},
        {"first_receive_ns", "01000000000"}, {"last_receive_ns", "999999999"},
        {"last_receive_ns", "9223372036854775808"}}}) {
        QJsonObject bad = good; bad[QLatin1String(key)] = value;
        check(!parseCaptureManifest(encode(bad)), "invalid capture metadata accepted");
    }
    QByteArray duplicate = encode(good); duplicate.chop(1); duplicate += ",\"keyed\":true}";
    check(!parseCaptureManifest(duplicate), "duplicate capture metadata accepted");
    QByteArray raw(4096, '\0');
    for (qsizetype i = 0; i < raw.size(); i += 8) {
        qToLittleEndian<quint32>(0x3f000000, raw.data() + i);
        qToLittleEndian<quint32>(0xbe800000, raw.data() + i + 4);
    }
    QJsonObject chunk{{"capture_id", manifest->captureId}, {"offset", 512},
                      {"iq_f32le", QString::fromLatin1(raw.toBase64())}};
    check(parseCaptureChunk(encode(chunk), *manifest, 1) == raw, "raw chunk changed");
    check(!parseCaptureChunk(encode(chunk), *manifest, 0), "wrong offset accepted");
    QJsonObject bad = chunk; bad[QStringLiteral("capture_id")] = QString(32, u'0');
    check(!parseCaptureChunk(encode(bad), *manifest, 1), "foreign capture accepted");
    bad = chunk; bad[QStringLiteral("iq_f32le")] = QString::fromLatin1(raw.toBase64()) + u'\n';
    check(!parseCaptureChunk(encode(bad), *manifest, 1), "noncanonical base64 accepted");
    qToLittleEndian<quint32>(0x7fc00000, raw.data() + raw.size() - 4);
    bad = chunk; bad[QStringLiteral("iq_f32le")] = QString::fromLatin1(raw.toBase64());
    check(!parseCaptureChunk(encode(bad), *manifest, 1), "nonfinite capture accepted");

    const QString directory = qEnvironmentVariable("AETHER_PD_CAPTURE_FIXTURE_DIR");
    if (!directory.isEmpty()) {
        QFile sourceFile(directory + QStringLiteral("/source/manifest.json"));
        check(sourceFile.open(QIODevice::ReadOnly), "Go source-identity capture manifest missing");
        const std::optional<CaptureManifest> sourceManifest = parseCaptureManifest(sourceFile.readAll());
        check(sourceManifest && sourceManifest->source && sourceManifest->source->streamId == 0x40000001
            && sourceManifest->source->generation == 7, "native/Go/desktop stream identity changed");
        QFile file(directory + QStringLiteral("/manifest.json"));
        check(file.open(QIODevice::ReadOnly), "Go capture manifest missing");
        const std::optional<CaptureManifest> native = parseCaptureManifest(file.readAll());
        check(native.has_value(), "Go capture manifest rejected");
        const QString expectedSpacing = qEnvironmentVariable("AETHER_PD_EXPECT_TONE_SPACING");
        if (!expectedSpacing.isEmpty()) {
            check(native->measurementToneConfig && native->measurementToneConfig->spacingHz == expectedSpacing.toUInt()
                && sourceManifest->measurementToneConfig == native->measurementToneConfig,
                "native/Go/desktop confirmed tone profile changed");
        }
        check(native->measurement && native->measurement->sourceId == 31 && native->measurement->tones, "native/Go measurement provenance changed");
        for (int index = 0; index < 32; ++index) {
            QFile part(directory + QStringLiteral("/chunk-%1.json").arg(index));
            check(part.open(QIODevice::ReadOnly), "Go capture chunk missing");
            const std::optional<QByteArray> bytes = parseCaptureChunk(part.readAll(), *native, index);
            check(bytes.has_value(), "Go capture chunk rejected");
            for (qsizetype i = 0; i < bytes->size(); i += 8) {
                check(qFromLittleEndian<quint32>(bytes->constData() + i) == 0x3f000000
                    && qFromLittleEndian<quint32>(bytes->constData() + i + 4) == 0xbe800000,
                    "C/Go/desktop raw samples changed");
            }
        }
    }
}
} // namespace

void batchProtocol()
{
    const QString id(32, u'a');
    QJsonArray frames;
    for (int i = 0; i < 4; ++i) {
        frames.append(QJsonObject{{"capture_id",id},{"format","f32le-iq-v1"},{"sample_rate_hz",48000},{"center_hz",7150000},
            {"sample_count",16384},{"chunk_samples",512},{"keyed",true},{"correcting",true},
            {"first_receive_ns",QString::number(1000000000ULL+quint64(i)*341000000)},
            {"last_receive_ns",QString::number(1341000000ULL+quint64(i)*341000000)},
            {"source",QJsonObject{{"kind","local_dax_iq"},{"stream_id",1073741825},{"generation","7"}}},
            {"feedback_binding",QJsonObject{{"transport","local_dax_iq"},{"channel",3},{"pan",1073741826},{"gui_handle",7},{"valid",true},{"failed",false}}},
            {"measurement",QJsonObject{{"source_id","000000000000001f"},{"tones",true}}},
            {"measurement_tone_config",QJsonObject{{"source_id","000000000000001f"},{"min_spacing_hz",200},{"max_spacing_hz",10000},{"center_hz",1305},{"spacing_hz",1190}}}});
    }
    const QJsonObject envelope{{"capture_id",id},{"frames",frames}};
    const QByteArray good = encode(envelope);
    const auto batch = parseCaptureBatchManifest(good,4);
    check(batch && batch->frames.size()==4, "valid batch rejected");
    check(captureBatchBeginRequest(4)->path==QStringLiteral("/v4/aetherpd/capture/4")
        && captureBatchChunkRequest(*batch,3,31)->path.endsWith("/3/31"), "batch request wrong");
    check(!captureBatchBeginRequest(0) && !captureBatchBeginRequest(5)
        && !captureBatchChunkRequest(*batch,-1,0) && !captureBatchChunkRequest(*batch,4,0)
        && !captureBatchChunkRequest(*batch,0,32), "unbounded batch request");
    for (int count : {0,1,3,5}) { check(!parseCaptureBatchManifest(good,count), "wrong batch count accepted"); }
    for (qsizetype i=0;i<good.size();++i) { check(!parseCaptureBatchManifest(good.first(i),4), "truncated batch accepted"); }
    for (const char* key : {"center_hz","correcting","source","feedback_binding","measurement_tone_config"}) {
        QJsonObject changed=frames[3].toObject(); changed[QLatin1String(key)]=QJsonValue::Null;
        QJsonArray bad=frames;bad[3]=changed;
        check(!parseCaptureBatchManifest(encode({{"capture_id",id},{"frames",bad}}),4), "partial or mixed batch accepted");
    }
    QByteArray duplicate=good;
    duplicate.replace("\"stream_id\":1073741825", "\"stream_id\":1,\"stream_id\":1073741825");
    check(!parseCaptureBatchManifest(duplicate,4), "nested duplicate batch key accepted");
    QJsonObject chunk{{"capture_id",id},{"frame_index",3},{"offset",31*512},{"iq_f32le",QString::fromLatin1(QByteArray(4096,'\0').toBase64())}};
    check(parseCaptureBatchChunk(encode(chunk),*batch,3,31).has_value(), "batch chunk rejected");
    check(!parseCaptureBatchChunk(encode(chunk),*batch,2,31), "chunk moved between frames");
    chunk["frame_index"]=3.5;
    check(!parseCaptureBatchChunk(encode(chunk),*batch,3,31), "fractional frame index accepted");
    const QString directory=qEnvironmentVariable("AETHER_PD_CAPTURE_BATCH_FIXTURE_DIR");
    if (!directory.isEmpty()) {
        QFile file(directory+"/manifest.json");check(file.open(QIODevice::ReadOnly),"native batch manifest missing");
        const auto native=parseCaptureBatchManifest(file.readAll(),4);check(native.has_value(),"native Go batch rejected by Qt");
        for (int f=0;f<4;++f) {
            for (int c=0;c<32;++c) {
                QFile part(directory+QStringLiteral("/chunk-%1-%2.json").arg(f).arg(c));check(part.open(QIODevice::ReadOnly),"native batch chunk missing");
                const auto bytes=parseCaptureBatchChunk(part.readAll(),*native,f,c);check(bytes.has_value(),"native Go batch chunk rejected by Qt");
                for (int i=0;i<512;++i) {
                    const quint32 index=quint32(f*16384+c*512+i);
                    check(qFromLittleEndian<quint32>(bytes->constData()+8*i)==0x3e000000+index
                        && qFromLittleEndian<quint32>(bytes->constData()+8*i+4)==0xbe000000+index,"native C/Go/Qt sample differs");
                }
            }
        }
    }
}

void ownerKeyProtocol()
{
    const QString installation(64, u'c'), candidate(64, u'b');
    const QByteArray hash = QCryptographicHash::hash(candidate.toLatin1(), QCryptographicHash::Sha256);
    const std::optional<QByteArray> body = ownerKeyBody(installation, candidate);
    check(body.has_value(), "valid owner replacement body rejected");
    const QJsonObject request = QJsonDocument::fromJson(*body).object();
    check(request.size() == 2 && request.value("installation_id").toString() == installation
        && request.value("new_token").toString() == candidate, "owner request changed identity or credential");
    for (const QString& invalid : {QString{}, QString(64, u'0'), QString(63, u'b'), QString(64, u'B'), QString(64, u'g')}) {
        check(!ownerKeyBody(invalid, candidate) && !ownerKeyBody(installation, invalid), "invalid owner identity or candidate encoded");
    }
    const QJsonObject value{{"state", "owner_key_current"}, {"installation_id", installation},
        {"owner_key_sha256", QString::fromLatin1(hash.toHex())}};
    const QByteArray valid = encode(value);
    const std::optional<OwnerKeyReceipt> receipt = parseOwnerKeyReceipt(valid);
    check(receipt && receipt->installationId == installation && receipt->tokenSha256 == hash,
          "owner receipt lost exact installation or key commitment");
    for (const auto& [key, replacement] : std::array<std::pair<const char*, QJsonValue>, 9>{{
        {"state", "accepted"}, {"state", false}, {"installation_id", "wrong"}, {"installation_id", QJsonValue()},
        {"owner_key_sha256", QString(64, u'0')}, {"owner_key_sha256", QString(64, u'A')},
        {"owner_key_sha256", 1}, {"owner_key_sha256", QString(65, u'a')}, {"extra", "ignored"}}}) {
        QJsonObject bad = value;
        bad[key] = replacement;
        check(!parseOwnerKeyReceipt(encode(bad)), "invalid owner receipt accepted");
    }
    for (const QByteArray& invalid : {QByteArray{}, QByteArray("null"), valid + "{}", valid + QByteArray(513, ' '),
        QByteArray(valid).insert(1, "\"state\":\"accepted\","),
        QByteArray(valid).insert(1, "\"st\\u0061te\":\"accepted\",")}) {
        check(!parseOwnerKeyReceipt(invalid), "duplicate/trailing/oversized owner receipt accepted");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ownerKeyProtocol();
    diagnosticProtocol();
    configuredTones();
    feedbackBindingProtocol();
    captureProtocol();
    batchProtocol();
    for (const char* name : {"idle", "armed", "correcting", "unavailable"}) {
        QFile fixture(QString::fromUtf8(AETHERPD_TESTDATA_DIR) + QLatin1Char('/')
                      + QLatin1String(name) + QStringLiteral(".json"));
        check(fixture.open(QIODevice::ReadOnly), "shared measured fixture missing");
        check(parseStatus(fixture.readAll()).has_value(), "measured Go HTTP status contract rejected");
    }
    const std::array<Operation, 9> operations{Operation::Status, Operation::ArmCalibration,
        Operation::CancelCalibration, Operation::Stop, Operation::RestoreReceive, Operation::Start, Operation::ArmMeasurement, Operation::EnableCorrection, Operation::BypassCorrection};
    const std::array<const char*, 9> paths{"/v1/aetherpd/status", "/v1/aetherpd/calibration/arm",
        "/v1/aetherpd/calibration/cancel", "/v1/aetherpd/stop", "/v1/aetherpd/receive/restore", "/v1/aetherpd/start", "/v1/aetherpd/measurement/arm", "/v1/aetherpd/correction/enable", "/v1/aetherpd/correction/bypass"};
    for (std::size_t i = 0; i < operations.size(); ++i) {
        const std::optional<Request> r = request(operations[i]);
        check(r && r->path == QLatin1String(paths[i]) && r->timeoutMs > 0 && r->timeoutMs <= (operations[i] == Operation::Start ? 22'000 : 8'000),
              "fixed bounded request missing");
        check(r->method == (i == 0 ? "GET" : "POST"), "wrong request method");
    }
    check(!request(static_cast<Operation>(99)), "unknown operation accepted");
    const StartIdentity identity{QStringLiteral("0D3EA0E8-B052-4AC6-9D32-D96918634469"), 0x0321b4de};
    const std::optional<QByteArray> start = startBody(identity);
    check(start.has_value(), "valid start identity rejected");
    const QJsonObject fields = QJsonDocument::fromJson(*start).object();
    check(fields.size() == 2 && fields.value(QStringLiteral("gui_client_id")).toString() == identity.guiClientId
        && fields.value(QStringLiteral("gui_handle")).toInteger() == 52'540'638,
        "start body must carry exactly the UUID and unsigned numeric handle");
    check(startBody({identity.guiClientId, 0xffffffffU}).has_value(), "uint32 maximum handle lost precision");
    check(!startBody({identity.guiClientId, 0}), "zero handle admitted");
    for (const quint32 channel : {1U, 2U, 3U, 4U}) {
        StartIdentity selected = identity; selected.iq = IQSelection{channel, 0xffffffffU};
        const std::optional<QByteArray> body = startBody(selected);
        check(body.has_value(), "selected start refused");
        const QJsonObject fields = QJsonDocument::fromJson(*body).object();
        check(fields.size() == 4 && fields.value("iq_channel").toInteger() == channel
            && fields.value("feedback_pan").toInteger() == 0xffffffffU, "selected start changed channel or pan precision");
    }
    for (const IQSelection invalid : {IQSelection{0, 1}, IQSelection{5, 1}, IQSelection{1, 0}}) {
        StartIdentity selected = identity; selected.iq = invalid;
        check(!startBody(selected), "invalid explicit selection fell back to legacy");
    }
    check(selectedStartRequest().path == QStringLiteral("/v2/aetherpd/start")
        && selectedStartRequest().timeoutMs == 22000, "selected startup version/deadline wrong");

    for (const QString& bad : {QString{}, QStringLiteral("owned-gui"),
             QStringLiteral("{0D3EA0E8-B052-4AC6-9D32-D96918634469}"),
             QStringLiteral("0D3EA0E8/B052-4AC6-9D32-D96918634469"),
             QStringLiteral("0D3EA0E8-B052-4AC6-9D32-D9691863446g")}) {
        check(!startBody({bad, 7}), "malformed start UUID admitted");
    }
    check(!parseAcknowledgment(Operation::Start, 200, R"({"state":"start_requested"})"),
          "start receipt accepted with completion status");

    const std::optional<Status> initial = parseStatus(encode(idle()));
    check(initial && initial->available && initial->state == State::Idle
          && !initial->keyed && !initial->correcting && !initial->faults.any(), "idle status rejected");
    check(parseStatus(R"({"available":false,"state":"unavailable"})")->state == State::Unavailable,
          "unavailable status rejected");
    check(!parseStatus(R"({"available":false,"state":"unavailable","correcting":true})"),
          "unavailable status carried contradictory data");
    for (const auto& [wire, expected] : std::array<std::pair<const char*, State>, 3>{{
             {"stopped", State::Stopped}, {"cleanup_unconfirmed", State::CleanupUnconfirmed},
             {"stopped_receive_unconfirmed", State::StoppedReceiveUnconfirmed}}}) {
        QJsonObject terminal{{"available", false}, {"state", QLatin1String(wire)}};
        const auto completion = parseStatus(encode(terminal));
        check(completion && completion->state == expected && !completion->available,
              "supervisor completion rejected");
        terminal[QStringLiteral("keyed")] = false;
        rejects(terminal, "supervisor completion carried fabricated RF telemetry");
        terminal.remove(QStringLiteral("keyed"));
        terminal[QStringLiteral("available")] = true;
        rejects(terminal, "terminal session marked available");
        QJsonObject mixed = idle(); mixed[QStringLiteral("state")] = QLatin1String(wire);
        rejects(mixed, "live DSP state mixed with supervisor completion");
    }
    for (const QString& key : idle().keys()) {
        QJsonObject missing = idle();
        missing.remove(key);
        rejects(missing, "required field absence accepted");
    }
    for (const char* key : {"available", "registered", "keyed", "correction_enabled", "correcting",
                            "calibration_required", "calibration_control"}) {
        for (const QJsonValue& bad : {QJsonValue(0), QJsonValue(QStringLiteral("true")), QJsonValue(QJsonValue::Null)}) {
            QJsonObject o = idle(); o[QLatin1String(key)] = bad;
            rejects(o, "nonboolean flag accepted");
        }
    }
    for (const QByteArray& value : {QByteArray("-1"), QByteArray("-0"), QByteArray("0.5"),
            QByteArray("1e0"), QByteArray("9007199254740991.1"), QByteArray("9007199254740992"),
            QByteArray("18446744073709551616"), QByteArray("null"), QByteArray("true"), QByteArray("\"1\"")}) {
        QByteArray bytes = encode(idle());
        bytes.replace("\"input_limited_samples\":0", "\"input_limited_samples\":" + value);
        check(!parseStatus(bytes), "invalid or imprecise counter accepted");
        bytes = encode(idle());
        bytes.replace("\"stale_iq\":0", "\"stale_iq\":" + value);
        check(!parseStatus(bytes), "invalid nested fault counter accepted");
    }
    for (const QByteArray& key : {QByteArray("state"), QByteArray("\\u0073tate")}) {
        QByteArray bytes = encode(idle());
        bytes.replace("\"state\":\"idle\"", "\"state\":\"correcting\",\"" + key + "\":\"idle\"");
        check(!parseStatus(bytes), "duplicate equivalent state accepted");
    }
    QByteArray duplicate = encode(idle());
    duplicate.replace("\"stale_iq\":0", "\"stale_iq\":2,\"stale_iq\":0");
    check(!parseStatus(duplicate), "duplicate fault accepted");
    for (const QByteArray& bad : {QByteArray(), QByteArray("[]"), QByteArray("{} {}"), QByteArray(16385, ' '),
            QByteArray("{\"available\":true,\"state\":\"new_unknown_state\"}")}) {
        check(!parseStatus(bad), "invalid framing or unknown state accepted");
    }
    QJsonObject o = idle(); o[QStringLiteral("keyed")] = true; o[QStringLiteral("correcting")] = true;
    o[QStringLiteral("state")] = QStringLiteral("correcting");
    check(parseStatus(encode(o))->correcting, "correcting status rejected");
    o[QStringLiteral("keyed")] = false; rejects(o, "unkeyed correction accepted");
    o = idle(); o[QStringLiteral("correcting")] = true; rejects(o, "idle correction accepted");
    o = idle(); o[QStringLiteral("correction_enabled")] = false; rejects(o, "state disagrees with disabled correction");
    o[QStringLiteral("state")] = QStringLiteral("disabled");
    check(parseStatus(encode(o))->state == State::Disabled, "disabled state rejected");
    o = idle(); o[QStringLiteral("registered")] = false; o[QStringLiteral("state")] = QStringLiteral("starting");
    check(parseStatus(encode(o))->state == State::Starting, "starting state rejected");
    o = idle(); o[QStringLiteral("state")] = QStringLiteral("blocked_by_radio_apd");
    check(parseStatus(encode(o))->state == State::BlockedByRadioApd, "native APD block rejected");
    o[QStringLiteral("correcting")] = true; o[QStringLiteral("keyed")] = true;
    rejects(o, "correction accepted while native APD blocks it");
    o = idle(); o[QStringLiteral("calibration_state")] = QStringLiteral("armed");
    o[QStringLiteral("calibration_samples_left")] = 192'000; o[QStringLiteral("state")] = QStringLiteral("calibration_armed");
    check(parseStatus(encode(o))->calibrationSamplesLeft == 192'000, "armed calibration rejected");
    o[QStringLiteral("keyed")] = true; rejects(o, "armed state accepted while keyed");
    o[QStringLiteral("keyed")] = false; o[QStringLiteral("calibration_control")] = false;
    rejects(o, "armed state without source control accepted");
    o[QStringLiteral("calibration_control")] = true; o[QStringLiteral("calibration_samples_left")] = 192'001;
    rejects(o, "oversized source accepted");
    o[QStringLiteral("calibration_samples_left")] = 0; rejects(o, "empty armed source accepted");
    o = idle(); o[QStringLiteral("calibration_state")] = QStringLiteral("running");
    o[QStringLiteral("calibration_samples_left")] = 12'000; o[QStringLiteral("keyed")] = true;
    o[QStringLiteral("state")] = QStringLiteral("calibrating");
    check(parseStatus(encode(o))->calibrationState == CalibrationState::Running, "running source rejected");
    o[QStringLiteral("keyed")] = false; rejects(o, "running source while idle accepted");
    o = idle(); o[QStringLiteral("calibration_required")] = true;
    o[QStringLiteral("state")] = QStringLiteral("recalibration_required");
    rejects(o, "unexplained recalibration flag accepted");
    QJsonObject faults = o[QStringLiteral("faults")].toObject(); faults[QStringLiteral("stale_iq")] = 1;
    o[QStringLiteral("faults")] = faults;
    check(parseStatus(encode(o))->calibrationRequired, "fault state rejected");
    o = idle(); o[QStringLiteral("keyed")] = true;
    o[QStringLiteral("state")] = QStringLiteral("calibrating");
    faults = o[QStringLiteral("faults")].toObject();
    faults[QStringLiteral("dispatch_deadlines")] = 1;
    o[QStringLiteral("faults")] = faults;
    check(parseStatus(encode(o)).has_value(), "validated recovery with retained fault counter rejected");
    o = idle(); o[QStringLiteral("calibration_control")] = false; o.remove(QStringLiteral("calibration_state"));
    check(parseStatus(encode(o)).has_value(), "legacy source-control absence rejected");

    o = idle();
    check(!parseStatus(encode(o))->measurementControl, "legacy status advertises measurement capability");
    o[QStringLiteral("measurement_control")] = true;
    rejects(o, "measurement capability without source purpose accepted");
    o[QStringLiteral("source_purpose")] = QStringLiteral("measurement");
    o[QStringLiteral("calibration_state")] = QStringLiteral("armed");
    o[QStringLiteral("calibration_samples_left")] = 192000;
    o[QStringLiteral("state")] = QStringLiteral("measurement_armed");
    check(parseStatus(encode(o))->sourcePurpose == SourcePurpose::Measurement, "measurement arm purpose lost");
    o[QStringLiteral("correction_enabled")] = false;
    o[QStringLiteral("state")] = QStringLiteral("disabled");
    check(parseStatus(encode(o)).has_value(), "uncorrected measurement rejected");
    for (const QJsonValue& bad : {QJsonValue(QJsonValue::Null), QJsonValue(17), QJsonValue("unexpected")}) {
        QJsonObject invalid = o;
        invalid[QStringLiteral("measurement_control")] = bad;
        rejects(invalid, "malformed measurement capability accepted");
        invalid = o;
        invalid[QStringLiteral("source_purpose")] = bad;
        rejects(invalid, "malformed source purpose accepted");
    }
    o[QStringLiteral("measurement_control")] = false;
    rejects(o, "measurement purpose admitted without capability");

    QJsonObject correction = idle();
    check(!parseStatus(encode(correction))->correctionControl, "legacy status advertised correction selection");
    correction["correction_control"] = true;
    check(parseStatus(encode(correction))->correctionControl, "correction capability lost");
    for (const QJsonValue& bad : {QJsonValue(QJsonValue::Null), QJsonValue("true"), QJsonValue(1)}) {
        correction["correction_control"] = bad;
        rejects(correction, "malformed correction capability accepted");
    }
    correction["correction_control"] = true;
    correction["calibration_control"] = false;
    rejects(correction, "correction selection advertised without private source control");

    struct AckCase { Operation operation; int code; const char* body; Acknowledgment expected; };
    const std::array<AckCase, 8> replies{{
        {Operation::EnableCorrection, 200, R"({"state":"correction_enable_accepted"})", Acknowledgment::CorrectionEnabled},
        {Operation::BypassCorrection, 200, R"({"state":"correction_bypass_accepted"})", Acknowledgment::CorrectionBypassed},
        {Operation::ArmMeasurement, 200, R"({"state":"measurement_arm_accepted"})", Acknowledgment::MeasurementArmAccepted},
        {Operation::Start, 202, R"({"state":"start_requested"})", Acknowledgment::StartRequested},
        {Operation::ArmCalibration, 200, R"({"state":"calibration_arm_accepted"})", Acknowledgment::ArmAccepted},
        {Operation::CancelCalibration, 200, R"({"state":"calibration_cancel_accepted"})", Acknowledgment::CancelAccepted},
        {Operation::Stop, 202, R"({"state":"stop_requested"})", Acknowledgment::StopRequested},
        {Operation::RestoreReceive, 200, R"({"state":"receive_restored"})", Acknowledgment::ReceiveRestored}}};
    for (const AckCase& a : replies) {
        check(parseAcknowledgment(a.operation, a.code, a.body) == a.expected, "valid acknowledgement rejected");
        for (const int badCode : {0, 204, 302, 401, 409, 503}) {
            check(!parseAcknowledgment(a.operation, badCode, a.body), "wrong HTTP response accepted");
        }
        for (const Operation other : operations) {
            if (other != a.operation) {
                check(!parseAcknowledgment(other, a.code, a.body), "acknowledgement crossed operation boundary");
            }
        }
    }
    check(!parseAcknowledgment(Operation::Stop, 200, R"({"state":"stop_requested"})"), "stop accepted as completed");
    check(!parseAcknowledgment(Operation::ArmCalibration, 200, R"({"state":"armed"})"), "unobserved arm claimed confirmed");
    check(!parseAcknowledgment(Operation::ArmCalibration, 200,
          R"({"state":"bad","state":"calibration_arm_accepted"})"), "duplicate acknowledgement accepted");
    std::puts("PASS: fixed verbs, strict bounded state, duplicate/precision refusal and operation-specific acknowledgement semantics");
    return 0;
}
