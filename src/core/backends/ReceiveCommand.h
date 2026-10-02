#pragma once

#include <QMetaType>
#include <QString>

namespace AetherSDR {

// Receive intent, not an observation or a promise of hardware completion.
// Presentation intent cannot be inferred from the size of a frequency jump.
struct SliceTuneRequest {
    enum class PanIntent { PreservePan, AllowRecenter };
    double frequencyHz{0.0};
    PanIntent panIntent{PanIntent::PreservePan};
};

struct SliceFilterRequest {
    // Mode normalization repairs an optimistic desktop passband. A radio
    // with its own per-mode filter memory must not receive that repair as an
    // operator edit. Adaptive writes do not advance the operator's epoch.
    enum class Origin { Operator, Adaptive, ModeNormalization };
    int lowHz{0};
    int highHz{0};
    Origin origin{Origin::Operator};
};

struct SliceAgcRequest {
    // Preserve which field the operator changed: Flex has three independent
    // fields, whereas host DSP needs the mode/threshold pair for either edit.
    enum class Field { Mode, Threshold, OffLevel };
    Field field{Field::Mode};
    QString mode;
    int threshold{0};
    int offLevel{0};
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::SliceTuneRequest)
Q_DECLARE_METATYPE(AetherSDR::SliceFilterRequest)
Q_DECLARE_METATYPE(AetherSDR::SliceAgcRequest)
