#pragma once

#include <QFlags>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QVariantMap>
#include <optional>

namespace AetherSDR {

struct TxPowerBand {
    double lowHz = 0.0;
    double highHz = 0.0;
    double maxWatts = 0.0;
};

// A backend-declared native band. The canonical name remains the model/UI key;
// the limits let clients present hardware-specific coverage without guessing
// from a family/model string or overloading a transmit-power capability.
struct DeclaredBandRange {
    QString name;
    double lowHz = 0.0;
    double highHz = 0.0;

    bool operator==(const DeclaredBandRange&) const = default;
};

// Frequency observations describe the state owned by the backend, not a
// promise that a queued hardware/DSP write has completed. Zero bounds mean
// this backend has not established a range for the headless control method.
struct SliceFrequencyControl {
    enum class Authority { Unknown, Radio, Engine };
    Authority authority{Authority::Unknown};
    qint64 minimumHz{0};
    qint64 maximumHz{0};
};

// Optional per-feature records (#5262 M2). Absence means no headless verb;
// neither UI ranges nor an inherited no-op establish support. Authority is
// configuration provenance, never hardware acknowledgement/DSP completion.
struct ReceiveModeControl {
    SliceFrequencyControl::Authority authority{SliceFrequencyControl::Authority::Unknown};
    QStringList modes;
};
struct ReceiveFilterMode {
    QString mode;
    int minimumLowHz{0};
    int maximumLowHz{0};
    int minimumHighHz{0};
    int maximumHighHz{0};
    int minimumWidthHz{0};
    int maximumWidthHz{0};
};
struct ReceiveFilterControl {
    SliceFrequencyControl::Authority authority{SliceFrequencyControl::Authority::Unknown};
    QList<ReceiveFilterMode> modes;
};
struct ReceiveAudioControl {
    SliceFrequencyControl::Authority authority{SliceFrequencyControl::Authority::Unknown};
    // Both gain (0..100) and mute must act on the shared slice's RX audio.
};
struct ReceivePanRangeControl {
    SliceFrequencyControl::Authority authority{SliceFrequencyControl::Authority::Unknown};
    qint64 minimumHz{0};
    qint64 maximumHz{0};
    // Declaring center support promises no implicit slice retune. Declaring
    // bandwidth support promises no slice creation/removal or retune.
};

// What the panadapter's SPAN is made of. Absent means NO BACKEND HAS BEEN READ
// on the question — not "no". That distinction is the whole reason this is a
// record and not two bools (#5262 M2): a bool that nobody set reports a
// definite answer indistinguishable from a considered one.
struct PanSpanModel {
    // `sampleRatesHz` is the complete span set: span IS the receiver sample rate.
    // True for a direct-sampling backend that computes the spectrum from raw IQ
    // (no display-side decimation between DDC and FFT); false where span is a
    // display parameter (Flex). When true, a span request must snap to one of
    // `sampleRatesHz` and zoom must stop at the narrowest.
    bool followsSampleRate = false;

    // One span register for the whole radio: every receiver shares one DDC rate, so
    // changing any pan's span changes all of them (HL2: a single two-bit sample-rate
    // field in the HPSDR config command). This is why `receivePanBandwidthControl`
    // can be absent on a radio that does change span: it is not per-panadapter.
    bool radioWide = false;
};

// Engaged when the backend averages its own pan frames per the operator's FFT
// AVG (ANAN: WDSP's display analyzer, AnanPanAnalyzer). The widget then skips
// its client-side EMA (SpectrumWidget::SMOOTH_ALPHA), which would add ~90 ms of
// lag at 25 fps; see SpectrumWidget::setClientFftSmoothingEnabled(). Absent: the
// widget keeps its EMA (Flex, HL2, Icom, Sim).
struct BackendPanAveraging {
    // One FFT AVG slider step as an averaging time (ANAN: deskHPSDR's 10 ms/step,
    // 0 = off). No default: an engaging backend must state its own unit.
    int msPerAverageStep;
};

// Meaning of the panadapter's vertical axis. Absent means no backend has been
// read, and the two fields then fall to opposite legacy answers, so consumers use
// RadioCapabilities::dbmAxisIsCalibrated() / panBinsAbsolute(), never this.
struct PanAmplitudeModel {
    // The axis is absolute dBm at the antenna from a per-unit factory calibration
    // (Flex). False means dBFS under a dBm label: relative levels hold but the zero
    // point is arbitrary, so no value may be compared across stations, published as
    // a spot level or used as an absolute threshold. The Icom scope is uncalibrated
    // (IcomScope.h) and its record is absent. A backend sets this from its own
    // reference where it has one (HL2: Hl2DbReference::isCalibrated()).
    bool calibratedDbm = false;

