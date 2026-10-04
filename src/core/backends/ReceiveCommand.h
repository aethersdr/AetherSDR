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
    // A recall replays a stored value. A radio with its own per-mode AGC
    // default must not receive that replay as an operator edit.
    enum class Origin { Operator, Recall };
    Field field{Field::Mode};
    QString mode;
    int threshold{0};
    int offLevel{0};
    Origin origin{Origin::Operator};
};

// Dispatch is not readback. LocalOnly is meaningful for the client-side tune
// lock; it must not be mistaken for a radio-wide dial-lock acknowledgement.
enum class ReceiveDispatch { Dispatched, LocalOnly, Unsupported };

struct SliceDspRequest {
    enum class Feature { Nb, Nr, Anf, Mn, Apf, Nrl, Nrs, Rnn, Nrf, Anfl, Anft };
    enum class Field { Enabled, Level };
    enum class Origin { Operator, ProfileRestore };
    Feature feature{Feature::Nb};
    Field field{Field::Enabled};
    bool enabled{false};
    int level{0};
    Origin origin{Origin::Operator};

    bool valid() const
    {
        return feature >= Feature::Nb && feature <= Feature::Anft
            && (field == Field::Enabled || field == Field::Level)
            && level >= 0 && level <= 100
            && (origin == Origin::Operator
                || (origin == Origin::ProfileRestore && feature == Feature::Nrs
                    && field == Field::Level));
    }
};

struct SliceAudioRequest {
    enum class Field { Gain, Mute, Pan };
    enum class Origin { Operator, ExternalReceiveSuppression };
    Field field{Field::Gain};
    int value{0};
    Origin origin{Origin::Operator};

    bool valid() const
    {
        return field >= Field::Gain && field <= Field::Pan
            && value >= 0 && value <= (field == Field::Mute ? 1 : 100)
            && (origin == Origin::Operator
                || (origin == Origin::ExternalReceiveSuppression && field == Field::Mute));
    }
};

struct SliceSquelchRequest {
    bool enabled{false};
    int level{0};
    bool enabledChanged{false};
    bool levelChanged{false};

    bool valid() const { return level >= 0 && level <= 100; }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::SliceTuneRequest)
Q_DECLARE_METATYPE(AetherSDR::SliceFilterRequest)
Q_DECLARE_METATYPE(AetherSDR::SliceAgcRequest)
Q_DECLARE_METATYPE(AetherSDR::SliceDspRequest)
Q_DECLARE_METATYPE(AetherSDR::SliceAudioRequest)
Q_DECLARE_METATYPE(AetherSDR::SliceSquelchRequest)
