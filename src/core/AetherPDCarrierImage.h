#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

namespace AetherSDR::aetherpd {
struct CarrierExecutables {
    QByteArray accept;
    QByteArray carrier;
    QByteArray send;
    QByteArray deliver;
};
struct CarrierImage {
    QByteArray archive;
    QString containerName;
    QString imageId;
    QString archiveSha256;
    qsizetype layerBytes{0};
};
// Packages caller-authenticated ARM64 code and a public PDPUB001 receipt.
// Never accepts a private PDXFER01 record, executable names or shell commands.
// Call off the UI thread when packaging large inputs; no radio I/O occurs here.
std::optional<CarrierImage> buildCarrierImage(const CarrierExecutables& executables,
                                             const QByteArray& publicRecord,
                                             const QByteArray& licenses);
} // namespace AetherSDR::aetherpd