    // Spectrum bins are absolute levels computed host-side, so a bin does not move
    // when the display reference level moves. A different question from
    // RadioCapabilities::radioOwnsDbmScale (does the radio echo a commanded range).
    // Absolute bins let the noise-floor auto-adjust converge without an echo: its
    // target (baseline + frac * dynamicRange) stays fixed under its own correction,
    // so moveRefLevelToward stops on the 0.45 dB deadband. The gate is
    // noiseFloorAutoAdjustAllowed() (NoiseFloorAutoAdjustGate.h): echo OR absolute
    // bins. Declare true only after reading the backend's bin path.
    bool binsAbsolute = false;
};

// Wideband converter view: raw ADC output before the DDC, spanning the
// converter's first Nyquist zone. A wire-protocol property, not a family; only
// openHPSDR P1 (HL2) implements it here. ANAN (P2) declares nothing because
// P2Protocol.h defines no such endpoint; a Flex has no raw converter stream at
// all. Consumers ask for this record, never for a family name.
struct WidebandConverterView {
    // The converter's own sample rate, in Hz. The view spans DC to half of it.
    double sampleRateHz{0.0};
    // Samples in one delivered record. Contiguous in CONVERTER time, which is
    // the only continuity that matters: a record's samples may be assembled
    // from several datagrams that arrived milliseconds apart.
    int blockSamples{0};
    // Extension verb that delivers ONE record, invoked with a non-zero requestId.
    // The result arrives on extensionResult as {samples: QList<float> in [-1, 1),
    // sampleRateHz, calibrated}, or on extensionError with a reason. On demand only:
    // no subscription until a continuous converter-rate consumer's cost is measured.
    QString frameNamespace;
    QString frameVerb;
};

// A stable, radio-owned receive-filter preset. `id` is the identity used on
// the wire (for example Icom FIL1/FIL2/FIL3); widthHz is mutable content of
// that preset and must never be used as its identity.
struct RxFilterPreset {
    int id = 0;
    QString label;
    int widthHz = 0;

    bool operator==(const RxFilterPreset&) const = default;
};

struct RxFilterControl {
    QList<RxFilterPreset> presets;
    int selectedPresetId = 0;
    int minimumWidthHz = 0;
    int maximumWidthHz = 0;
    int widthStepHz = 0;

    bool operator==(const RxFilterControl&) const = default;
};

// A capability update reaches the two legacy/new presentation setters one at
// a time. Treat the preset metadata as usable only when it describes every
// width in the current presentation list; this keeps a disconnect or mode
// transition from indexing stale FIL metadata against a newly rebuilt list.
[[nodiscard]] inline bool hasCompleteRxFilterPresets(const RxFilterControl& control,
                                                      qsizetype widthCount)
{
    return !control.presets.isEmpty() && control.presets.size() == widthCount;
}

enum class FmTonePresentation {
    Legacy,
    Hidden,
    Ctcss,
};

[[nodiscard]] inline const QStringList& legacyFmToneModes()
{
    static const QStringList modes{
        QStringLiteral("off"),
        QStringLiteral("ctcss_tx"),
    };
    return modes;
}

// The self-declared feature set of a connected radio (aetherd RFC §4.1
// `welcome`, §5.5 Q1): a typed core profile plus a namespaced `extensions` bag.
// Clients render against what the radio reports instead of assuming a Flex.
// Distinct from models/ModelCapabilities, derived from the model name.
//
// ADDING A FIELD: feature-presence fields default to false/0/empty, so omitting
// one declares the feature absent; shape fields of an established control default
// to the legacy shape (e.g. PROC's 0..2). Set every field explicitly in every
// backend and record it in docs/architecture/radio-capabilities-map.md.
struct RadioCapabilities {
    // Identity
    QString family;   // backend id: "flex", "kiwi", … (stable, lowercase)
    QString model;    // radio model string as reported by the hardware

    // Vendor as an operator would say it ("Icom", "FlexRadio"). Display only; empty
    // means not reported. Separate from `family`, which is a wire-protocol id. The
    // status bar shows it above the model unless the model string already names it.
    QString manufacturer;

    // Receive
    // Independent slice creation on an existing pan through the neutral backend
    // hook. RadioModel consults this only without a command plane; Flex and Sim
    // retain their command adapters regardless of this value. Do not use this
    // field alone to gate +RX in the UI. Separate from maxSlices: a paired
    // receiver/pan topology can support several slices but not this operation.
    bool canCreateSlices = false;
    int maxSlices = 1;             // independent demod slices the radio supports
    int maxPanadapters = 1;        // simultaneous panadapters
    QVector<int> sampleRatesHz;    // supported per-receiver sample rates (Hz)

