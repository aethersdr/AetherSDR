#include "asr/AsrModelCatalog.h"

namespace AetherSDR {
namespace AsrModelCatalog {

namespace {

// Upstream Hugging Face repo hosting the ggml Whisper weights (MIT). The
// `resolve/main` path streams the LFS object; `?download=true` requests the
// raw bytes. Qt6's default redirect policy follows the CDN redirect.
QString huggingFaceUrl(const QString& fileName)
{
    return QStringLiteral(
               "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/%1?download=true")
        .arg(fileName);
}

// Fallback mirror: the exact same bytes uploaded to a dedicated AetherSDR
// release tag. Verified against the same pinned SHA-256, so the mirror cannot
// diverge from upstream without the download being rejected.
QString releaseAssetUrl(const QString& fileName)
{
    return QStringLiteral(
               "https://github.com/aethersdr/AetherSDR/releases/download/asr-models-v1/%1")
        .arg(fileName);
}

AsrModelTier makeTier(const QString& id, const QString& displayName,
                      const QString& fileName, qint64 sizeBytes, const QString& sha256,
                      bool mirrored = false)
{
    AsrModelTier tier;
    tier.id = id;
    tier.displayName = displayName;
    tier.fileName = fileName;
    tier.sizeBytes = sizeBytes;
    tier.sha256 = sha256;
    // Hugging Face is the primary source. Only tiers actually mirrored to the
    // `asr-models-v1` GitHub release get a fallback URL — currently just the
    // default `base` (keeping the mirror lightweight); the rest are HF-only.
    tier.sources = {huggingFaceUrl(fileName)};
    if (mirrored) {
        tier.sources.append(releaseAssetUrl(fileName));
    }
    return tier;
}

// True when a GPU ggml backend is compiled/available. Phase-1 builds are
// CPU-only, so this is always false today; the GPU phase wires it to real
// backend detection, which upgrades the default tier to large-v3-turbo.
bool gpuBackendAvailable()
{
    return false;
}

} // namespace

const QVector<AsrModelTier>& tiers()
{
    // Sizes + SHA-256 are the upstream git-LFS pointer values, pinned per RFC.
    static const QVector<AsrModelTier> kTiers = {
        makeTier(QStringLiteral("tiny"), QStringLiteral("Tiny — 74 MB (fastest, roughest)"),
                 QStringLiteral("ggml-tiny.bin"), 77691713,
                 QStringLiteral("be07e048e1e599ad46341c8d2a135645097a538221678b7acdd1b1919c6e1b21")),
        makeTier(QStringLiteral("base"), QStringLiteral("Base — 141 MB (live copy, CPU/Pi default)"),
                 QStringLiteral("ggml-base.bin"), 147951465,
                 QStringLiteral("60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe"),
                 /*mirrored=*/true),
        makeTier(QStringLiteral("small"), QStringLiteral("Small — 465 MB (desktop CPU)"),
                 QStringLiteral("ggml-small.bin"), 487601967,
                 QStringLiteral("1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b")),
        makeTier(QStringLiteral("large-v3-turbo"),
                 QStringLiteral("Large v3 Turbo — 1.6 GB (best, GPU)"),
                 QStringLiteral("ggml-large-v3-turbo.bin"), 1624555275,
                 QStringLiteral("1fc70f774d38eb169993ac391eea357ef47c88757ef72ee5943879b7e8e2bc69")),
    };
    return kTiers;
}

const AsrModelTier* tierById(const QString& id)
{
    for (const AsrModelTier& tier : tiers()) {
        if (tier.id == id) {
            return &tier;
        }
    }
    return nullptr;
}

QString defaultTierId()
{
    if (gpuBackendAvailable()) {
        return QStringLiteral("large-v3-turbo");
    }
    return QStringLiteral("base");
}

} // namespace AsrModelCatalog
} // namespace AetherSDR
