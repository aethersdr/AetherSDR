#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// Registry of the Whisper (ggml) model tiers AetherSDR can download for the
// on-device ASR engine (RFC #4333). Weights are NOT shipped; each tier lists
// its pinned size + SHA-256 and an ordered list of download sources (Hugging
// Face primary, GitHub release-asset fallback). The model manager tries the
// sources in order and accepts a download only if its SHA-256 matches, so a
// mirror can never serve different bytes undetected.

namespace AetherSDR {

// The inference-engine family a tier's weights are built for. Whisper (ggml) is
// the only bundled family today; the field lets a tier declare its engine so the
// controller can route it to the right IAsrBackend as more engines are added
// (RFC #4333 follow-up). Adding a family is a drop-in: a new enumerator here, a
// case in CopyAssistController's backend map, and a factory.
enum class AsrModelFamily {
    Whisper,    // whisper.cpp / ggml (.bin/.gguf)
    SherpaOnnx, // sherpa-onnx offline model bundle (a directory of .onnx + tokens)
};

struct AsrModelTier {
    QString id;           // stable key, e.g. "base"
    QString displayName;  // UI label, e.g. "Base — 147 MB"
    QString fileName;     // on-disk + upstream name, e.g. "ggml-base.bin"
    qint64 sizeBytes = 0; // exact expected size (from the upstream LFS pointer)
    QString sha256;       // lowercase hex, pinned
    QStringList sources;  // ordered download URLs (primary first)
    AsrModelFamily family = AsrModelFamily::Whisper; // inference engine the weights target
};

namespace AsrModelCatalog {

// All known tiers, ordered smallest → largest.
const QVector<AsrModelTier>& tiers();

// Tier by id, or nullptr if unknown.
const AsrModelTier* tierById(const QString& id);

// Platform-defaulted starting tier (RFC #4333): "base" on CPU-only builds
// (incl. ARM / Raspberry Pi); a GPU-backed build upgrades this to
// "large-v3-turbo" once a GPU ggml backend is available. Operator-overridable.
QString defaultTierId();

} // namespace AsrModelCatalog
} // namespace AetherSDR