    // What this radio's panadapter span and vertical axis are made of, as two
    // per-feature records (#5262 M2). ABSENT MEANS "no backend has been read",
    // never "no" — and for panAmplitude the two fields fall to opposite legacy
    // answers when it is absent, so read it through the accessors below rather
    // than unwrapping it at the call site.
    std::optional<PanSpanModel> panSpanModel;
    std::optional<PanAmplitudeModel> panAmplitude;
    // See BackendPanAveraging. Absent = the widget averages client-side.
    std::optional<BackendPanAveraging> backendPanAveraging;

    // A backend nobody has read labelled its axis dBm and was consumed as
    // though it meant it. ABSENT KEEPS THAT CLAIM, so this is the legacy shape
    // rather than the conservative one: defaulting to "uncalibrated" would
    // silently restate a claim about backends nobody has read.
    [[nodiscard]] bool dbmAxisIsCalibrated() const
    {
        return !panAmplitude || panAmplitude->calibratedDbm;
    }

    // Absent means UNDECLARED, and an undeclared backend must not be assumed to
    // have absolute bins. The opposite default to the accessor above, and it
    // costs nothing: the auto-floor gate is an OR whose other term,
    // radioOwnsDbmScale, still defaults permissive.
    [[nodiscard]] bool panBinsAbsolute() const
    {
        return panAmplitude && panAmplitude->binsAbsolute;
    }


    // Per-pan band/segment zoom: `display pan set <panId> band_zoom=<0|1>` and
    // `segment_zoom=` (#4057). Absent refuses: every surface that can issue the
    // write asks gui/PanZoomModeGate.h, so a radio with no such verb never shows a
    // zoom control that moves without effect (HERMES.md §17). A record so a radio
    // answering only one of the two keys can gain a field here.
    struct PanZoomModes {
        // The verb that carries both modes, recorded so the declaration names
        // what it grants rather than being a bare presence bit. DIAGNOSTIC, in
        // exactly TwoToneGenerator::selectionCommand's sense below: the gate
        // branches on this record being ENGAGED and never on the string.
        QString setCommand;
    };
    std::optional<PanZoomModes> panZoomModes;


    // Tunable receive range in Hz; both zero = not reported (clients keep their prior
    // assumption). Lets band controls refuse bands the receiver cannot reach.
    double tuningMinHz = 0.0;
    double tuningMaxHz = 0.0;
    SliceFrequencyControl sliceFrequencyControl;
    std::optional<ReceiveModeControl> receiveModeControl;
    std::optional<ReceiveFilterControl> receiveFilterControl;
    std::optional<ReceiveAudioControl> receiveAudioControl;
    std::optional<ReceivePanRangeControl> receivePanCenterControl;
    std::optional<ReceivePanRangeControl> receivePanBandwidthControl;
    // Engaged when the radio can deliver a wideband converter view; see the
    // struct above for why absence is the right default and what it means.
    std::optional<WidebandConverterView> widebandConverterView;

    // Optional per-band native coverage. Empty means "not reported" and keeps
    // canonical band labels. This is distinct from txPowerBands: receive-only
    // radios and bands still need honest presentation even when no PA rating
    // exists.
    QVector<DeclaredBandRange> declaredBandRanges;

    // Manual notch filters (Flex TNF) the radio can hold. 0 = cannot notch, and the
    // UI omits +TNF and the panadapter's notch menu entries.
    int maxNotchFilters = 0;

    // Whether a notch has a depth control (Flex TNF: three depths). A WDSP host notch
    // is a full null with no depth. Meaningless when maxNotchFilters is 0.
    bool notchHasDepth = false;

    // Notch width limits in Hz; 0 = not reported (UI defaults). On host DSP the
    // minimum is real: WDSP widens a narrower request to its filter-length floor
    // instead of refusing it.
    double notchMinWidthHz = 0.0;
    double notchMaxWidthHz = 0.0;

    // Transmit — the load-bearing capability for TX safety (RFC §6). A backend
    // that cannot key sets canTransmit=false; the engine guard then denies any
    // keying intent regardless of client requests.
    bool canTransmit = false;
    double txPowerMaxWatts = 0.0;  // 0 when RX-only

    // Optional per-frequency ceilings for radios whose PA rating changes by
    // band. Empty means txPowerMaxWatts applies everywhere. The ranges are
    // inclusive and expressed in Hz, matching the tuning fields above.
    QVector<TxPowerBand> txPowerBands;

