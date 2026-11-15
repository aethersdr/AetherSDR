#include "AetherPDCarrierImage.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QtEndian>
#include <zlib.h>

#include <array>
#include <utility>

namespace AetherSDR::aetherpd {
namespace {
constexpr qsizetype kMaxExecutable = 8 * 1024 * 1024;
constexpr qsizetype kMaxExecutables = 16 * 1024 * 1024;
constexpr qsizetype kMaxArchive = 24 * 1024 * 1024;
QString sha(const QByteArray& bytes)
{
    return QStringLiteral("sha256:") + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QByteArray json(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
void octal(QByteArray& header, qsizetype offset, qsizetype width, quint64 value)
{
    header.replace(offset, width, QByteArray::number(value, 8).rightJustified(width - 1, '0') + '\0');
}
void entry(QByteArray& tar, QByteArray name, const QByteArray& bytes, unsigned mode, bool directory = false)
{
    if (directory) { name += '/'; }
    QByteArray header(512, '\0');
    header.replace(0, name.size(), name);
    octal(header, 100, 8, mode);
    octal(header, 108, 8, 0);
    octal(header, 116, 8, 0);
    octal(header, 124, 12, static_cast<quint64>(bytes.size()));
    octal(header, 136, 12, 0);
    header.replace(148, 8, QByteArray(8, ' '));
    header[156] = directory ? '5' : '0';
    header.replace(257, 8, QByteArray("ustar\0" "00", 8));
    unsigned checksum = 0;
    for (const char byte : header) { checksum += static_cast<unsigned char>(byte); }
    header.replace(148, 8, QByteArray::number(checksum, 8).rightJustified(6, '0') + QByteArray("\0 ", 2));
    tar += header;
    tar += bytes;
    tar += QByteArray((512 - bytes.size() % 512) % 512, '\0');
}
void finishTar(QByteArray& tar)
{
    tar += QByteArray(1024, '\0');
    tar += QByteArray((10240 - tar.size() % 10240) % 10240, '\0');
}
std::optional<QByteArray> gzip(const QByteArray& input)
{
    struct Deflater {
        z_stream stream{};
        bool initialized{false};
        ~Deflater() { if (initialized) { deflateEnd(&stream); } }
    } state;
    if (input.size() > kMaxArchive || deflateInit2(&state.stream, 9, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) { return std::nullopt; }
    state.initialized = true;
    gz_header header{};
    header.os = 255;
    if (deflateSetHeader(&state.stream, &header) != Z_OK) { return std::nullopt; }
    const uLong bound = deflateBound(&state.stream, static_cast<uLong>(input.size()));
    if (bound > static_cast<uLong>(kMaxArchive)) { return std::nullopt; }
    QByteArray result(static_cast<qsizetype>(bound), '\0');
    state.stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.constData()));
    state.stream.avail_in = static_cast<uInt>(input.size());
    state.stream.next_out = reinterpret_cast<Bytef*>(result.data());
    state.stream.avail_out = static_cast<uInt>(result.size());
    if (deflate(&state.stream, Z_FINISH) != Z_STREAM_END || state.stream.avail_in != 0) { return std::nullopt; }
    result.resize(static_cast<qsizetype>(state.stream.total_out));
    return result;
}
bool elf(const QByteArray& bytes)
{
    return bytes.size() >= 64 && bytes.size() <= kMaxExecutable
        && bytes.first(7) == QByteArray("\x7f" "ELF\x02\x01\x01", 7)
        && qFromLittleEndian<quint16>(bytes.constData() + 18) == 183;
}
}
std::optional<CarrierImage> buildCarrierImage(const CarrierExecutables& executables,
                                             const QByteArray& publicRecord, const QByteArray& licenses)
{
    if (publicRecord.size() != 120 || !publicRecord.startsWith("PDPUB001")
        || publicRecord.mid(8, 8) != "PDDELIV1"
        || qFromBigEndian<quint32>(publicRecord.constData() + 16) != 64
        || qFromBigEndian<quint32>(publicRecord.constData() + 20) != 2
        || licenses.isEmpty() || licenses.size() > 512 * 1024) { return std::nullopt; }
    for (const qsizetype offset : {qsizetype(24), qsizetype(56), qsizetype(88)}) {
        if (publicRecord.mid(offset, 32) == QByteArray(32, '\0')) { return std::nullopt; }
    }
    const std::array<std::pair<QByteArray, const QByteArray*>, 4> files{{
        {"accept", &executables.accept}, {"carrier", &executables.carrier},
        {"deliver", &executables.deliver}, {"send", &executables.send}}};
    qsizetype total = 0;
    for (const auto& [name, data] : files) {
        if (!elf(*data)) { return std::nullopt; }
        total += data->size();
    }
    if (total > kMaxExecutables) { return std::nullopt; }
    CarrierImage result;
    result.containerName = QStringLiteral("aetherpd-carrier-") + QString::fromLatin1(publicRecord.last(32).toHex().first(32));
    QByteArray layerTar;
    entry(layerTar, "pd", {}, 0700, true);
    for (const auto& [name, data] : files) { entry(layerTar, "pd/" + name, *data, 0500); }
    entry(layerTar, "pd/request", publicRecord, 0444);
    entry(layerTar, "LICENSES", licenses, 0444);
    finishTar(layerTar);
    result.layerBytes = layerTar.size();
    const std::optional<QByteArray> layer = gzip(layerTar);
    if (!layer) { return std::nullopt; }
    const QJsonObject config{
        {"architecture", "arm64"}, {"os", "linux"},
        {"config", QJsonObject{{"Entrypoint", QJsonArray{"/pd/accept"}}, {"User", "0:0"},
            {"Labels", QJsonObject{{"aetherpd.carrier", "v1"}, {"com.flexradio.waveform.name", result.containerName},
                {"com.flexradio.waveform.version", "0.1.0"}}}}},
        {"rootfs", QJsonObject{{"type", "layers"}, {"diff_ids", QJsonArray{sha(layerTar)}}}},
        {"history", QJsonArray{QJsonObject{{"created_by", "AetherSDR AetherPDCarrierImage"}}}}};
    const QByteArray configBytes = json(config);
    const QString manifestType = QStringLiteral("application/vnd.oci.image.manifest.v1+json");
    const QJsonObject manifest{
        {"schemaVersion", 2}, {"mediaType", manifestType},
        {"config", QJsonObject{{"mediaType", "application/vnd.oci.image.config.v1+json"}, {"digest", sha(configBytes)}, {"size", configBytes.size()}}},
        {"layers", QJsonArray{QJsonObject{{"mediaType", "application/vnd.oci.image.layer.v1.tar+gzip"}, {"digest", sha(*layer)}, {"size", layer->size()}}}}};
    const QByteArray manifestBytes = json(manifest);
    const QJsonObject descriptor{
        {"mediaType", manifestType}, {"digest", sha(manifestBytes)}, {"size", manifestBytes.size()},
        {"annotations", QJsonObject{{"org.opencontainers.image.ref.name", result.containerName + QStringLiteral(":0.1.0")}}},
        {"platform", QJsonObject{{"architecture", "arm64"}, {"os", "linux"}}}};
    const QJsonObject index{{"schemaVersion", 2}, {"manifests", QJsonArray{descriptor}}};
    const QMap<QString, QByteArray> blobs{{sha(configBytes), configBytes}, {sha(manifestBytes), manifestBytes}, {sha(*layer), *layer}};
    QByteArray tar;
    entry(tar, "blobs", {}, 0755, true);
    entry(tar, "blobs/sha256", {}, 0755, true);
    for (auto it = blobs.cbegin(); it != blobs.cend(); ++it) { entry(tar, "blobs/sha256/" + it.key().mid(7).toLatin1(), it.value(), 0644); }
    entry(tar, "index.json", json(index), 0644);
    entry(tar, "oci-layout", "{\"imageLayoutVersion\":\"1.0.0\"}", 0644);
    finishTar(tar);
    const std::optional<QByteArray> archive = gzip(tar);
    if (!archive) { return std::nullopt; }
    result.archive = *archive;
    result.imageId = sha(configBytes).mid(7);
    result.archiveSha256 = sha(*archive).mid(7);
    return result;
}
} // namespace AetherSDR::aetherpd
