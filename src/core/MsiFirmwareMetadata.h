#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

namespace AetherSDR {

struct MsiFirmwareImage {
    QString fileName;
    QString memberName;
    QString cabinet;
    qint64 size{};
};

// Resolves a firmware filename through MSI File/Media tables, without running
// the installer. CAB member names are File primary keys, not display filenames.
class MsiFirmwareMetadata {
public:
    struct Streams {
        QByteArray stringPool;
        QByteArray stringData;
        QByteArray columns;
        QByteArray files;
        QByteArray media;
    };

    static constexpr qint64 kMaxMetadataBytes = 16LL * 1024LL * 1024LL;
    static constexpr qint64 kMaxFirmwareBytes = 500LL * 1024LL * 1024LL;

    static std::optional<MsiFirmwareImage> resolve(const Streams& streams,
                                                  const QString& family,
                                                  QString& error);
};

} // namespace AetherSDR