    // Who owns the drive value TransmitModel::rfPower() carries (#5518). Absent: no
    // drive at all (Sim, RTL). Radio: parsed back off the wire (Flex `transmit
    // rfpower=`, Icom CI-V level::kRfPower). Engine: the host owns the register and
    // rfPower() is intent, not applied power (HL2 pins the register at 0 while TX is
    // blocked). Exported on MQTT `aethersdr/radio/state` as `drive_confirmed`, ANDed
    // with TransmitModel::rfPowerIsFromRadio().
    struct TransmitDriveControl {
        SliceFrequencyControl::Authority authority{
            SliceFrequencyControl::Authority::Unknown};
    };
    std::optional<TransmitDriveControl> transmitDriveControl;

    // Whether forward-power telemetry needs client-side attack/decay
    // ballistics. True preserves the established Flex presentation. A backend
    // whose telemetry already carries a stable indicated value can disable the
    // second response layer so consumers reflect each authoritative sample.
    bool forwardPowerRequiresSmoothing = false;

    [[nodiscard]] double txPowerMaxWattsAt(double frequencyHz) const noexcept
    {
        for (const TxPowerBand& band : txPowerBands) {
            if (frequencyHz >= band.lowHz && frequencyHz <= band.highHz) {
                return band.maxWatts;
            }
        }
        return txPowerMaxWatts;
    }

    // Modes the radio demodulates but will not transmit in, in SliceModel's neutral
    // vocabulary (IC-705 WFM). Not canTransmit=false: the radio keys one mode away.
    // Empty = transmits in everything it receives. RadioModel's key-on guards read
    // this and own the refusal, interlock notification and transmit-state rollback
    // (a backend cannot reach TransmitModel).
    QStringList receiveOnlyModes;

    // FM tone presentation is explicit so a vendor-specific model can expose
    // its proven CTCSS/DTCS registers without changing another radio family's
    // controls. fmToneModes is the authoritative per-model mode vocabulary.
    // Hidden is the safe default; established backends opt into Legacy.
    // Existing backends retain their offset controls; model-profile backends
    // explicitly decline this when their protocol has no repeater duplex verb.
    bool hasFmRepeaterOffset = true;
    // Some audio-tone tune implementations cannot key a CW carrier.
    bool hasCwTune = true;

    // The radio generates a genuine two-tone test signal, not a tune carrier (Flex
    // `transmit set tune_mode=two_tone`; other backends key a single carrier).
    // Absent makes `txtest twotone` refuse, so no IMD/ALC report cites a waveform
    // that was never on the air (#5516). An optional cannot distinguish "declared
    // absent" from "never set", so set it explicitly per ADDING A FIELD.
    struct TwoToneGenerator {
        // The command that SELECTS the waveform, recorded because that route —
        // not the act of keying — is what separates a real two-tone from a tune
        // carrier. Diagnostic: nothing branches on the string.
        QString selectionCommand;
    };
    std::optional<TwoToneGenerator> twoToneGenerator;
    FmTonePresentation fmTonePresentation = FmTonePresentation::Hidden;
    QStringList fmToneModes;
    QList<int> fmDtcsCodes;

    // TX audio is modulated on THIS host rather than inside the radio. True for
    // direct-sampling backends (HL2) where the PC runs the modulator and streams
    // baseband to the radio; false for a Flex, which modulates on-radio from its
    // own mic/line jacks. Drives the mic-source list and the PC-audio lock — so
    // it must be a capability, not a family-name special case: an RX-only
    // non-Flex backend must NOT open the mic on connect. (#4449)
    bool hostModulates = false;

    // The radio owns its display dBm scale and echoes back a commanded range (Flex
    // `display pan set min_dbm=…`). False for a fixed-calibration scope (Icom CI-V,
    // ScopeCalibration). The noise-floor auto-adjust waits for that echo before
    // stepping again; with neither an echo nor PanAmplitudeModel::binsAbsolute it
    // ratchets ~24 dB/s off the bottom of the scale. Gate:
    // noiseFloorAutoAdjustAllowed().
    bool radioOwnsDbmScale = true;

    // WHETHER THOSE NUMBERS MEAN ANYTHING is a SEPARATE question from who owns
    // the scale, and it lives in PanAmplitudeModel::calibratedDbm above, read
    // through dbmAxisIsCalibrated(). A radio can own its scale and still label
    // dBFS as dBm; a radio that owns nothing can still be calibrated.

    // Memory channels live in the radio and are re-dumped on connect (Flex). False
    // (default) gives the client-side memory bank; set true only when the radio
    // provably returns the slots.
    bool persistsMemories = false;

    // Whether the active memory store accepts mutations/native recalls, and
    // whether the radio can be read as an explicit import source. Refresh is
    // deliberately independent of persistsMemories: Icom keeps AetherSDR's
    // shared client database as the working store while model-specific codecs
    // ingest snapshots from the radio into it.
    bool canWriteMemories = false;
    bool canApplyMemories = false;
    bool canRefreshMemories = false;
    QStringList memoryGroups;
    QString memoryGroupColumnTitle = QStringLiteral("Group");
    bool memoryRefreshRequiresGroup = false;

