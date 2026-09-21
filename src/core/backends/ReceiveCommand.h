#pragma once

#include <QMetaType>
#include <QString>

namespace AetherSDR {

// Receive intent, not an observation or a promise of hardware completion.
// Presentation intent cannot be inferred from the size of a frequency jump.
struct SliceTuneRequest {
    enum class PanIntent { PreservePan, AllowRecenter };
    double frequencyHz;
    PanIntent panIntent;
};

struct SliceFilterRequest {
    // Mode normalization repairs an optimistic desktop passband. A radio
    // with its own per-mode filter memory must not receive that repair as an
    // operator edit. Adaptive writes do not advance the operator's epoch.
    enum class Origin { Operator, Adaptive, ModeNormalization };
    int lowHz;
    int highHz;
    Origin origin;
};

struct SliceAgcRequest {
    // Preserve which field the operator changed: Flex has three independent
    // fields, whereas host DSP needs the mode/threshold pair for either edit.
    enum class Field { Mode, Threshold, OffLevel };
    Field field;
    QString mode;
    int threshold;
    int offLevel;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::SliceTuneRequest)
Q_DECLARE_METATYPE(AetherSDR::SliceFilterRequest)
Q_DECLARE_METATYPE(AetherSDR::SliceAgcRequest)