    // Operating-state domains this client persists and restores because the radio
    // cannot (RFC #4603 proposal B); persistence authority is per value, not per
    // family. Empty (default) restores nothing, so a radio that keeps its own state
    // (Flex) never has radio-owned values re-asserted (#2465/#4126/#4261). Restore
    // never keys transmit: TxSetpoints covers drive setpoints only.
    enum class ClientSettingsDomain : quint32 {
        Tuning      = 1u << 0,  // RF frequency + demod mode
        Passband    = 1u << 1,  // filter low/high edges
        SpanRate    = 1u << 2,  // span / IQ sample rate
        RfGain      = 1u << 3,  // LNA/preamp gain (per band — see RFC PR 3)
        TxSetpoints = 1u << 4,  // TX drive setpoints (per band); never keying
        Memories    = 1u << 5,  // host-side memory bank documents (#4590 fold-in)
        Agc         = 1u << 6,  // AGC mode + threshold (client-side WDSP AGC)
        Cw          = 1u << 7,  // client-side keyer/sidetone setpoints; never keying
    };
    Q_DECLARE_FLAGS(ClientSettingsDomains, ClientSettingsDomain)
    ClientSettingsDomains clientSettingsDomains;   // default: empty — restore nothing

    // The client owns frequency-error correction because the radio cannot be told
    // its reference error (HL2: the 76.8 MHz NCO scale is a gateware localparam with
    // no HPSDR register). False for a radio with its own calibration command (Flex
    // `radio set cal_freq` / `freq_error_ppb`).
    bool hostFrequencyCalibration = false;

    // The client corrects the DDC0 CIC/decimation droop on panadapter samples
    // (AnanDroopCorrection.h) because the wire protocol cannot. ANAN-G2 only. Gates
    // the Droop Correction settings tab and the `droopcal` bridge verb.
    bool hostDroopCalibration = false;

    // Peripherals / features every family may or may not have
    bool canReboot = false;        // supports a client-triggered radio reboot
    // The radio exposes an authoritative, client-settable dial lock. This is
    // distinct from AetherSDR's local per-slice tuning guard: a radio-side
    // lock may be global and may also follow front-panel changes.
    bool hasRadioDialLock = false;
    bool hasRemoteOnControl = false; // client can configure wake-on-network
    bool canUpgradeFirmware = false; // client can upload radio firmware
    bool hasSmartLink = false;       // client has the SmartLink/WAN service and pin store
    bool hasLicenseInfo = false;     // radio exposes SmartSDR entitlement details
    bool hasClientNetworkConfig = false; // client may write the radio's IP configuration
    bool hasFlexControlIntegration = false; // FlexControl/AetherControl verbs are supported
    bool hasAudioCompression = false; // selectable compressed radio-audio transport
    bool hasSharpFilters = false;    // radio implements the sharp-filter settings page
    // The radio's streaming data plane uses VITA-49. This currently gates the
    // receive-socket buffer and network MTU controls; it describes the transport,
    // not the vendor or only one stream direction.
    bool usesVita49Transport = false;
    // The backend can read the radio's own IP configuration rather than only
    // knowing the address selected by the client.
    bool hasNetworkConfigurationReadback = false;
    bool hasPrivateIpConnectionPolicy = false; // SmartSDR private-IP enforcement setting
    bool hasTuner = false;         // antenna tuner / ATU matching control
    bool hasTunerMemories = false; // radio-side ATU memory recall/database
    bool hasAmplifier = false;     // integrated or controllable PA
    bool hasExtendedDsp = false;   // extended firmware DSP filters (NRS/RNN/NRF)

    // WDSP LMS/FFT filter family: NRL, ANFL, ANFT. Narrower than hasRadioSideDsp and
    // orthogonal to hasExtendedDsp: an Icom has radio-side NR/NB/notch but no LMS
    // filters, so these buttons would reach no register (HERMES §17). Flex only
    // today; named for the concept.
    bool hasLmsNoiseFilters = false;

    // CW audio peaking filter in radio firmware (Flex `slice set <n> apf=` /
    // `apf_level=`). Separate from hasRadioSideDsp because an Icom has radio DSP but
    // no APF register. Gates the P/CW CW-face APF row.
    bool hasAudioPeakingFilter = false;

    // This host runs an impulse noise blanker (WDSP) on the radio's IQ, so NB works
    // without radio-side DSP; OR'd with hasRadioSideDsp at the NB button. Requires
    // an IQ path this host demodulates: a backend receiving finished audio must
    // leave this false.
    bool hasHostNoiseBlanker = false;

    // One operator-placed notch in radio DSP: enable plus position in the passband
    // (IC-705: 16 48 enable, 14 0D position 0000..0255, 16 57 one of three widths).
    // Not a TNF (absolute frequency, several, survive tuning) and not the auto notch
    // (hasRadioSideDsp's ANF).
    bool hasManualNotch = false;

    // The radio's own speech processor (Flex by its command plane; otherwise
    // IRadioBackend::setSpeechProcessor must apply it). Absent: the P/CW face
    // keeps PROC 0..2, and a host-modulating transmitter's ClientComp is the
    // processor instead (#6086). levelMaximum: inclusive top of the level, 2 for
    // NOR/DX/DX+, larger for an evidenced continuous control; minimum is zero.
    struct SpeechProcessorControl {
        int levelMaximum = 2;
        QString label{QStringLiteral("PROC")};
    };
    std::optional<SpeechProcessorControl> speechProcessorControl;

    // The radio can temporarily monitor the transmit frequency while the
    // operator holds a control. This is Icom's XFC (CI-V 1C 02), not a
    // persistent repeater-reverse setting: releasing it returns reception to
    // the normal frequency. The UI therefore renders a momentary button and
    // follows the radio's reported state in both directions.
    bool hasTransmitFrequencyCheck = false;

    // The radio reports the PA supply-voltage rail (the status-bar readout under PA
    // temperature); false removes the readout. Named for the telemetry: an HL2 has a
    // PA but reports no rail. Not hasAmplifier, which nothing reads
    // (radio-capabilities-map.md).
    bool hasSupplyVoltageTelemetry = false;

    // The radio reports PA temperature as live telemetry. False means the
    // Radio Vitals applet omits the temperature gauge and its unit selector
    // instead of presenting an instrument that can never receive a sample.
    // This is independent of supply voltage: a backend may support either,
    // both, or neither telemetry source.
    bool hasPaTemperatureTelemetry = false;

    // The radio reports PA drain current as calibrated live telemetry. The
    // Radio Vitals applet may reuse its PA-instrument row for this only when
    // PA temperature is unavailable; the capability is deliberately separate
    // because some radios define PACURRENT with an unusable/clipped range.
    bool hasPaCurrentTelemetry = false;

    // The radio reports main-fan speed as live telemetry. False means the
    // Radio Vitals applet omits the fan gauge instead of presenting an
    // instrument that can never receive a sample. This is independent of PA
    // temperature and supply voltage: each telemetry source is declared on
    // its own evidence.
    bool hasMainFanTelemetry = false;

    // Selectable hardware mic inputs (Flex MIC/BAL/LINE/ACC). False means the client
    // cannot pick an input and the dropdown collapses to PC: an Icom selects its own
    // (MOD Input > DATA MOD), an HL2 modulates on the host. Offering MIC there would
    // key a transmission with no modulation.
    bool hasSelectableMicInputs = false;

    // Whether the radio implements the downward-expander control surfaced as
    // DEXP in the Phone applet. This is deliberately narrower than
    // hasRadioSideDsp: receive-side DSP does not imply a TX compander command.
    // False hides the complete row rather than leaving an optimistic control
    // with no authoritative command path.
    bool hasDownwardExpander = false;
    // Compression amount in physical dB; preserve the existing Flex face by default.
    float compressionMaximumDb = 25.0f;
    QString alcMeterUnit{QStringLiteral("dBFS")};

    // Independent controls require an implemented command or host DSP path.
    // AGC mode selection alone does not imply a writable threshold/off level.
    bool hasAgcThreshold = false;
    // Modes the implemented selector can honor. A native OFF time-constant
    // editor is a different contract from selecting a fast/medium/slow bank.
    QStringList agcModes{QStringLiteral("off"), QStringLiteral("slow"),
                         QStringLiteral("med"), QStringLiteral("fast")};
    // The radio accepts manual SQL in CW/data modes and owns its persistence.
    // False preserves the existing mode-specific client squelch policy.
    bool hasModeIndependentSquelch = false;
    bool hasAmCarrierLevel = false;

    // The radio's own VOX and SSB transmit monitor (Flex by its command plane;
    // otherwise IRadioBackend::setVox / setTxMonitor must apply them). Absent:
    // their wire text reaches nothing, and the drop notice says so (#6086).
    // hasDelay: the VOX hang time is applied too, not just enable and level.
    struct VoxControl {
        bool hasDelay = false;
    };
    std::optional<VoxControl> voxControl;
    struct TxMonitorControl {};
    std::optional<TxMonitorControl> txMonitorControl;


    // TX audio reaches this backend through IRadioBackend::submitTxAudio rather than
    // a Flex DAX/VITA-49 stream. Separate from hostModulates:
    //   Flex   modulates on the radio, audio via DAX      → false
    //   HL2    modulates on the host,  audio via the seam → true
    //   Icom   modulates on the radio, audio via the seam → true
    bool takesTxAudioOverSeam = false;

    // The backend publishes transmitChanged / keyingStateConfirmed from the radio's
    // own PTT readback; setKeying() is intent only. Consumers that must wait for a
    // real key use RadioModel::radioTransmittingChanged / radioTransmitConfirmed, and
    // RadioModel synthesises no command-edge fallback. False for HL2 (no readback)
    // and Flex (interlock decoded by RadioModel). Icom: CI-V `1C 00`.
    bool hasRadioPttReadback = false;

    // Reachable RX filter widths in Hz; empty = continuous or unknown (the UI keeps
    // its configurable list). Set by radios with a short fixed IF set (IC-705
    // FIL1/FIL2/FIL3).
    QList<int> rxFilterWidthsHz;

    // Stable preset identity and continuous-width limits for radios where a
    // preset selects a mutable hardware slot. Empty preserves the legacy
    // width-only button contract above (Flex/HL2/ANAN/Sim).
    RxFilterControl rxFilterControl;

    // Whether the radio implements the independent TX low/high cutoff controls
    // presented by PhoneApplet. False hides the complete control row rather
    // than offering controls whose writes the backend cannot honour.
    bool hasTxFilterControls = false;

    // Reachable TX passband edges in Hz, ascending; empty = continuous (the Phone
    // applet steps 50 Hz). Non-empty is a hard list: the radio has only these edges
    // (IC-7300MK2: six low, four high), and the two lists are independent.
    QList<int> txFilterLowEdgesHz;
    QList<int> txFilterHighEdgesHz;

    // The RADIO stores named configuration profiles (global / TX / mic) that a
    // client can list, load and save. The seam already carries ProfileDelta and
    // profileChanged in both directions; this is the flag that says whether the
    // radio has any such thing to carry. A backend whose hardware has no
    // on-radio profile store reports false and every profile surface — the PROF
    // applet, the Profile Manager, import/export, the Profiles menu — goes away,
    // rather than offering an empty list the operator cannot populate.
    bool hasProfiles = false;

    // Per-slice RX audio and per-pan IQ as separate streams routed to virtual audio
    // devices for external decoders (Flex DAX). UI visibility only; the crash guard
    // is the separate panStream() null-check in MainWindow::startDax().
    bool hasDaxStreams = false;

    // Audio DSP runs inside the radio via command-plane verbs (Flex: NR/NB/ANF/NRL/
    // ANFL/ANFT, APD, wideband NB, 8-band hardware EQ `eq RXsc`/`eq TXsc`). False for
    // direct-sampling backends. Never gate client-side DSP (AetherDSP NR modules,
    // Aetherial EQ) on this: on a false radio it is the only DSP there is. Broader
    // than hasExtendedDsp (8000-series NRS/RNN/NRF).
    bool hasRadioSideDsp = false;

    // The radio computes the waterfall black level per tile and embeds it in the
    // stream (Flex `display panafall set <id> auto_black=1`). Enables the HW step of
    // the Display panel's Black Level cycle (Off -> SW -> HW); without it the cycle
    // is Off <-> SW. Never gate the client-side SW estimate on this.
    bool hasRadioSideWaterfallAutoBlack = false;

    // The DDC decimation chain attenuates the extreme pan edges in the sampled data
    // itself (ANAN-G2). Drives SpectrumWidget::setPanEdgeTaperEnabled() from
    // MainWindow::onConnectionStateChanged(), a display-only edge crop.
    bool hasDdcPanEdgeRolloff = false;

    // No hasTrackingNotchFilters: TNF surfaces are gated by maxNotchFilters, which
    // host-DSP backends also declare (HL2), not by a Flex command-plane flag.

    // The radio buffers CW text and sends it on its own keyer (Flex `cwx`, progress
    // via `sub cwx all`). Gates the CWX indicator, panel and F1-F12 macros. Every
    // other entry point (FlexControl/Ulanzi, MQTT, TCI, rigctl, SmartCAT KY, the
    // bridge `cwx` verb) asks RadioModel::hasRadioSideCwKeyer(), never this field, so
    // the permissive disconnected rule applies; bridge/rigctl/SmartCAT return an
    // error. False means no text buffer, not no CW.
    bool hasRadioSideCwKeyer = false;

    // Shape of that text keyer. These fields keep shared callers honest when
    // two radios both accept text but expose different surrounding contracts:
    // Flex CWX has a progress counter, stored F-key macros, live typing and
    // per-word speed changes; the verified Icom CI-V command 17 path has none
    // of those and accepts one documented 30-character message at a time.
    // Physical CW controls, independent of whether a text keyer is present.
    // Defaults preserve the continuous controls used by existing backends.
    int cwSpeedMinWpm = 5;
    int cwSpeedMaxWpm = 100;
    int cwPitchMinHz = 100;
    int cwPitchMaxHz = 6000;
    int cwPitchStepHz = 10;

    QString cwTextKeyerName{QStringLiteral("CWX")};
    int cwTextMinWpm = 5;
    int cwTextMaxWpm = 100;
    int cwTextMaxMessageChars = 0;  // 0 = backend has no fixed whole-message limit
    // Empty means the backend accepts its existing command-plane character
    // contract. Non-empty lets protocol adapters reject text synchronously
    // instead of reporting success for a message the radio will alter/refuse.
    QString cwTextAllowedCharacters;
    bool cwTextHasProgress = true;
    bool cwTextHasStoredMacros = true;
    bool cwTextSupportsLive = true;
    bool cwTextSupportsSpeedModifiers = true;

    // The radio records and plays voice-keyer messages (Flex `dvk`). Evaluated before
    // DvkAvailabilityGate's SmartSDR+ entitlement check, whose fail-open rule for an
    // unknown entitlement (#4210) only makes sense on a Flex.
    bool hasVoiceKeyer = false;

    // Full duplex via `radio set full_duplex_enabled=` (Flex). Gates the status-bar
    // FDX indicator.
    bool hasFullDuplex = false;

    // The radio accepts installable waveform/mode plugins (SmartSDR waveforms),
    // so a client can offer to manage them. Also gates the AetherModem D-STAR
    // tab: that page drives the local ThumbDV helper against a SmartSDR D-STAR
    // waveform, which is empty on every family that cannot load waveforms.
    bool hasWaveforms = false;

    // Several GUI clients can hold independent sessions on the radio at once,
    // each with its own slices and audio streams (SmartSDR multiFLEX). A backend
    // that serves exactly one client reports false and the multi-client
    // configuration UI goes away.
    bool hasMultiClientSessions = false;

    // SpotHub spots must stay in the client's SpotModel rather than being
    // published through the radio's command plane. True for a backend whose
    // radio protocol has no compatible spot service and which explicitly
    // chooses the existing passive-local fallback. False preserves the
    // operator's Passive toggle and every existing radio-publication path.
    //
    // This is intentionally a backend policy, not a `family == "icom"` check
    // above the seam. Icom declares its CI-V limitation with it, and HL2 its
    // missing command plane, without changing Flex or Sim spot behavior.
    bool alwaysUseClientSideSpots = false;

    // The radio reports position/time from an on-board GNSS receiver (live GPS
    // readout and location dashboard). Not the operator's configured grid square,
    // which must never be gated on this.
    bool hasGpsLocation = false;

    // Optional detail planes within the location dashboard. Keeping them
    // separate prevents a radio that reports coordinates from being presented
    // as a GPSDO or as a source of satellite-count telemetry.
    bool hasGpsSatelliteTelemetry = false;
    bool hasGpsFrequencyReference = false;

    // The radio owns configurable GPS/NTP clock settings and reports their
    // read-back state. This is an NTP CLIENT capability; hasNtpServer in the
    // legacy Flex model table describes the distinct server role.
    bool hasGpsTimeConfiguration = false;
    // The radio contains GPS/GNSS hardware and therefore has a meaningful GPS
    // setup surface. This is deliberately separate from hasGpsLocation: an
    // IC-705 has an internal GPS receiver (this flag drives its Radio Setup
    // page) and also reports live position/time through 23 00 (hasGpsLocation
    // drives the dashboard). A future model may truthfully declare only the
    // hardware half, so the two claims stay independent.
    bool hasGpsHardware = false;
    bool gpsHardwareRequiresPresence = false; // family declaration is conditional per unit

    // Vendor-specific capabilities, keyed by extension namespace. Clients that
    // don't understand a namespace ignore it; a backend never puts core-profile
    // fields here. Example: {"flex": {"multiFlex": true, "guiClientId": "…"}}.
    QVariantMap extensions;

    // The vendor-extension namespaces this backend implements (for the
    // capability handshake). A client can pre-check before issuing
    // invokeExtension(ns, …).
    QVector<QString> extensionNamespaces;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(RadioCapabilities::ClientSettingsDomains)


// Whether `mode` is receive-only on this radio. An empty mode (no slice) is not;
// case-insensitive because the automation bridge upper-cases its input.
inline bool modeIsReceiveOnly(const RadioCapabilities& caps, const QString& mode)
{
    return !mode.isEmpty() && caps.receiveOnlyModes.contains(mode, Qt::CaseInsensitive);
}
}  // namespace AetherSDR
