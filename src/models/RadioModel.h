#pragma once

#include "core/CommandParser.h"   // MessageSeverity for radioMessageReceived
#include "core/GuiClientRegistrationState.h"
#include "core/RadioSettingsScope.h"  // RFC #4603 radio-scoped feature documents
#include "core/backends/GpsDelta.h"     // applyGpsChanges payload (aetherd 2.3)
#include "core/backends/MemoryDelta.h"  // applyMemoryChanges payload (aetherd 2.3)
#include "core/backends/ProfileDelta.h" // applyProfileChanges payload (aetherd 2.3)
#include "core/backends/RadioDelta.h"   // applyRadioChanges payload (aetherd 2.3)
#include "core/backends/RadioCapabilities.h" // backendCapabilities() return type
#include "core/backends/IRadioBackend.h"     // backendHealthSnapshot() return type
#include "core/RadioConnection.h"
#include "core/WanConnection.h"
#include "core/PanadapterStream.h"
#include "core/SleepInhibitor.h"
#include "core/DaxTxPolicy.h"
#include "core/LocalMemoryBank.h"   // memory channels for a radio that has none
#include "core/DigitalVoiceWaveformTelemetry.h"
#include <QThread>
#include <optional>
#include "SliceModel.h"
#include "MeterModel.h"
#include "PanadapterModel.h"
#include "ProfileLoadPanWriteQueue.h"
#include "TunerModel.h"
#include "AmpModel.h"
#include "TransmitModel.h"
#include "EqualizerModel.h"
#include "TnfModel.h"
#include "SpotModel.h"
#include "CwxModel.h"
#include "DvkModel.h"
#include "UsbCableModel.h"
#include "DaxIqModel.h"
#include "NavtexModel.h"
#include "FlexWaveformModel.h"
#include "DStarModel.h"
#include "MemoryEntry.h"
#include "ModelCapabilities.h"
#include "RadioStatusOwnership.h"
#include "DisplayInventoryPolicy.h"

#include <QObject>
#include <QString>
#include <QList>
#include <QJsonObject>
#include <QHash>
#include <QMap>
#include <QSet>
#include <functional>
#include <memory>

#include <QTimer>
#include <QElapsedTimer>

namespace AetherSDR {

class IRadioBackend;   // aetherd RFC §5.5 radio-facing seam (owned via unique_ptr below)
class FlexBackend;     // transitional concrete alias for 2.3 status-decode driving

struct LicenseFeatureState {
    bool    seen{false};
    bool    enabled{false};
    QString reason;
};

// RadioModel is the central data model for a connected radio.
// It owns the RadioConnection, processes incoming status messages,
// and exposes the radio's current state to the GUI via Qt properties/signals.
class RadioModel : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString name        READ name        NOTIFY infoChanged)
    Q_PROPERTY(QString model       READ model       NOTIFY infoChanged)
    Q_PROPERTY(QString version     READ version     NOTIFY infoChanged)
    Q_PROPERTY(bool    connected   READ isConnected NOTIFY connectionStateChanged)
    Q_PROPERTY(float   paTemp      READ paTemp      NOTIFY metersChanged)
    Q_PROPERTY(float   txPower     READ txPower     NOTIFY metersChanged)

public:
    explicit RadioModel(QObject* parent = nullptr);
    ~RadioModel() override;

    // Access the underlying connection and panadapter stream
    RadioConnection*  connection()  { return m_connection; }
    PanadapterStream* panStream()   { return m_panStream; }
    // The radio-facing backend seam (aetherd RFC §5.5). Non-owning; used to wire
    // seam-native signals (e.g. a SimBackend's audioFrameReady) that don't flow
    // through PanadapterStream. Null before the first connect.
    IRadioBackend*    backend()     { return m_backend.get(); }

    // DAX channel holds, null-safe.
    //
    // A PanadapterStream is the Flex VITA-49 transport; a backend that carries
    // its own IQ (HL2, KiwiSDR) has none, and panStream() is then null. Every
    // caller of these three used to dereference it bare, so activating RADE or
    // the DAX bridge on such a backend was a segfault rather than a decline --
    // the same crash already fixed once at the startDax() entry, reachable by
    // four more paths behind it.
    //
    // Routing the family through here makes "no stream means no channels to
    // hold" a property of the seam instead of something every call site has to
    // remember, which is the point: the next backend should not be able to
    // reintroduce this by adding a call.
    //
    // Returns false when there is no stream, so callers can report honestly
    // rather than believing they hold a channel they do not.
    bool acquireDaxChannel(int channel, PanadapterStream::DaxConsumer who);
    void releaseDaxChannel(int channel, PanadapterStream::DaxConsumer who);
    void releaseAllDaxChannels(PanadapterStream::DaxConsumer who);

    // Sub-models owned by RadioModel (main thread). (#502)
    MeterModel&       meterModel()       { return m_meterModel; }

    // PROOF OF LIFE, per data class, in milliseconds since the last arrival.
    //
    // -1 means nothing of that class has EVER arrived this session, which is a
    // different answer from "arrived a long time ago" and must not collapse
    // into it. Negative-vs-large is the whole diagnostic.
    //
    // Why this exists: a revoked session keeps every model populated. `get
    // model=pan` answered cheerfully with a centre and a bandwidth while the
    // panadapter rendered a "Connecting to radio…" spinner, and the only thing
    // that caught it was a screenshot. Models hold the LAST value they were
    // given; nothing above them says whether anything is still coming.
    struct DataLiveness {
        qint64 spectrumMs = -1;   // scope sweeps / FFT frames
        qint64 audioMs    = -1;   // demodulated RX audio
        qint64 meterMs    = -1;   // any meter value at all
    };
    [[nodiscard]] DataLiveness dataLiveness() const;
    TunerModel&       tunerModel()       { return m_tunerModel; }
    TransmitModel&    transmitModel()    { return m_transmitModel; }
    EqualizerModel&   equalizerModel()   { return m_equalizerModel; }
    TnfModel&         tnfModel()         { return m_tnfModel; }
    SpotModel&        spotModel()        { return m_spotModel; }
    CwxModel&         cwxModel()         { return m_cwxModel; }
    DvkModel&         dvkModel()         { return m_dvkModel; }
    NavtexModel&      navtexModel()      { return m_navtexModel; }
    UsbCableModel&    usbCableModel()    { return m_usbCableModel; }
    DaxIqModel&       daxIqModel()       { return m_daxIqModel; }
    FlexWaveformModel& flexWaveformModel() { return m_flexWaveformModel; }
    DStarModel&        dstarModel()        { return m_dstarModel; }
    const DigitalVoiceWaveformMetrics& digitalVoiceWaveformMetrics() const;
    DigitalVoiceWaveformHealth digitalVoiceWaveformHealth() const;
    QString digitalVoiceWaveformHealthName() const;
    QString digitalVoiceWaveformHealthDetail() const;
    // Power amplifier (PGXL / any non-TGXL amp the radio proxies). Extracted
    // from RadioModel (#4094); consumers bind it like the other sub-models.
    AmpModel&         amplifier()        { return m_amplifier; }
    const AmpModel&   amplifier() const  { return m_amplifier; }

    // Getters
    QString name()    const { return m_name; }
    QString model()   const { return m_model; }
    QString version() const { return m_version; }
    // Backend-supplied word for what `version` IS ("Gateware" on an HL2),
    // empty when the bare value speaks for itself. Display only — version()
    // stays the unadorned token that rigctl and the bridge serve.
    QString versionLabel() const { return m_versionLabel; }
    bool isConnected() const;
    // True from the moment a connect is requested until it lands, fails, or is
    // abandoned. isConnected() alone cannot express "still working": it is
    // false both before an attempt starts and while one is in flight, which is
    // why the bridge's `connect wait` could not tell a timeout worth retrying
    // from one that never had a chance (#4912). The HL2 makes the gap wide —
    // Hl2Backend::connectRadio() parks the request in m_queuedConnect behind
    // the DSP open and re-drives it from finishDspSetup(), and nothing about
    // that queue reaches this model, so no signal fires for seconds.
    bool isConnectAttemptInFlight() const { return m_connectAttemptActive; }
    bool fullDuplexEnabled() const { return m_fullDuplex; }
    void setFullDuplex(bool on) { m_fullDuplex = on; emit infoChanged(); }
    float paTemp()    const { return m_paTemp; }
    float txPower()   const { return m_txPower; }
    bool  isRadioTransmitting() const { return m_radioTransmitting; }
    // True when the interlock's tx_client_handle is this client (or has
    // never been reported) — false only when another client provably owns
    // the transmitter. See the interlock status parse.
    bool  txOwnedByUs() const { return m_txOwnedByUs; }
    // True while the local operator is keying a phone/data mode (MOX/PTT/VOX/
    // tune), false for TCI-hardware, DAX, and CW transmits. See
    // operatorTransmitChanged().
    bool  isOperatorTransmitting() const { return m_operatorTransmitting; }
    QStringList antennaList() const { return m_antList; }
    QString antennaAlias(const QString& token) const;
    QString antennaDisplayName(const QString& token,
                               bool includeTokenForDisambiguation = false) const;
    QString antennaShortDisplayName(const QString& token, int maxChars = 6) const;
    QMap<QString, QString> antennaAliases() const;
    bool antennaAliasNeedsDisambiguation(const QString& token,
                                         const QStringList& tokens) const;
    void setAntennaAlias(const QString& token, const QString& alias);
    void clearAntennaAlias(const QString& token);
    QStringList knownAntennaTokens() const;
    QString serial()       const;
    QString chassisSerial() const { return m_chassisSerial; }
    // The operator's callsign, from the radio when it has one and from the
    // client-side station setting when it does not.
    //
    // Only a FlexRadio stores a callsign: it arrives in the discovery RadioInfo
    // and in the `info` reply, and `radio callsign <x>` writes it back. Every
    // other family has nowhere to put one, so on an HL2 this returned empty
    // forever — Radio Setup's field accepted an edit, sent Flex text nobody was
    // listening for, and read back blank on reopen. Everything downstream then
    // behaved as if the station had no identity: PSK Reporter had no callsign to
    // query, so the map stayed empty and the status bar said "No callsign —
    // connect to a radio first" against a perfectly connected radio, and the
    // WSPR beacon could not prefill.
    //
    // A callsign is the OPERATOR's, not the radio's — unlike the nickname, which
    // is per-radio and keyed by serial (Hl2Discovery::nicknameSettingsKey). One
    // station-wide key is therefore correct: the same callsign is right on every
    // radio the operator owns.
    //
    // The radio's own value still wins when present, so a Flex behaves exactly
    // as before and a station callsign typed while running headless cannot
    // silently override what the radio reports.
    QString callsign() const;
    // Persist the operator's callsign client-side and publish it. Safe to call
    // on any family; on a Flex the caller is additionally responsible for
    // sending `radio callsign` so the radio's own copy stays in step.
    void setStationCallsign(const QString& callsign);
    QString nickname()     const { return m_nickname; }
    QString region()       const { return m_region; }
    int     rttyMarkDefault() const { return m_rttyMarkDefault; }
    QString radioOptions() const { return m_radioOptions; }
    RadioInfo lastRadioInfo() const { return m_lastInfo; }

    // License info (populated from "sub license all" responses)
    QString licenseRadioId()        const { return m_licenseRadioId; }
    QString licenseExpirationDate() const { return m_licenseExpirationDate; }
    QString licenseMaxVersion()     const { return m_licenseMaxVersion; }
    QString licenseSubscription()   const { return m_licenseSubscription; }
    LicenseFeatureState licenseFeature(const QString& name) const;
    bool licenseFeatureSeen(const QString& name) const;
    bool licenseFeatureEnabled(const QString& name) const;
    QString licenseFeatureReason(const QString& name) const;

    QString ip()          const { return m_ip; }
    QString netmask()     const { return m_netmask; }
    QString gateway()     const { return m_gateway; }
    QString mac()         const { return m_mac; }
    bool    enforcePrivateIp() const { return m_enforcePrivateIp; }

    // GPS data
    QString gpsStatus()    const { return m_gpsStatus; }
    int     gpsTracked()   const { return m_gpsTracked; }
    int     gpsVisible()   const { return m_gpsVisible; }
    QString gpsGrid()      const { return m_gpsGrid; }
    QString gpsAltitude()  const { return m_gpsAltitude; }
    QString gpsLat()       const { return m_gpsLat; }
    QString gpsLon()       const { return m_gpsLon; }
    QString gpsTime()      const { return m_gpsTime; }
    QString gpsSpeed()     const { return m_gpsSpeed; }
    QString gpsTrack()     const { return m_gpsTrack; }
    QString gpsFreqError() const { return m_gpsFreqError; }
    QString gpsNtpServerAddress() const;

    // Max slices reported by radio
    int maxSlices() const {
        // Same authority rule as maxPanadapters(): a backend that reports its
        // own slice capacity knows its radio, and m_maxSlices is either a
        // FlexLib model-table estimate or a Flex `slices=N` status — neither of
        // which a Hermes-Lite 2 ever produces, so it sat at the 2-slice default
        // regardless of how many receivers the board actually has.
        //
        // TciServer's own comment recorded the consequence: "maxSlices() is the
        // model-string-derived Flex estimate (2 by default), not the backend's
        // own maxSlices, so a single-slice HL2 looks like it has room."
        if (!m_flexBackend && m_backend) {
            const int reported = m_backend->capabilities().maxSlices;
            if (reported > 0)
                return reported;
        }
        return m_maxSlices;
    }
    static int maxSlicesForModel(const QString& model);

    // Per-model feature flags from the central ModelCapabilities table.
    // First consumer is the band selector (#695); future model-conditional
    // UI should pull from here rather than adding more model.contains()
    // checks.
    ModelCapabilities capabilities() const {
        return capabilitiesFor(m_model);
    }

    // The live BACKEND's capabilities (RadioCapabilities: canTransmit, canReboot,
    // sample rates, …). Distinct from capabilities() above, which is the
    // FlexLib-derived model table. Use this for anything the backend/seam owns —
    // e.g. TX capability and reboot support, which differ by radio family (#4448).
    // Non-inline: IRadioBackend is only forward-declared here.
    RadioCapabilities backendCapabilities() const;

    // The connected backend's health/status registers, for the Radio Health
    // dialog. Empty when no backend is connected or the family reports none —
    // which the dialog renders as "this radio reports no health registers"
    // rather than as an empty table.
    IRadioBackend::HealthSnapshot backendHealthSnapshot() const;

    // Bands the radio itself declared via the optional discovery/status
    // key "bands=2m,440,23cm" (names validated against BandDefs).  Empty
    // for real Flex radios — the band UI then falls back to the model
    // capability flags.  Lets a gateway presenting non-Flex hardware
    // (e.g. an Icom IC-9700 shown as a FLEX-6700) offer its true band
    // set rather than the impersonated model's.
    QStringList declaredBands() const { return m_declaredBands; }

    // Returns true for BigBend/DragonFire-platform radios (8400, 8600,
    // AU-/ML-/MLS-/CL-/CLS- series, RT-2122) that support the extended
    // firmware DSP filters (NRL, NRS, RNN, NRF).  6000-series radios don't
    // expose these filters and the UI hides them when this returns false. (#2177)
    //
    // Reads the CONNECTED BACKEND's declared RadioCapabilities::hasExtendedDsp,
    // falling back to the FlexLib-sourced ModelCapabilities platform table
    // (Principle I) only when no radio is connected.
    //
    // The backend is the authority here and the table is the guess. FlexBackend
    // already populated caps.hasExtendedDsp — from that same table — but nothing
    // read it: all three GUI call sites came through this method, which went
    // straight to capabilitiesFor(m_model) and bypassed the seam entirely. A
    // non-Flex backend declaring the capability honestly had no way to be heard,
    // and a Flex refining the value from live radio status (as touchpoints
    // convert) would have been ignored.
    //
    // Deliberately NOT ad-hoc substring checks, which the table replaced: the
    // old prefix form silently missed the "S" server variants (MLS-9601 doesn't
    // contain "ML-"; CLS-9301 doesn't contain "CL-") and was case-sensitive.
    bool hasExtendedDspFilters() const;

    // Whether the RADIO runs its own noise reduction / blanking / auto-notch
    // (RadioCapabilities::hasRadioSideDsp) — NR, NB, ANF, NRL, ANFL, ANFT, the
    // APD predistorter and the wideband noise blanker.
    //
    // Permissive when no radio is connected, like every other capability-gated
    // surface: there is nothing to be honest about with nothing attached, and
    // controls that stayed hidden after unplugging would read as a fault. Unlike
    // hasExtendedDspFilters() there is no model-name table to fall back to, so
    // the fallback is simply "assume present".
    //
    // Says nothing about the CLIENT-side modules (NR2/NR4/MNR/BNR/DFNR/RN2),
    // which run on this host and work on any family.
    bool hasRadioSideDsp() const;
    // The two NARROWER claims under it — see the capability struct.
    //
    // hasLmsNoiseFilters() keeps hasRadioSideDsp()'s permissive rule: NRL,
    // ANFL and ANFT existed before the flag did, and hiding them on a
    // Flex the moment it disconnects would be a regression rather than an
    // honesty gain.
    //
    // hasManualNotch() does NOT, and that asymmetry is the point. MN is a
    // new button; a permissive default would show it on every radio in
    // the window before a backend reports, including the Flexes that
    // notch with TNFs instead and will never claim it.
    bool hasLmsNoiseFilters() const;
    bool hasManualNotch() const;
    // Whether THIS HOST blanks impulse noise in the radio's IQ
    // (RadioCapabilities::hasHostNoiseBlanker). Non-permissive on the same
    // reasoning as hasManualNotch(): it can only add the NB button.
    bool hasHostNoiseBlanker() const;
    // The filter widths the radio declares, widest first, or an EMPTY list
    // when it declares none. Empty is the permissive answer here — it means
    // "use the operator's own presets", which is what every radio without a
    // fixed IF ladder wants and what a disconnected app should show.
    QList<int> radioFilterWidthsHz() const;
    // Whether the RADIO computes the waterfall black level per tile
    // (RadioCapabilities::hasRadioSideWaterfallAutoBlack) — the HW position of
    // the Display panel's Black Level button. Same permissive disconnected rule.
    //
    // Says nothing about auto-black itself: the client-side (SW) estimate works
    // on every family and is never gated on this.
    bool hasRadioSideWaterfallAutoBlack() const;
    // Whether the RADIO buffers CW text and sends it on its own keyer
    // (RadioCapabilities::hasRadioSideCwKeyer), and whether it records and
    // plays back voice-keyer messages (RadioCapabilities::hasVoiceKeyer). Same
    // permissive disconnected rule as hasRadioSideDsp().
    //
    // These are accessors rather than inline capability reads because the `cwx`
    // and `dvk` verbs have more entry points than the status-bar buttons: the
    // FlexControl/Ulanzi macro actions, the MQTT CW-transmit topic, the TCI
    // cw_msg / cw_macros commands, rigctl's send_morse / stop_morse, SmartCAT's
    // KY and the automation bridge's `cwx` verb all reach CwxModel without
    // passing through MainWindow's keyer gate. Every one of them asks here, so
    // "the radio has no such verb" is answered in one place instead of once per
    // surface — and the ones that owe a caller a return code answer with an
    // error instead of a success for work that never happened.
    //
    // Says nothing about CW itself: a radio reporting hasRadioSideCwKeyer=false
    // still transmits CW from a key, a paddle or the host keying path; what it
    // lacks is a text buffer.
    bool hasRadioSideCwKeyer() const;
    bool hasVoiceKeyer() const;
    // Whether this radio has DAX audio/IQ channels. Same permissive
    // disconnected rule as hasRadioSideDsp(): with nothing attached there is
    // nothing to be honest about, and blanking the controls on unplug would look
    // like a fault rather than a fact about the radio.
    bool hasDaxStreams() const;

    // True for 2-SCU radios that support diversity RX, from the FlexLib-sourced
    // ModelCapabilities table (Principle I).  Replaces the hand-maintained
    // contains("6500")|... checks, which wrongly enabled diversity on the
    // single-SCU FLEX-6500 and omitted the ML-/MLS-/CL-/CLS- dual-SCU models.
    bool isDiversityAllowed() const {
        return capabilitiesFor(m_model).isDiversityAllowed;
    }

    // Max panadapters supported by this radio model.  Panadapter capacity
    // tracks the radio's SCU/slice capacity (identical across every current
    // model), so this comes from the same FlexLib-sourced ModelCapabilities
    // table (Principle I) rather than an ad-hoc contains() list — the old list
    // omitted the dual-SCU ML-/MLS-/CL-/CLS- models, capping them at 2 pans
    // instead of 4.  Examples: FLEX-6700 -> 8; 6600/6500/8600/AU-520/ML/CL -> 4;
    // 6300/6400/8400/AU-510/RT-2122 -> 2.
    int maxPanadapters() const {
        // A backend that reports its own capability is the AUTHORITY for its
        // radio. capabilitiesFor() is a FlexLib platform table keyed by model
        // string (Principle I) and has nothing true to say about a non-Flex
        // radio — "Hermes-Lite 2" simply falls through it to the 2-pan default,
        // which then refused a third receiver on a board that reported four.
        //
        // This is consulted by more than one caller (the GUI's Add Panadapter,
        // the automation bridge's `pan create`), which is exactly why it is
        // fixed here rather than at either call site.
        if (!m_flexBackend && m_backend) {
            const int reported = m_backend->capabilities().maxPanadapters;
            if (reported > 0)
                return reported;
        }
        return capabilitiesFor(m_model).maxSlices;
    }

    // Panadapter bandwidth limits by radio model (MHz).
    // Values from SmartSDR TCP packet captures (#1385).
    // Dual-SCU: 6700, 8600. Single-SCU: 6300, 6400, 6500, 6600, 8400.
    double maxPanBandwidthMhz() const {
        if (m_model.contains("6700") || m_model.contains("8600"))
            return 14.745601;
        if (m_model.contains("6500"))
            return 14.745601;
        // 6300, 6400, 6600, 8400, Aurora: single SCU
        return 5.4;
    }
    // The span limits to clamp a zoom against for ONE pan.
    //
    // Prefers what the backend reported for that pan over the model-string table
    // below. The table is a FlexLib platform lookup, so it is right for a Flex
    // radio and a guess for anything else: it falls through to 5.4 MHz for any
    // model string it doesn't recognise, which let an HL2 delivering 384 kHz be
    // zoomed fourteen times past its own data. A backend that knows its real
    // rates says so, and then this returns the truth instead of the guess.
    //
    // Every zoom path — wheel, drag, keyboard, MIDI — must clamp through here, or
    // the one that doesn't becomes the one that reopens the black bars.
    double panMinBandwidthMhz(const QString& panId) const {
        const PanadapterModel* pan = panadapter(panId);
        if (pan && pan->bandwidthLimitsKnown())
            return pan->minBandwidthMhz();
        return minPanBandwidthMhz();
    }
    double panMaxBandwidthMhz(const QString& panId) const {
        const PanadapterModel* pan = panadapter(panId);
        if (pan && pan->bandwidthLimitsKnown())
            return pan->maxBandwidthMhz();
        return maxPanBandwidthMhz();
    }

    double minPanBandwidthMhz() const {
        if (m_model.contains("6700") || m_model.contains("8600"))
            return 0.001230;
        if (m_model.contains("6500"))
            return 0.004920;
        // 6300, 6400, 6600, 8400, Aurora
        return 0.004920;
    }

    // Oscillator / RX settings
    QString oscState()     const { return m_oscState; }
    QString oscSetting()   const { return m_oscSetting; }
    bool    oscLocked()    const { return m_oscLocked; }
    bool    extPresent()   const { return m_extPresent; }
    bool    gpsdoPresent() const { return m_gpsdoPresent; }

    // True when this unit has a GPS position source: FLEX-8000 class (8400,
    // 8600) and Aurora by model, a live oscillator presence flag, or a `gps`
    // status object reporting data. The live signals cover optional GPSDOs on
    // 6000-series radios without turning the family-level capability into a
    // per-unit presence claim.
    bool hasGpsHardware() const {
        return m_model.contains("8400") || m_model.contains("8600")
               || m_model.startsWith("AU-")
               || m_gpsdoPresent
               || (!m_gpsStatus.isEmpty()
                   && m_gpsStatus != QLatin1String("Not Present"));
    }
    bool    tcxoPresent()  const { return m_tcxoPresent; }
    bool    binauralRx()   const { return m_binauralRx; }
    bool    cwxActive()    const { return m_cwxActive; }
    void    setBinauralRx(bool on);  // optimistic update + radio command
    bool    muteLocalWhenRemote() const { return m_muteLocalWhenRemote; }
    bool    autoSave() const { return m_autoSave; }
    int     freqErrorPpb() const { return m_freqErrorPpb; }
    double  calFreqMhz() const { return m_calFreqMhz; }

    // Audio output
    int     lineoutGain()    const { return m_lineoutGain; }
    bool    lineoutMute()    const { return m_lineoutMute; }
    int     headphoneGain()  const { return m_headphoneGain; }
    bool    headphoneMute()  const { return m_headphoneMute; }
    bool    frontSpeakerMute() const { return m_frontSpeakerMute; }
    void setLineoutGain(int v);
    void setLineoutMute(bool m);
    void setHeadphoneGain(int v);
    void setHeadphoneMute(bool m);
    void setFrontSpeakerMute(bool m);
    QHostAddress radioAddress() const { return m_lastInfo.address; }

    int     filterSharpnessVoice()     const { return m_filterVoice; }
    bool    filterSharpnessVoiceAuto() const { return m_filterVoiceAuto; }
    int     filterSharpnessCw()        const { return m_filterCw; }
    bool    filterSharpnessCwAuto()    const { return m_filterCwAuto; }
    int     filterSharpnessDigital()   const { return m_filterDigital; }
    bool    filterSharpnessDigitalAuto() const { return m_filterDigitalAuto; }

    // Global profiles
    QStringList globalProfiles() const { return m_globalProfiles; }
    QString activeGlobalProfile() const { return m_activeGlobalProfile; }
    void loadGlobalProfile(const QString& name);
    void buildBackend();   // RFC #4288 Route A: create + wire m_backend (Sim/Flex)
    void resetPanState();

    // True when a text command issued through sendCmd() can actually reach the
    // radio. False for a backend that owns no RadioConnection (HL2 and any
    // other family that takes typed intents through the IRadioBackend seam) —
    // there, Flex wire text has nowhere to go and is dropped at the sink.
    bool hasCommandPlane() const { return m_wanConn != nullptr || m_connection != nullptr; }

    // ── Local memory bank (radios with no memory slots of their own) ─────────
    //
    // Answer a `memory …` command out of the local bank. Returns the sequence
    // number sendCmd() would have returned (non-zero — sendCommand() reads that
    // as "dispatched"), or nullopt when the command is not one the bank owns
    // and must take its normal path.
    // (spelled out rather than the ResponseCallback alias — that is declared
    // further down this class.)
    std::optional<quint32> tryLocalMemoryCommand(
        const QString& command, const RadioConnection::ResponseCallback& cb);
    // Settle which store owns the memory cache for the session being started:
    // the local bank, or the radio's own slots.
    void syncMemoryStoreForSession();
    // Push the loaded bank into m_memories, emitting per-slot memoryChanged so
    // the browse panel and the panadapter memory-spot feed populate exactly as
    // they do from a Flex's memory-status dump.
    void publishLocalMemories();
    // Apply a stored channel to the active slice. This is what `memory apply`
    // does on a Flex; with no radio to do it, the model drives SliceModel's
    // operator-issue setters so the change routes through the backend seam.
    void recallLocalMemory(int index);
    void createAudioStream();
    bool ensureDaxTxStream(DaxTxRequestReason reason);
    bool prepareWsprTransmit();
    void releaseWsprTransmit();
    void restoreWsprTransmitDax();
    // "The WSPR beacon's transmit-audio route is ready." On a Flex that is
    // literally a `dax_tx` stream; on a host-modulating backend (HL2) there is
    // no stream to own — the modulator is ours and the pump feeds it through
    // txFinalMonitorPcmReady → submitTxAudio, which needs nothing created.
    //
    // The dialog polls this every 50 ms while transmitting and aborts the frame
    // if it goes false, so the host-modulated flag must stay latched from
    // prepareWsprTransmit() to releaseWsprTransmit() rather than being derived
    // from anything that can flicker.
    bool hasWsprTxStream() const
    {
        // The host-modulated claim is re-checked against the CURRENTLY connected
        // radio, not just the latch. A latch alone was an unintended-transmission
        // bug: connectToRadio() on a family switch runs
        // dropAllSessionModelsForFamilySwitch() -> teardownBackend() ->
        // setupBackend() and touches none of the WSPR state, so an armed HL2
        // beacon carried "route ready" onto a Flex and would have keyed it for a
        // full 111.6 s frame with no dax_tx stream behind it — transmitting
        // nothing, on a radio the operator never armed. (PR #4537 review.)
        //
        // teardownBackend() now clears the latch as well; this is the backstop
        // that makes a missed clear harmless rather than dangerous, which is the
        // right split for anything guarding a transmitter.
        if (m_wsprTxHostModulated && backendCapabilities().hostModulates)
            return true;
        return m_daxTxStreamId != 0 && m_daxTxActive;
    }
    QJsonObject troubleshootingSnapshot() const;

    // Memory channel cache
    const QMap<int, MemoryEntry>& memories() const { return m_memories; }
    void handleMemoryStatus(int index, const QMap<QString, QString>& kvs);

    // True when memory channels live in a file on THIS host rather than in the
    // radio — the HL2/Kiwi/demo case, and the disconnected case. Driven by
    // RadioCapabilities::persistsMemories, so a new backend gets the local bank
    // by default rather than writing channels into a radio that drops them.
    bool usesLocalMemoryBank() const;
    // The bank itself, for the automation bridge and tests. Empty and unread
    // until the first local memory command or connect.
    LocalMemoryBank& localMemoryBank() { return m_localMemories; }
    bool    lowLatencyDigital()        const { return m_lowLatencyDigital; }
    bool    hasStaticIp()     const { return m_hasStaticIp; }
    QString staticIp()        const { return m_staticIp; }
    QString staticNetmask()   const { return m_staticNetmask; }
    QString staticGateway()   const { return m_staticGateway; }
    bool    remoteOnEnabled() const { return m_remoteOnEnabled; }
    bool    multiFlexEnabled() const { return m_multiFlexEnabled; }
    const QMap<quint32, QString>& clientStations() const { return m_clientStations; }
    quint32 txClientHandle() const { return m_txClientHandle; }
    quint32 ourClientHandle() const;
    bool sliceMayBelongToUs(int sliceId) const;

    struct ClientInfo {
        QString clientId;
        QString station;
        QString program;
        QString source;
        bool localPtt{false};
        QString txAntenna;
        double txFreqMhz{0};
    };
    const QMap<quint32, ClientInfo>& clientInfoMap() const { return m_clientInfoMap; }
    // #3977: per-source-handle tally of dBm-range writes made by OTHER
    // clients against pans we own — the #3951 zombie signature. Exposed via
    // the bridge `get clients` verb; feeds the evidence-based eviction.
    struct ForeignPanWrite {
        int count{0};
        QString panId;   // last pan written
        qint64 lastMs{0};
    };
    const QMap<quint32, ForeignPanWrite>& foreignPanWrites() const { return m_foreignPanWrites; }
    // Confirmed evictions only — a handle enters this set when the radio
    // acknowledges the `client disconnect`, never before (#3977).
    const QSet<quint32>& evictedPredecessorHandles() const { return m_evictedPredecessorHandles; }
    // #3977: force-disconnecting another client is opt-in
    // (AppSettings["StaleSessionDefense"].EvictionEnabled, default false);
    // detection and forensics always run.
    bool staleSessionEvictionEnabled() const;
    QString ourStationName() const;
    void    setKnownGuiClients(const QStringList& handles,
                               const QStringList& programs,
                               const QStringList& stations,
                               const QStringList& ips = {},
                               const QStringList& hosts = {});
    void    mergeKnownGuiClients(const QStringList& handles,
                                 const QStringList& programs,
                                 const QStringList& stations,
                                 const QStringList& ips = {},
                                 const QStringList& hosts = {});
    void    setRemoteOnEnabled(bool on);
    void    setMultiFlexEnabled(bool on);
    // Panadapter access (delegates to active pan)
    double panCenterMhz() const;
    double panBandwidthMhz() const;
    QString panId() const { return m_activePanId; }
    void setActivePanId(const QString& id) { m_activePanId = id; }
    PanadapterModel* activePanadapter() const;
    PanadapterModel* panadapter(const QString& panId) const;
    // Addressed pan, else active — the pan-addressing policy for the aetherd RFC
    // 2.3 backend-signal handlers, single-sourced (#4065 review).
    PanadapterModel* resolvePan(const QString& panId) const;
    QList<PanadapterModel*> panadapters() const { return m_panadapters.values(); }

    // Radio-authoritative display inventory vs what we own (#3856 Layer B).
    // Built from the accumulated "display pan"/"display waterfall" status maps;
    // surfaces leaked waterfalls (parent pan gone), foreign and orphan objects.
    DisplayInventory::Report displayInventoryReport() const;

    // Force the radio to re-dump every currently-allocated display object by
    // re-subscribing to the pan/display domain. The status replies refresh the
    // Layer-B inventory maps (m_radioDisplayPans/Waterfalls) to the radio's
    // authoritative present-tense set — re-adding a resource-level lingering
    // waterfall that stopped emitting UDP and whose client view we already tore
    // down. Async: callers re-poll displayInventoryReport() after the re-dump
    // settles (#3856). Returns true if the re-subscribe was sent.
    bool resyncDisplayInventory();

    QList<SliceModel*> slices() const { return m_slices; }
    SliceModel* slice(int id) const;
    // The slice that transmits (IsTransmitSlice), or nullptr if none. Mirrors
    // FlexLib's TX-slice scan (CWX::getTXFrequency, CWX.cs:186) — keyer/CWX
    // targeting follows this slice, not the selected RX slice.
    SliceModel* txSlice() const;
    QMap<int, QString> rawSliceModeLists() const { return m_rawSliceModeLists; }
    int rawModeOccurrenceCount(const QString& mode) const;
    int activeTxSliceNum() const;
    void setPanTransmitInhibited(const QString& panId,
                                 bool inhibited,
                                 const QString& reason = {});
    bool panTransmitInhibited(const QString& panId) const;
    QString panTransmitInhibitReason(const QString& panId) const;

    // Multi-Flex slot occupancy.  These let UI distinguish three slot
    // states for any global slice index: ours (we have a SliceModel for
    // it), foreign (another client owns it — we have no SliceModel but
    // know the slot is taken), and empty (neither).  Updated by
    // handleSliceStatus() as slice ownership comes in.
    bool isSlotOurs(int sliceId) const;
    bool isSlotForeign(int sliceId) const;
    // Station name of the client occupying a foreign slot, or empty string
    // if not foreign / not known yet.
    QString foreignSliceOwnerStation(int sliceId) const;
    bool automationApplySliceFixture(int sliceId,
                                     const QString& radioLetter,
                                     QString* error = nullptr);
    bool automationRemoveSliceFixture(int sliceId,
                                      QString* error = nullptr);
    bool automationApplyGpsFixture(const GpsDelta& delta,
                                   const QString& referenceState,
                                   const QString& referenceSetting,
                                   bool referenceLocked,
                                   const QString& ntpServerAddress,
                                   QString* error = nullptr);

    // High-level actions
    void connectToRadio(const RadioInfo& info);
    void connectViaWan(WanConnection* wan, const QString& publicIp, quint16 udpPort);
    void setPendingClientDisconnects(const QList<quint32>& handles);
    bool disconnectClient(quint32 handle);
    // Called by MainWindow in response to multiFlexConflictDetected().
    // Disconnects handle and resumes the connection sequence.
    void resolveMultiFlexConflict(quint32 handle);
    // Called by MainWindow when the user cancels the conflict dialog.
    void cancelMultiFlexConflict();
    void disconnectFromRadio();
    void forceDisconnect();  // Close TCP but allow auto-reconnect
    // Send `radio reboot` (FlexLib Radio.cs:2575), surface a notification to
    // the operator, then trigger forceDisconnect so the standard reconnect
    // timer brings the link back when the radio finishes booting.
    void rebootRadio();
    bool isWan() const { return m_wanConn != nullptr; }

    // Phase 2 of GHSA-wfx7-w6p8-4jr2 (#2951): forward the user's
    // cert-mismatch decision down to the active WAN connection. No-op
    // if not connected via WAN or no decision is pending.
    void acceptPresentedWanCert();
    void rejectPresentedWanCert();
    void setTransmit(bool tx, TransmitModel::PttSource source = TransmitModel::PttSource::Mox);
    void setDigitalVoiceTxSlice(int sliceId);
    QString audioCompressionParam() const;        // "none" or "opus" based on settings
    void sendCwKey(bool down, const QString& debugSource = {},
                   quint64 debugTraceId = 0, quint64 debugSourceMs = 0); // straight key via netcw stream
    void sendCwPaddle(bool dit, bool dah, const QString& debugSource = {},
                      quint64 debugTraceId = 0, quint64 debugSourceMs = 0); // iambic paddle via netcw stream
    // Lower-level pieces used by the local iambic keyer: PTT and key
    // edges are managed separately so PTT stays asserted across the whole
    // squeeze while key transitions on each element boundary.
    void sendCwPtt(bool on, const QString& debugSource = {},
                   quint64 debugTraceId = 0, quint64 debugSourceMs = 0);
    void sendCwKeyEdge(bool down, const QString& debugSource = {},
                       quint64 debugTraceId = 0, quint64 debugSourceMs = 0);
    void cwAutoTune(int sliceId, bool intermittent); // int=1 start loop, int=0 stop
    void cwAutoTuneOnce(int sliceId);                // one-shot (no int= param)
    void addSlice();           // Create a new slice on the active panadapter
    void addSliceOnPan(const QString& panId); // Create a new slice on a specific pan
    void addSliceOnPan(const QString& panId, double freqMhz); // Create slice on specific pan/frequency
    void createPanadapter();   // Create a new independent panadapter
    void removePanadapter(const QString& panId);
    void setPanBandwidth(double bandwidthMhz);
    void setPanCenter(double centerMhz);
    void setPanDbmRange(float minDbm, float maxDbm);

    // #4142 — the ONLY supported way for a USER-INTENT path to write a pan's
    // center/bandwidth/band to the radio.
    //
    // `display pan set <id> center=…` (and bandwidth=/band=) is classified as
    // a profile-owned radio state write, so sendCmd() DROPS it while the
    // profile-load hold is armed: it returns before a sequence number is
    // allocated and the command never reaches the wire. A user action that
    // lands in that window (typed frequency, zoom, drag, band change, ATU
    // sweep, automation) was silently lost, and the client kept its optimistic
    // state — leaving the pan permanently claiming state the radio never took.
    //
    // requestPan*() defers instead of dropping: while the hold is armed it
    // coalesces the request per pan (field-wise, last write wins per field)
    // and returns false WITHOUT advancing local model state, so the client
    // never claims state the radio does not have. The replay is scheduled by
    // the act of deferring and re-checks the hold before sending.
    //
    // Deduping is centralized here against EFFECTIVE state — the pending value
    // if one is queued, else the model (which keeps tracking radio status
    // during the hold). A request equal to the model that supersedes a
    // different pending value is a user CORRECTION: the pending entry is
    // cancelled instead of replayed.
    //
    // Routing discipline is the user-intent boundary: model-echo/reconcile
    // writers (active-slice reasserts, dBm auto-floor, fps/average reconciles)
    // must keep their explicit guards and the sendCmd() backstop — #3563
    // suppresses them during a profile load BY DESIGN. Do not route those.
    //
    // Pass bandwidthMhz > 0 to set center and bandwidth coherently in one
    // command (zoom paths must never split the pair); pass <= 0 to leave the
    // radio's bandwidth untouched.
    //
    // Returns true if the radio's state matches the request (dispatched, or a
    // corrective cancel — the radio is already there); false if the request is
    // deferred or could not be dispatched. Callers that also advance view
    // state optimistically must gate that on the return value, or they will
    // re-create the black-waterfall divergence.
    //
    // THE INTENT IS THE CALLER'S TO STATE, not something to infer here. On a
    // backend whose scope window is slaved to the VFO (every networked Icom)
    // Drag means RETUNE, so "which caller is this" decides whether the radio
    // moves. Inferring it from whether a bandwidth came along classifies every
    // centre-only writer — pan-follow, reveal, band change, the WFM recentre —
    // as a drag, and reveal in particular asks for a DELIBERATELY OFFSET centre
    // (settle distance from the edge), which would tune the radio most of a
    // half-span off the signal the operator just clicked.
    //
    // Range is the default because it is the one that cannot move a radio: a
    // slaved-scope backend refuses it and re-asserts its own geometry. Only the
    // two genuine "the operator moved the window" sites pass Drag.
    bool requestPanCenter(const QString& panId,
                          double centerMhz,
                          double bandwidthMhz = -1.0,
                          IRadioBackend::PanCenterIntent intent =
                              IRadioBackend::PanCenterIntent::Range);
    bool requestPanBandwidth(const QString& panId, double bandwidthMhz);
    // The operator's Display→FFT FPS / Display→Waterfall Rate intent.
    //
    // On a Flex these are radio settings and this sends the wire text, exactly
    // as the call sites used to inline. On a backend that streams raw spectra
    // there is no radio to ask — the engine shapes the stream itself — so the
    // values are applied to the pan model, which is what the shaper reads.
    // Without this the sliders moved nothing on an HL2: the wire text was
    // addressed to a command interpreter that does not exist on that radio.
    //
    // Returns true when the intent was applied or dispatched.
    // `wfRate` is the 1..100 waterfall RATE control value, low slow / high
    // fast — NOT the milliseconds its Flex wire name (`line_duration`) claims.
    // See core/WaterfallRate.h. (#4606)
    bool requestPanDisplayRates(const QString& panId, int fps, int wfRate);
    bool requestPanBand(const QString& panId, const QString& bandKey);

    // Retune a slice on behalf of the CAT servers (rigctld / SmartCAT) so a band
    // change made over CAT (WSJT-X/FLDigi "change band") is recentered instead of
    // leaving the panadapter behind. Applies the recenter policy: in-span keeps
    // autopan=0 (no yank — external Doppler software like SatPC32 steps every few
    // seconds); an out-of-span or cross-band target uses tuneAndRecenter. Both go
    // through SliceModel, which updates the model and emits frequencyChanged —
    // required to drive the client-side follow (Center Lock / Pan-Follows-VFO)
    // that lets the radio actually move a centered slice. We issue no pan command
    // directly; the client lock logic does, exactly as the GUI path does. Call on
    // the GUI thread (owns SliceModel).
    // Returns false (and issues no tune) when the target is rejected — a null
    // slice, an implausible frequency (see isPlausibleCatTuneMhz), or a locked
    // slice, which SliceModel refuses — so the CAT protocol layer can report the
    // failure instead of acknowledging a tune that never happened. A retune to
    // the frequency the slice already holds is a no-op, not a rejection, and
    // still returns true — unless the slice is locked, since the lock is tested
    // ahead of the no-op case and a locked slice always reports failure.
    bool tuneSliceForCat(SliceModel* slice, double mhz);

    // Is this a physically plausible CAT-tune target (MHz)? The single policy for
    // the boundary check tuneSliceForCat applies. rigctld pre-validates with it
    // too: its handlers answer the client synchronously and then marshal the tune
    // through a queued call, so tuneSliceForCat's bool is unobservable to them —
    // without this they would answer success and silently drop an out-of-range
    // tune. Rejects non-positive, non-finite, and absurdly-high targets (an
    // FA-5000;, a DN whose multi-step product underflows past 0, a parse that
    // yielded NaN, a fat-fingered absurd set); the radio still enforces its real
    // band limits — this only stops the physically impossible from being
    // broadcast via frequencyChanged before the radio rejects it.
    // NOT a bound on step arithmetic in the UP direction: the ceiling has to sit
    // above the top amateur allocation, and a multi-step UP cannot reach it
    // (2^31 steps at the usual 100 Hz is ~215 GHz), so a runaway UP is caught by
    // the radio refusing the tune, not here.
    static bool isPlausibleCatTuneMhz(double mhz);

    // Effective pan geometry: the deferred pending value if one is queued,
    // else the live model value, else NaN when the pan is unknown. Caller-side
    // no-op guards must compare against THIS, not the raw model — during the
    // hold the model deliberately lags the user's deferred request.
    double effectivePanCenterMhz(const QString& panId) const;
    double effectivePanBandwidthMhz(const QString& panId) const;

    // Replays deferred pan writes. Hard invariant: this early-returns while
    // the hold is armed, so no deferred write can reach the wire inside the
    // hold window. Scheduling is owned by the defer path (hold-relative,
    // self-re-arming) — never by the profile-load ACK, which may never arrive.
    void flushPendingProfileLoadPanWrites();
    void setPanWnb(bool on);
    void setPanWnbLevel(int level);
    void setPanRfGain(int gain);
    // Same, addressed at a specific pan rather than the active one. The overlay
    // menu belongs to one panadapter and must drive that one, not whichever
    // happens to be active when the slider moves.
    void setPanRfGainFor(const QString& panId, int gain);
    // Discrete receive front-end stages — `step` indexes the label list the
    // backend published for that pan (PanadapterModel::preampLabels /
    // attenuatorLabels). Seam-only; see the .cpp for why there is no Flex
    // wire-text fallback.
    void setPanPreampFor(const QString& panId, int step);
    void setPanAttenuatorFor(const QString& panId, int step);

    // Display controls — FFT (display pan set)
    void setPanAverage(int frames);
    void setPanFps(int fps);
    void setPanWeightedAverage(bool on);

    // Display controls — Waterfall (display panafall set)
    void setWaterfallColorGain(int gain);
    void setWaterfallBlackLevel(int level);
    void setWaterfallAutoBlack(bool on);
    // Auto-black source: false = client-side estimate (radio auto_black off),
    // true = radio's per-tile level (radio auto_black on). The radio only
    // receives auto_black=1 when auto-black is on AND radio-side is selected.
    void setWaterfallAutoBlackSource(bool radioSide);
    void setWaterfallLineDuration(int ms);

    // Display controls — Noise floor
    void setPanNoiseFloorPosition(int pos);
    void setPanNoiseFloorEnable(bool on);

signals:
    void infoChanged();
    void licenseFeaturesChanged();
    void connectionStateChanged(bool connected);
    // The connected backend's self-declared RadioCapabilities changed, or a
    // connect/disconnect changed which backend is answering. Relays
    // IRadioBackend::capabilitiesChanged and also fires on every
    // connectionStateChanged edge, so a consumer that wants "the capability
    // picture is now different, re-read it" needs exactly this one connection.
    //
    // `connected` is passed rather than left for the slot to query, because
    // every capability-driven surface has to distinguish "the radio says it
    // lacks this" from "there is no radio" — the latter restores the permissive
    // value (see MainWindow::applyCapabilitiesToUi).
    void capabilitiesChanged(bool connected, const RadioCapabilities& caps);
    // Emitted whenever the backend instance is (re)built — including the
    // connect-time swap between FlexBackend and SimBackend (RFC #4288). The old
    // m_backend is already destroyed and m_backend now points at the new one.
    // Emitted whenever the local CW key transitions on/off — funnel for
    // serial CTS/DSR, MIDI Gate, TCI key, CWX, and HID encoder sources.
    // Wired to AudioEngine's CwSidetoneGenerator for low-latency local
    // sidetone independent of the radio's own DAX-fed sidetone.
    void cwKeyDownChanged(bool down);
    void sliceAdded(SliceModel* slice);
    void sliceRemoved(int sliceId);
    void rawSliceModeListsChanged();
    void metersChanged();
    void connectionError(const QString& msg);
    // Radio CONFIGURATION advice that does not end the session. See
    // IRadioBackend::configurationWarning for why this is a separate channel.
    void configurationWarning(const QString& msg);
    // Phase 2 of GHSA-wfx7-w6p8-4jr2 (#2951): forwarded from
    // WanConnection. UI is expected to prompt the operator and call
    // accept/rejectPresentedWanCert() in response.
    void certFingerprintMismatch(const QString& host,
                                 const QString& expectedHex,
                                 const QString& presentedHex);
    // Emitted when another GUI client forces this client to disconnect.
    void forcedDisconnectRequested();
    // Emitted before teardown when the radio rejects `client gui`. The UI uses
    // this terminal signal to stop reconnect UX and preserve the radio's reason.
    void guiClientRegistrationFailed(const QString& message);
    // aetherd Gap B (HL2 Phase 1c, Step 1): the backend-neutral panadapter render
    // feed. Signatures mirror PanadapterStream.h:212-218 exactly, so the UI binds
    // its panadapter/waterfall rendering to these instead of the Flex-only
    // PanadapterStream and the wiring is family-agnostic. A Flex session forwards
    // its PanadapterStream into these 1:1 (signal-to-signal, no transformation);
    // an HL2 session (Step 2) synthesises them from IRadioBackend::spectrumFrameReady.
    // Per-RadioModel → per-session, which is exactly what the two-panadapter end
    // state needs.
    void panFeedSpectrumReady(quint32 streamId, const QVector<float>& binsDbm,
                              qint64 emittedNs);
    void panFeedWaterfallRowReady(quint32 streamId, const QVector<float>& binsDbm,
                                  double lowFreqMhz, double highFreqMhz,
                                  quint32 timecode, qint64 emittedNs);
    void panFeedWaterfallAutoBlackLevel(quint32 streamId, quint32 autoBlack);
    // Demodulated RX audio from a backend that produces it in-process (HL2).
    // 24 kHz stereo float32 — the format AudioEngine::feedAudioData expects.
    // Flex never emits this; its audio arrives on the PanadapterStream path.
    void backendAudioFrameReady(const QByteArray& pcm);
    // ONE slice's demodulated audio, relayed from IRadioBackend. The per-slice
    // counterpart of backendAudioFrameReady, which is the mixed speaker feed.
    // Only a backend that demodulates in this process emits it; Flex per-slice
    // audio arrives as DAX channels instead.
    void backendSliceAudioFrameReady(int sliceId, const QByteArray& pcm);

    // ── The normalized demodulated-RX-audio bus ────────────────────────────
    //
    // The audio the OPERATOR HEARS, whoever produced it: 24 kHz interleaved
    // stereo float32, byte-identical to what both producers already emit.
    //
    // Exactly one producer is connected at a time — PanadapterStream::
    // audioDataReady for a Flex, IRadioBackend::audioFrameReady for a backend
    // that answers ownsRxAudio() — and that choice is made in ONE place
    // (wireRxDemodAudioBus). Consumers subscribe once and never rebind, because
    // this signal belongs to RadioModel, which OUTLIVES the backend swap that
    // destroys and rebuilds a PanadapterStream.
    //
    // That is the actual bug this fixes, and it is a shape rather than an
    // instance. Three features — the CW decoder, the RTTY decoder and the QSO
    // recorder's RX tap — were bound directly to the Flex stream, so on any
    // radio without one they bound to nothing: no error, no log line, the
    // toggle worked and nothing ever decoded. See HERMES.md §18.
    //
    // Deliberately NOT the speaker path. AudioEngine::feedAudioData keeps its
    // existing per-family wiring untouched, so nothing audible changes on any
    // radio; this carries the taps that listen alongside it.
    //
    // Named for the tap it carries. A future filter-flat, pre-AGC feed for
    // modems is a SEPARATE signal (rxWidebandAudioReady), not a mode flag on
    // this one — see HERMES.md §18.5.
    void rxDemodAudioReady(const QByteArray& pcm24kStereoFloat);
    // The backend was replaced because the operator picked a radio of another
    // family. Consumers holding backend-owned objects (PanadapterStream) must
    // re-establish anything that binds to them directly.
    void backendRebuilt();
    // Emitted when a panadapter's center frequency or bandwidth changes.
    void panadapterInfoChanged(double centerMhz, double bandwidthMhz);
    // Emitted when the radio reports the panadapter's dBm display range.
    void panadapterLevelChanged(float minDbm, float maxDbm);
    // Emitted when the backend reports the span limits this pan can be zoomed
    // between (MHz). Per-pan and addressed, because the limits belong to the
    // receiver behind the pan rather than to the radio as a whole — a backend
    // that never reports leaves the GUI on its model-derived clamp.
    void panBandwidthLimitsChanged(const QString& panId,
                                   double minMhz, double maxMhz);
    // Emitted when the radio reports FFT pixel height for a panadapter.
    void panadapterFftScaleChanged(const QString& panId, int yPixels);
    void panadapterAdded(PanadapterModel* pan);
    // Emitted when a previous-session pan model is reclaimed on reconnect
    // instead of created fresh. The applet/widget wiring from the original
    // panadapterAdded survives (model and widget both outlive the disconnect),
    // but connections MainWindow tears down at disconnect (the per-pan
    // radio-status display connections wired by wirePanDisplayStatus(), #4261)
    // must be re-established from this.
    void panadapterReclaimed(PanadapterModel* pan);
    void panadapterRemoved(const QString& panId);
    // Brackets the actual wire dispatch of a radio-authoritative band-stack
    // recall. Unlike requestPanBand(), these signals fire after any profile-
    // load deferral, so clients can order dependent radio commands around the
    // real `display pan set ... band=...` write.
    void panBandAboutToDispatch(const QString& panId);
    void panBandDispatchFailed(const QString& panId);
    // Emitted when createPanadapter() is blocked because the radio's pan limit is reached.
    void panadapterLimitReached(int limit, const QString& model);
    // Emitted when the radio rejects a slice create command (e.g. limit reached across
    // all Multi-Flex clients — our local slice count may be below maxSlices()).
    void sliceCreateFailed(int limit, const QString& model);
    // Emitted when a pan needs xpixels/ypixels pushed (after profile change, reconnect, etc.)
    void panDimensionsNeeded(const QString& panId);
    // Emitted when the radio reports its antenna list (e.g. "ANT1,ANT2,RX_A,RX_B").
    void antListChanged(QStringList ants);
    // Local AetherSDR display aliases changed. The radio still uses canonical tokens.
    void antennaAliasesChanged();
    // (Amplifier presence/state/telemetry signals moved to AmpModel — bind
    //  m_radioModel.amplifier() directly. #4094.)
    void memoryChanged(int index);
    void memoryRemoved(int index);
    void memoriesCleared();
    void audioOutputChanged();
    // Emitted when multiFLEX is disabled and another client is already connected,
    // detected post-TCP-connect before client gui is sent. MainWindow should show
    // ConnectedStationsDialog and call resolveMultiFlexConflict() or cancelMultiFlexConflict().
    void multiFlexConflictDetected();
    // Emitted when TX ownership changes in Multi-Flex (another client transmitting)
    void txOwnerChanged(bool ownedByUs, const QString& otherStation);
    // Emitted when a global slice slot transitions between ours / foreign /
    // empty states.  UI listens to this to re-style slice-tab buttons
    // (e.g. RxApplet dims foreign slots).
    void slotOccupancyChanged(int sliceId);
    // Emitted when the set of other connected clients changes.
    void otherClientsChanged(int count, const QStringList& names);
    // Emitted when another GUI client logs in after our known client list.
    void clientConnected(quint32 handle,
                         const QString& source,
                         const QString& station,
                         const QString& program);
    // Emitted when GPS status changes (from "sub gps all").
    void gpsStatusChanged(const QString& status, int tracked, int visible,
                          const QString& grid, const QString& altitude,
                          const QString& lat, const QString& lon,
                          const QString& utcTime);
    // Emitted when the station callsign becomes known or changes (from the
    // radio "info"/status feed). Lets features like the PSK Reporter map pick
    // up a late-arriving or edited callsign without a reconnect.
    void callsignChanged(const QString& callsign);
    // Emitted when the radio reports 10 MHz reference oscillator state.
    void oscillatorChanged();
    // Emitted when network quality assessment changes.
    // quality: "Off", "Excellent", "Very Good", "Good", "Fair", "Poor"
    // pingMs: round-trip time in milliseconds
    void networkQualityChanged(const QString& quality, int pingMs);
    // Emitted when the radio assigns a TX audio stream ID (DAX TX).
    void txAudioStreamReady(quint32 streamId);
    // Emitted when the radio assigns a remote audio TX stream ID (voice/VOX).
    void remoteTxStreamReady(quint32 streamId);
    // Audio TX gate for sample pipeline (separate from optimistic MOX UI state).
    void txAudioGateChanged(bool transmitting);
    // Raw interlock TX state (regardless of ownership — for DAX passthrough).
    void radioTransmittingChanged(bool transmitting);
    // Operator-driven RF transmit: true while THIS seat is keyed by the local
    // operator in a phone/data mode (MOX, local/hardware PTT, footswitch, VOX)
    // and false otherwise. Deliberately excludes TUNE/two-tone/ATU carriers,
    // TCI-hardware/DAX transmits (external-app keying paths, not the operator
    // on the mic), and CW (break-in/QSK per-element keying would thrash a
    // wall-clock timer). Drives the status-bar TX timer.
    void operatorTransmitChanged(bool active);
    // Short operator-facing interlock warnings for the panadapter overlay.
    // `key` is the stable, translation-invariant dedup key (e.g. "radio:...",
    // "pan-tx-inhibit:...") so the UI can classify the notice without sniffing
    // the localized message text.
    void interlockNotificationRequested(const QString& message,
                                        const QString& key,
                                        const QString& panId);
    // Emitted at most once per transmission when the TX filter is measurably
    // removing the operator's transmit audio (#4649). Carries a ready-to-show
    // operator-facing message naming the offending passband.
    void txFilterBlockingAudio(const QString& title,
                               const QString& detail,
                               const QString& panId);
    // Emitted when global profile list or active profile changes.
    void globalProfilesChanged();
    void profileDatabaseImportingChanged(bool importing);
    void profileDatabaseExportingChanged(bool exporting);
    // Emitted when a profile load command is sent, before the radio tears down
    // and rebuilds profile-owned slices/pans.
    void profileLoadStarted(const QString& profileType, const QString& profileName);
    // Emitted after the radio accepts a profile load command. Profile recall can
    // tear down per-session streams; GUI/session code should re-arm client-owned
    // state from this signal instead of guessing from later status churn.
    void profileLoadCompleted(const QString& profileType, const QString& profileName);

    // Emitted when the radio reports a change to the global Auto-Save
    // profile setting (radio status field "auto_save").  UI consumers
    // should react via this signal rather than polling autoSave() — the
    // value can be flipped by the Auto-Save tab, the Profile Manager
    // "Enable Auto-Save" affirmation, TCI clients, profile load, and
    // other connected SmartSDR clients.
    void autoSaveChanged(bool autoSave);
    // Emitted on each successful ping response from the radio.
    void pingReceived();
    // Emitted when adaptive frame-rate throttling engages or lifts.
    // active=true: all pans are being throttled to fpsCap fps to reduce UDP load.
    // active=false: throttle lifted; receivers should restore user-configured fps.
    void adaptiveThrottleChanged(bool active, int fpsCap);
    // Local waveform diagnostics are proxied through the model so GUI
    // consumers do not depend on the helper process implementation.
    void digitalVoiceWaveformMetricsChanged();
    void digitalVoiceWaveformHealthChanged();
    void digitalVoiceWaveformDegradationStarted(const QString& message);
    // Generic status relay — for dialogs that need to listen for specific objects.
    void statusReceived(const QString& object, const QMap<QString, QString>& kvs);
    // Emitted when the radio sends an M-prefix informational, warning, error,
    // or fatal message.  Severity comes from the high bits of the message
    // number per FlexLib Radio.cs:4498-4516.  MainWindow uses it to decide
    // log-only vs. modal-dialog surfacing — Info-severity messages like
    // "Client connected from IP …" must NOT pop a dialog (#2785).
    void radioMessageReceived(const QString& text, MessageSeverity severity);

public:
    // Send a raw command to the radio (for dialogs that need direct protocol
    // access). Returns whether the command was actually dispatched: false when
    // the foreign-owner gate (#3977) drops a pan write, when the profile-load
    // hold backstop suppresses a profile-owned write (#4142), or when a WAN
    // session is not connected. The WAN case is the only transport failure
    // this contract can see: the LAN path allocates a sequence number
    // unconditionally, so a write queued onto a dead LAN socket still reports
    // as dispatched (pre-existing optimistic behavior; a disconnect instead
    // voids any deferred pan writes via onDisconnected()). Callers that
    // advance local state to match a command MUST gate that on this return,
    // or the client will claim state the radio never took.
    bool sendCommand(const QString& cmd);
    // Backend family currently in use ("flex", "hl2", "icom", "sim", ...).
    QString family() const { return m_family; }

    // Flush any pending operating-state capture immediately (RFC #4603 PR 3).
    // PUBLIC because MainWindow::closeEvent() must call it explicitly: quit
    // tears down without pumping the event loop, so the queued
    // disconnect→flush path never runs there (PR #4619 review — same class
    // as the explicit TGXL/D-STAR teardown in closeEvent).
    void flushPendingOperatingState();

    // The (family, radio) handle into the radio-scoped feature-document store
    // (RFC #4603). Identity is the family's canonical serial (Flex serial /
    // HL2 MAC); an unconnected model yields a family-wide scope.
    RadioSettingsScope settingsScope() const
    {
        return RadioSettingsScope(m_family, serial());
    }

    // Fire a vendor-extension verb at the connected backend (IRadioBackend
    // §"vendor-extension"). Generic on purpose: the model stays free of any one
    // family's verb vocabulary, exactly as it does for the tuner and amp
    // intents it already routes this way. requestId 0 means no reply is
    // expected; a caller that wants one connects to the backend's
    // extensionResult/extensionError and correlates its own id.
    //
    // A no-op when nothing is connected, rather than a crash or a queued call
    // that lands on the next radio.
    void invokeBackendExtension(const QString& ns, const QString& verb,
                                quint64 requestId = 0, const QVariant& arg = {});
    // True when the radio speaks the SmartSDR text-command plane — the only
    // family where sendCmd() reaches anything and a command has a response to
    // await. Every other backend takes typed intents through the IRadioBackend
    // seam and settles synchronously in the model, so a caller that awaits a
    // sendCmd() callback there waits forever. Callers that must work on both
    // planes branch on this rather than on the family name.
    bool usesFlexCommandPlane() const { return m_family == QLatin1String("flex"); }

    // True when the ENGINE owns display rate shaping rather than the radio —
    // i.e. a backend that streams raw spectra and has no radio-side display
    // engine to ask. The single predicate behind requestPanDisplayRates' branch
    // and the seeding in MainWindow, so the two cannot drift into disagreeing
    // about who is in charge of the frame rate.
    bool shapesDisplayRatesLocally() const {
        return m_backend != nullptr && m_flexBackend == nullptr;
    }
    // Forward processed transmit audio to a host-modulating backend. No-op when
    // the backend modulates on the radio side.
    void submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz);
    // Let receive audio through while transmitting. Diagnostic use only — see
    // IRadioBackend::setTxAudioMonitor.
    void setTxAudioMonitor(bool on);

    // Whether the BACKEND reports it can transmit.
    //
    // Distinct from capabilities(), which is the Flex model-name lookup. Note
    // that IRadioBackend::capabilities().canTransmit had no consumer anywhere
    // outside the backends when this was added — the same shape as meterUpdate
    // and setKeying, both of which were wired to nothing.
    bool backendCanTransmit() const;

    // Request local PTT for our station. Sends "client set local_ptt=1" and applies
    // an optimistic update in case the radio doesn't echo the state change.
    void requestLocalPtt();

    // PC Audio: create/remove remote_audio_rx stream
    void createRxAudioStream();
    void removeRxAudioStream();

    // Send a command with a response callback (for firmware uploader, etc.)
    void sendCmdPublic(const QString& cmd, std::function<void(int code, const QString& body)> cb);
    void requestFileUploadPort(qint64 size, const QString& uploadKind,
                               std::function<void(int code, const QString& body)> cb);
    void requestFileDownloadPort(const QString& downloadKind,
                                 std::function<void(int code, const QString& body)> cb);
    void refreshProfiles();
    bool isProfileTransferBlocked() const;
    bool profileDatabaseImporting() const { return m_profileDatabaseImporting; }
    bool profileDatabaseExporting() const { return m_profileDatabaseExporting; }

    // Radio software version string (from discovery broadcast, e.g. "4.1.5")
    QString softwareVersion() const { return m_version; }
    // SmartSDR protocol version from the V line (e.g. "1.4.0.0"), empty until connected
    QString protocolVersion() const { return m_protocolVersion; }

private slots:
    void onStatusReceived(const QString& object, const QMap<QString, QString>& kvs);
    void onMessageReceived(const ParsedMessage& msg);
    void onConnected();
    void onDisconnected();
    void onConnectionError(const QString& msg);
    void onVersionReceived(const QString& version);
    // aetherd Gap B (Step 2): adapt a backend that delivers spectra via the
    // normalized IRadioBackend data-plane signal (e.g. HL2) into the neutral
    // panFeed. Decodes the float32 frame and re-emits panFeedSpectrumReady. Flex
    // never triggers this (it feeds panFeed via the PanadapterStream passthrough).
    void onBackendSpectrumFrame(int panId, const QByteArray& frame);

private:
    void handleRadioStatus(const QMap<QString, QString>& kvs);
    // Apply a normalized radio-global delta from the backend
    // (IRadioBackend::radioChanged). aetherd RFC 2.3 — RadioModel residual.
    void applyRadioChanges(const RadioDelta& delta);
    // Apply normalized GPS / memory-slot / profile deltas from the backend
    // (IRadioBackend::gpsChanged / memoryChanged / profileChanged). aetherd RFC
    // 2.3 — RadioModel residual.
    void applyGpsChanges(const GpsDelta& delta);
    void applyMemoryChanges(const MemoryDelta& delta);
    void applyProfileChanges(const ProfileDelta& delta);
    void handleSliceStatus(int id, const QMap<QString, QString>& kvs, bool removed);
    void scheduleDStarRuntimeConfiguration();
    void applyPendingDStarRuntimeConfiguration();
    void syncDigitalVoiceTxSelection(bool force = false);
    void handleMeterStatus(const QString& rawBody);
    void handlePanadapterStatus(const QString& panId, const QMap<QString, QString>& kvs);
    void handleProfileStatus(const QString& object, const QMap<QString, QString>& kvs);
    void handleProfileStatusRaw(const QString& profileType, const QString& rawBody);
    void traceDaxStreamStatus(const QString& object, const QMap<QString, QString>& kvs);
    void handleDaxRxStreamRegistry(const QString& object, const QMap<QString, QString>& kvs);
    bool handleRemoteAudioRxStreamStatus(const QString& object,
                                         const QMap<QString, QString>& kvs);
    void scheduleRxAudioStreamEnsure(const QString& reason);
    void logRemoteAudioRxSummary(const QString& reason) const;

    void configurePan(const QString& panId);
    void configureWaterfall(const QString& waterfallId);
    // Sends the radio auto_black flag from the combined auto-black on/off +
    // client/radio-source state (auto_black=1 only when both select radio-side).
    void applyWaterfallAutoBlack();
    bool m_wfAutoBlackOn{true};         // mirrors the client auto-black on/off
    bool m_wfAutoBlackRadioSide{false}; // false = client-side, true = radio-side
    bool profileLoadRadioStateWritesHeld() const;
    // Raw senders (#4142). Both keep ALL wire-string building for the pan
    // touchpoint in one place (AGENTS.md seam step 2.4: command encode migrates
    // to FlexBackend per-touchpoint — these two functions are that touchpoint).
    //
    // dispatchPanCenterBandwidth re-clamps the center against the pan's
    // CURRENT geometry, puts the command on the wire FIRST, and advances the
    // model only when the send actually happened — wire-before-model is what
    // makes a re-entrant request converge (last wire write == last model
    // write) instead of diverging. bandwidthMhz <= 0 means center-only;
    // centerMhz as NaN means bandwidth-only.
    // intent is forwarded to the seam untouched — see requestPanCenter(). A
    // write replayed out of the profile-load queue is Range by construction:
    // the queue stores geometry, not who asked for it, and Range is the value
    // that cannot move a radio.
    bool dispatchPanCenterBandwidth(const QString& panId,
                                    double centerMhz,
                                    double bandwidthMhz,
                                    IRadioBackend::PanCenterIntent intent =
                                        IRadioBackend::PanCenterIntent::Range);
    // No model write: band-stack state arrives via radio status.
    bool dispatchPanBand(const QString& panId, const QString& bandKey);
    // Schedules the deferred-pan-write replay for when the hold lifts. Armed by
    // the act of deferring, NOT by the profile-load ACK — a large topology can
    // stall the radio into a disconnect before it ever ACKs. (#4142)
    void armProfileLoadPanWriteFlush();
    // Voids one pan's deferred writes, loudly naming what was destroyed.
    void voidPendingPanWrites(const QString& panId, const QString& reason);
    void registerAsGuiClient(const QString& clientId);
    void handleGuiClientRegistrationFailure(
        const GuiClientRegistrationState::Result& result);
    void disconnectPendingClientsThen(std::function<void()> continuation);
    // LAN-only: subscribe to radio+client topics early, wait 400 ms for
    // the radio status burst, then check for a multiFLEX conflict before
    // sending client gui. Calls continuation() if no conflict is found.
    void peekForMultiFlexConflictThen(std::function<void()> continuation);
    void handleForcedClientDisconnect();
    void handleDuplicateClientIdDisconnect();
    // Shared transport teardown for a terminal session failure (forced,
    // duplicate-client-id, or rejected GUI registration). Callers set
    // m_intentionalDisconnect first.
    void closeConnectionForTerminalDisconnect();
    void resolveLiveGuiClientIdCollision();
    void applyKnownGuiClients(const QStringList& handles,
                              const QStringList& programs,
                              const QStringList& stations,
                              const QStringList& ips,
                              const QStringList& hosts,
                              bool replaceExisting);
    void armClientConnectionNoticeSuppression();
    bool clientConnectionNoticeSuppressionActive() const;
    bool shouldSuppressRadioMessageNotice(const QString& text, MessageSeverity severity) const;
    bool shouldSuppressClientConnectionNotice(quint32 handle);
    void announceClientConnection(quint32 handle,
                                  const QString& source,
                                  const QString& station,
                                  const QString& program);
    void disconnectClientHandlesThen(const QList<quint32>& handles,
                                     std::function<void()> continuation = {});

    // Route command to active connection (LAN or WAN)
    using ResponseCallback = RadioConnection::ResponseCallback;
    quint32 sendCmd(const QString& command, ResponseCallback cb = nullptr);
    quint32 clientHandle() const;
    PanadapterModel* ensureOwnedPanadapter(const QString& panId);
    void updateStreamFilters();
    void handleGpsStatus(const QString& rawBody);
    void emitOtherClientsChanged();
    QString antennaAliasRadioKey() const;
    bool reloadAntennaAliases() const;

    // Standalone mode: create a panadapter then attach a slice to it.
    void createDefaultSlice(const QString& freqMhz = "14.225000",
                            const QString& mode    = "USB",
                            const QString& antenna = "ANT1");
    void createDefaultSliceOnPan(const QString& panId,
                                 const QString& freqMhz,
                                 const QString& mode,
                                 const QString& antenna);
    // Reconnect recovery (#3212): the radio's GUIClientID session restore can
    // bring back our panadapter without a slice, so "slice list" returns empty.
    // Reuse the already-claimed pan instead of allocating a second one; only
    // fall back to createDefaultSlice() (which issues "display panafall create")
    // when no owned pan exists yet.
    void ensureDefaultSlicePreferringRestoredPan();
    void stageSessionModelsForReconnect();
    void pruneStaleSessionModels(quint64 generation);
    // Destroy every live AND staged slice/pan model unconditionally. Called on a
    // radio-family switch, which is a hard radio change: no model from the old
    // family may survive to be reclaimed as one of the new family (their slice
    // indices and stream ids collide, and their command/TX/DV wiring differs, so
    // a reclaimed cross-family slice is a partially-wired, broken slice). The
    // serial-based reclaim guard cannot cover this — HL2 carries no chassis
    // serial — so the switch drops the models outright. (#4448)
    void dropAllSessionModelsForFamilySwitch();

    // aetherd Gap A (HL2 Phase 1c): the minimal backend-selection seam. Returns
    // the IRadioBackend for `family` ("flex" default, "hl2" for Hermes-Lite 2,
    // "icom" for Icom networked radios, "sim" for demo mode).
    // Replaces the hard-wired make_unique<FlexBackend>; a fuller step-3 registry
    // supersedes it later. Flex-specific construction wiring (command sinks,
    // RadioConnection/PanadapterStream grabs) stays behind a dynamic_cast adapter
    // in the ctor, so a non-Flex backend simply skips it.
    static std::unique_ptr<IRadioBackend> makeBackend(const QString& family);
    void handRestoredStateToBackend(const QString& serial);  // RFC #4603
    void persistOperatingState(bool force = false);          // RFC #4603 PR 3

    // aetherd Gap B: build/destroy the backend for a radio family. The backend
    // follows the radio the operator picks in the connection manager, so these
    // run again on a family change — not just at construction.
    void setupBackend(const QString& family);
    void teardownBackend();
    // Push the backend's RadioCapabilities into the models that own each flag,
    // then emit capabilitiesChanged. Called on every connect/disconnect edge and
    // whenever the backend revises its own capabilities.
    void publishCapabilities(bool connected);
    // Bind the one producer for rxDemodAudioReady. Idempotent; call after
    // m_backend and m_panStream are both settled for the new family.
    void wireRxDemodAudioBus();

    // aetherd RFC step 2 (§5.5): the radio-facing seam. Held via std::unique_ptr
    // (owned via unique_ptr below). As of 2.2b it OWNS the RadioConnection +
    // PanadapterStream and their worker threads; RadioModel keeps the two
    // NON-OWNING pointers below, obtained from the backend at construction.
    // Radio family the live backend implements ("flex", "hl2", "icom"). Set by
    // setupBackend(); compared against the picked radio's RadioInfo::family to
    // decide whether a connect needs a different backend.
    QString m_family;
    std::unique_ptr<IRadioBackend> m_backend;
    // RFC #4288 Route A: when true, m_backend is a wire-less SimBackend (the demo
    // simulator) instead of a FlexBackend. Selected per-connection from the target
    // — see connectToRadio(). Also generalizes to future non-Flex backends (HL2).
    // Transitional (aetherd RFC 2.3): RadioModel drives the backend's Flex
    // status decode from its status choke points while touchpoints convert
    // one at a time. Non-owning alias of m_backend; goes away once the backend
    // owns status ingress (the protocol step). Concrete type because the decode
    // input is vendor-specific; the outputs are normalized interface signals.
    FlexBackend* m_flexBackend{nullptr};
    RadioConnection*  m_connection{nullptr};   // non-owning — owned by m_backend
    // Sequence counter and callback map — owned by RadioModel on main thread.
    // RadioConnection no longer manages callbacks. (#502)
    std::atomic<quint32> m_seqCounter{1};
    QMap<quint32, ResponseCallback> m_pendingCallbacks;
    PanadapterStream* m_panStream{nullptr};    // non-owning — owned by m_backend
    // The single live producer feeding rxDemodAudioReady. Held so re-entry can
    // drop the previous one: connecting both producers is the double-feed that
    // made the engine consume at double rate in #4490, and here it would make
    // every decoder see each block twice.
    QMetaObject::Connection m_rxDemodBusConn;
    // aetherd Gap B (Step 2c): geometry + row counter for backends that deliver
    // spectra through IRadioBackend (HL2). Kept on RadioModel because such a
    // backend has no PanadapterModel yet, and the neutral waterfall rows still
    // need frequency edges.
    // Per-pan, keyed by NEUTRAL pan index, because a backend may run several
    // receivers at once (HL2 runs up to four). These were single scalars while
    // only one non-Flex pan could exist, which quietly made every extra pan's
    // waterfall scale against the FIRST pan's band edges.
    QHash<int, double> m_backendPanCenterMhz;
    QHash<int, double> m_backendPanBandwidthMhz;
    quint32 m_backendWfTimecode{0};

    // Backend pan id ("hl2-2") -> neutral pan index, allocated in first-seen
    // order. The backend's ids are OPAQUE here: RadioModel must not parse a
    // family's naming scheme, and a backend must not have to know about the
    // 0xE1000000 stream-id space. This table is the translation between them.
    QHash<QString, int> m_backendPanIndex;
    // The inverse, for commands going DOWN. Filled alongside the forward map.
    QHash<int, QString> m_backendPanIdByIndex;
    // Allocate (or recall) the neutral index for a backend pan id.
    int neutralPanIndexFor(const QString& backendPanId);
    // Resolve a BACKEND-namespaced pan id to its PanadapterModel, translating
    // through the neutral index. Every IRadioBackend pan signal must use this
    // rather than resolvePan(), whose active-pan fallback silently misdirects a
    // multi-pan backend's updates. See the definition.
public:
    // Test hooks for the backend<->model pan-id mapping.
    //
    // Exposed because the property they pin is exactly the one that broke: the
    // mapping was built in ONE direction, so every pan signal arrived correctly
    // and every pan COMMAND was refused by a backend that could not resolve the
    // model's id. A round-trip assertion is the cheapest thing that fails when
    // half of a two-way mapping goes missing.
    int panIndexForBackendIdForTest(const QString& backendPanId)
    {
        return neutralPanIndexFor(backendPanId);
    }
    QString backendPanIdForTest(const QString& modelPanId) const
    {
        return backendPanIdFor(modelPanId);
    }
    static QString neutralPanIdStringForTest(int panIdx);

private:
    PanadapterModel* resolveBackendPan(const QString& backendPanId);
    // Connect a slice's operator-issued AUDIO and TX-slice intents to the
    // backend seam. Must be called from EVERY site that constructs a
    // SliceModel — see the definition for why that is not a style preference.
    void wireSliceAudioIntentsToBackend(SliceModel* s);
    // Translate a MODEL pan id to the backend's own id for a command going down
    // the seam. The inverse of resolveBackendPan(); both are needed or the
    // mapping is one-way and every pan command addresses a pan the backend
    // cannot resolve. Identity for Flex.
    QString backendPanIdFor(const QString& modelPanId) const;

    // ---- waterfall pacing for raw-spectrum backends (HL2) ------------------
    //
    // The PAN rate is capped at the source (IRadioBackend::setPanFrameRate), so
    // frames arrive here already at the operator's FFT FPS. The waterfall runs
    // SLOWER than that — line_duration is its own control and typically 100 ms
    // against 25-40 fps — so it needs one more gate, and only in that
    // direction.
    //
    // A plain drop, not a coalesce. Frames are already scarce by the time they
    // reach here, and combining them would mean a magnitude/log round trip per
    // bin per frame for no gain the operator can see.
    //
    // It is also correctness, not just load: the widget scales its time axis
    // from line_duration, so a row must actually represent line_duration of
    // time. Unpaced, rows arrived at the full frame rate and the visible
    // history was several times shorter than the axis claimed.
    QHash<int, qint64> m_backendWfLastRowNs;
    // Covers only the window before MainWindow seeds the pan model from the
    // operator's sliders. 100 is the top of the 1..100 rate control and matches
    // SpectrumWidget's own m_wfLineDuration default, so a self-shaping backend
    // starts at full speed rather than at the 10 fps the number used to mean
    // when it was read as milliseconds (#4606).
    static constexpr int kBackendDefaultWfRate = 100;
    // Sub-models — value members on main thread (#502)
    MeterModel       m_meterModel;
    // Epoch ms of the last arrival of each class; 0 = never. Written on the
    // hot path, so they are plain scalars rather than anything that allocates.
    qint64 m_lastSpectrumMs{0};
    qint64 m_lastAudioMs{0};
    TunerModel       m_tunerModel;
    TransmitModel    m_transmitModel;
    EqualizerModel   m_equalizerModel;
    TnfModel         m_tnfModel;
    SpotModel        m_spotModel;
    CwxModel         m_cwxModel;
    DvkModel         m_dvkModel;
    NavtexModel         m_navtexModel;
    UsbCableModel       m_usbCableModel;
    DaxIqModel          m_daxIqModel;
    FlexWaveformModel   m_flexWaveformModel;
    DStarModel          m_dstarModel{nullptr, true};

    // NetCW stream — VITA-49 UDP delivery for low-latency CW keying
    quint32  m_netCwStreamId{0};
    int      m_netCwIndex{1};           // sequential dedup index
    QElapsedTimer m_netCwClock;          // 16-bit relative ms clock for time=0x....
    qint64   m_netCwLastSendMs{-1};
    void sendNetCwCommand(const QString& cmd, const QString& debugSource = {},
                          quint64 debugTraceId = 0, quint64 debugSourceMs = 0);
    QByteArray buildNetCwPacket(const QByteArray& payload);

    QString     m_name;
    QString     m_model;
    QStringList m_declaredBands;    // optional "bands=" declaration (see declaredBands())
    int         m_maxSlices{4};
    QString     m_version;          // software version from discovery (e.g. "4.1.5")
    QString     m_versionLabel;     // display-only word for it (Gateware on an HL2)
    QString     m_protocolVersion;  // protocol version from V line (e.g. "1.4.0.0")
    float       m_paTemp{0.0f};
    float       m_txPower{0.0f};
    QString     m_chassisSerial;
    QString     m_callsign;
    QString     m_nickname;
    QString     m_region;
    QString     m_radioOptions;
    QString     m_licenseRadioId;
    QString     m_licenseExpirationDate;
    QString     m_licenseMaxVersion;
    QString     m_licenseSubscription;   // e.g. "SmartSDR+", "SmartSDR", "Unknown"
    QHash<QString, LicenseFeatureState> m_licenseFeatures;
    QString     m_ip;
    QString     m_netmask;
    QString     m_gateway;
    QString     m_mac;
    bool        m_hasStaticIp{false};
    QString     m_staticIp;
    QString     m_staticNetmask;
    QString     m_staticGateway;
    // Oscillator state
    QString     m_oscState;
    QString     m_oscSetting{"auto"};
    bool        m_oscLocked{false};
    bool        m_extPresent{false};
    bool        m_gpsdoPresent{false};
    bool        m_tcxoPresent{false};
    bool        m_binauralRx{false};
    bool        m_muteLocalWhenRemote{false};
    bool        m_autoSave{true};
    int         m_lineoutGain{50};
    bool        m_lineoutMute{false};
    int         m_headphoneGain{50};
    bool        m_headphoneMute{false};
    bool        m_frontSpeakerMute{false};
    int         m_freqErrorPpb{0};
    double      m_calFreqMhz{15.0};
    int         m_filterVoice{2};
    bool        m_filterVoiceAuto{false};
    int         m_filterCw{2};
    bool        m_filterCwAuto{true};
    int         m_filterDigital{2};
    bool        m_filterDigitalAuto{true};
    bool        m_lowLatencyDigital{true};
    bool        m_enforcePrivateIp{true};
    bool        m_remoteOnEnabled{false};
    bool        m_multiFlexEnabled{true};
    bool        m_txRequested{false}; // local MOX command intent (for edge sync)
    bool        m_cwKeyActive{false}; // true while CW key/paddle is held (#1379)
    bool        m_cwxActive{false};   // true while CWX send is in flight (#2047, #2097)
    bool        m_cwxDrainArmed{false}; // CWX drain-release latch, immune to interlock flicker (#3949)
    bool        m_txAudioGate{false}; // actual TX audio gate state
    bool        m_radioTransmitting{false}; // raw interlock TX state, any owner
    bool        m_operatorTransmitting{false}; // owned MOX/PTT/VOX (not tune/ATU/TCI/DAX)
    int         m_txFilterKillSamples{0};      // consecutive qualifying meter packets (#4649)
    bool        m_txFilterKillReported{false}; // latched, so we speak once per transmission
    QString     m_lastInterlockNotificationKey;
    qint64      m_lastInterlockNotificationMs{0};
    qint64      m_interlockNotificationArmedUntilMs{0};
    TransmitModel::PttSource m_pendingTransmitPreflightSource{TransmitModel::PttSource::Mox};
    TransmitModel::PttSource m_interlockNotificationSource{TransmitModel::PttSource::Mox};
    int         m_digitalVoiceTxSliceId{-1};
    QString     m_lastDigitalVoiceTxSelectionKey;
    bool        m_dstarRuntimeConfigurationPending{false};
    QString     m_lastInterlockSource;   // last seen interlock source= (#2373)
                                         // SW/MIC/ACC/RCA/TUNE per FlexLib
                                         // v4.2.18 ParsePTTSource. Persists
                                         // across status updates that omit
                                         // the field; cleared on non-TX
                                         // interlock states and on disconnect.
    QStringList m_antList;
    mutable QString m_antennaAliasRadioKey;
    mutable QMap<QString, QString> m_antennaAliases;

    QMap<QString, PanadapterModel*> m_panadapters;  // panId → model
    QMap<QString, PanadapterModel*> m_stalePanadapters;  // previous session, kept alive for UI reuse
    // #3977: eviction bookkeeping, all cleared on disconnect (handles are
    // radio-boot-scoped and recycled). m_evictedPredecessorHandles holds
    // radio-CONFIRMED disconnects; m_evictionsInFlight guards double-sends
    // while a `client disconnect` reply is pending.
    QSet<quint32> m_evictedPredecessorHandles;
    QSet<quint32> m_evictionsInFlight;
    QMap<quint32, ForeignPanWrite> m_foreignPanWrites;
    void noteForeignPanWriteIfAny(const QString& object,
                                  const QMap<QString, QString>& kvs,
                                  quint32 sourceHandle);
    void evictStaleSession(quint32 handle, const QString& reason);
    QString m_activePanId;       // currently active panadapter

    // Radio-authoritative display inventory (#3856 Layer B). Accumulated from
    // ALL "display pan"/"display waterfall" status (ours, foreign, and orphan),
    // pruned on the matching "removed" — independent of m_panadapters, which only
    // holds objects WE own. A leaked waterfall (panafall closed without
    // "display panafall remove") therefore lingers here after its pan is pruned,
    // making it detectable even when the radio has stopped streaming it.
    // Main-thread only (written in onStatusReceived, read in the bridge).
    struct RadioDisplayPan { quint32 clientHandle{0}; QString waterfallId; };
    struct RadioDisplayWf  { quint32 clientHandle{0}; QString parentPanId; };
    QMap<QString, RadioDisplayPan> m_radioDisplayPans;        // panId → entry
    QMap<QString, RadioDisplayWf>  m_radioDisplayWaterfalls;  // wfId  → entry
    // Deferred "display pan" status pending ownership confirmation. Paired
    // with QDateTime::currentSecsSinceEpoch() at insert so a sweep on insert
    // can drop entries that the radio never resolved with a client_handle
    // frame (#2228 — would otherwise leak for non-owned pans that also never
    // emit "removed").
    QMap<QString, QPair<qint64, QMap<QString, QString>>> m_pendingPanStatuses;

    AmpModel m_amplifier;            // power amp (PGXL) state + relay (#4094)

    // GPS state
    QString m_gpsStatus;           // "Locked", "Present", "Not Present"
    int     m_gpsTracked{0};
    int     m_gpsVisible{0};
    QString m_gpsGrid;
    QString m_gpsAltitude;
    QString m_gpsLat;
    QString m_gpsLon;
    QString m_gpsTime;
    QString m_gpsSpeed;
    QString m_gpsTrack;
    QString m_gpsFreqError;
    QString m_automationGpsNtpServerAddress;

    // Per-band TX settings (from "transmit band" and "interlock band" status)
    struct TxBandInfo {
        int     bandId{0};
        QString bandName;
        int     rfPower{100};
        int     tunePower{10};
        bool    inhibit{false};
        bool    hwAlc{false};
        bool    accTxReq{false};
        bool    rcaTxReq{false};
        bool    accTx{false};
        bool    tx1{false};
        bool    tx2{false};
        bool    tx3{false};
    };
    QMap<int, TxBandInfo> m_txBandSettings;
    QHash<QString, QString> m_panTransmitInhibitReasons;
    QHash<QString, int> m_panTransmitInhibitedTxSlices;
    int  m_tuneInhibitBandId{-1};  // band ID whose TX outputs were inhibited during tune
    bool m_tuneInhibitActive{false};

    int bandIdForFrequency(double freqMhz) const;  // map TX freq → band ID
    void applyTuneInhibit();    // suppress selected TX outputs before tune
    void restoreTuneInhibit();  // re-enable TX outputs after tune
    QString transmitInhibitMessageForSlice(const SliceModel* slice) const;
    QString transmitInhibitMessageForTxSlice() const;
    void enforceTransmitInhibitForPan(const QString& panId);
    void enforceTransmitInhibitForSlice(SliceModel* slice);
    void selectSoleValidTxAntennaIfNeeded(SliceModel* slice, bool txAntennaStatusReceived);
    bool transmitStartBlockedByInhibit(const QString& key);
    void noteLocalTxSliceEnableIntent(int sliceId);
    void sendSliceCommand(SliceModel* slice, const QString& cmd);
    QString localPttInterlockMessage(TransmitModel::PttSource source) const;
    QString txFilterFrequencyLimitMessage(int lowHz, int highHz) const;
    QString radioInterlockNotificationMessage(const QMap<QString, QString>& kvs) const;
    void evaluateTxFilterAudioLoss(float scFilt1, float scFilt2);
    void armInterlockNotification(TransmitModel::PttSource source = TransmitModel::PttSource::Mox);
    // Recompute the operator-transmit predicate and emit operatorTransmitChanged
    // on a rising/falling edge. Cheap; safe to call from every TX-state path.
    void updateOperatorTransmit();
    // Raw-TX edge for backends with no interlock status plane (HL2). No-op on
    // Flex, where the edge is decoded from `interlock` status instead.
    void publishBackendTransmitEdge(bool tx);
    // Key-on guard for the MOX/TUNE seam paths, which do not run through
    // setTransmit() and therefore missed its canTransmit test. Returns true when
    // keying may proceed; on refusal it rolls back the optimistic transmit state
    // and notifies, so no raw-TX edge is ever published for a refused key.
    bool refuseKeyOnTransmitIncapableBackend();
    bool interlockNotificationArmed() const;
    void emitInterlockNotification(const QString& message,
                                   const QString& key,
                                   const QString& panId = QString());

public:
    const QMap<int, TxBandInfo>& txBandSettings() const { return m_txBandSettings; }

    struct XvtrInfo {
        int     index{0};
        int     order{-1};
        QString name;
        double  rfFreq{0.0};
        double  ifFreq{0.0};
        double  loError{0.0};
        double  rxGain{0.0};
        double  maxPower{10.0};
        bool    rxOnly{false};
        bool    isValid{false};
        bool    hasIsValid{false};
    };
    const QMap<int, XvtrInfo>& xvtrList() const { return m_xvtrList; }

private:
    QMap<int, XvtrInfo> m_xvtrList;

    // Backward-compat helpers for active panadapter (Phase 1)
    QString activeWfId() const {
        auto* p = activePanadapter();
        return p ? p->waterfallId() : QString();
    }
    void setActiveWfId(const QString& id) {
        if (auto* p = activePanadapter()) p->setWaterfallId(id);
    }
    bool activePanResized() const {
        auto* p = activePanadapter();
        return p ? p->isResized() : false;
    }
    void setActivePanResized(bool r) {
        if (auto* p = activePanadapter()) p->setResized(r);
    }
    bool activeWfConfigured() const {
        auto* p = activePanadapter();
        return p ? p->isWaterfallConfigured() : false;
    }
    void setActiveWfConfigured(bool c) {
        if (auto* p = activePanadapter()) p->setWaterfallConfigured(c);
    }

private:
    QList<SliceModel*> m_slices;
    QMap<int, QString> m_rawSliceModeLists;
    QMap<int, SliceModel*> m_staleSlices;  // previous session, kept alive for UI reuse
    quint64 m_sessionModelGeneration{0};
    // chassis_serial of the radio the staged session models came from.
    // Reclaim-by-ID is only valid against the same radio — slice indexes and
    // stream IDs collide near-certainly across different radios.
    QString m_staleSessionSerial;
    // #3977: OUR handle from the PREVIOUS session (captured at registration
    // into m_ownSessionHandle, consumed at stage time). Reclaim eviction must
    // only fire when the staged pan still records THIS handle — pan status
    // parsing (client_handle) can legitimately rewrite a pan's owner to
    // another live client before we disconnect (ownership transfer), and
    // evicting that client would kick a healthy session, not a zombie.
    quint32 m_staleSessionOwnHandle{0};
    quint32 m_ownSessionHandle{0};   // this session's handle, set at registration
    QMap<int, MemoryEntry> m_memories;
    // Backing store for m_memories when the radio has no memory slots of its
    // own. It is always constructed but only read/written on that path, so a
    // Flex session never touches the file.
    LocalMemoryBank m_localMemories;
    // Does the radio THIS SESSION belongs to own its memory slots? Latched on the
    // connect edge and held across an unexpected drop, because isConnected()
    // alone cannot tell "never connected" from "the link blipped and a reconnect
    // is armed" — and during a blip the Memory dialog stays fully usable, so an
    // Add would be answered by the host bank, reported as saved, then wiped by
    // syncMemoryStoreForSession() on reconnect, leaving a phantom channel in
    // memories.json that reappears on every later disconnect. Cleared only when
    // the session really ends. See usesLocalMemoryBank().
    bool        m_sessionRadioOwnsMemories{false};
    QStringList m_globalProfiles;
    QString     m_activeGlobalProfile;
    bool        m_profileDatabaseImporting{false};
    bool        m_profileDatabaseExporting{false};
    RadioStatusOwnership::RemoteAudioRxTracking m_rxAudio;
    struct DaxStreamDebugState {
        QString type;
        quint32 clientHandle{0};
        int daxChannel{0};
        int daxIqChannel{0};
        int sliceId{-1};
        int daxIqRate{0};
        QString panId;
        QString ip;
        bool active{false};
        bool tx{false};
        bool activeKnown{false};
        bool txKnown{false};
    };
    QMap<quint32, DaxStreamDebugState> m_daxStreamDebug;
    quint32     m_daxTxStreamId{0};
    bool        m_daxTxActive{false};
    bool        m_wsprTxOwnershipRequested{false};
    bool        m_wsprTxYieldAfterUse{false};
    bool        m_wsprTxReleaseWhenReady{false};
    bool        m_wsprTxPreviousDax{false};   // `transmit dax` before the beacon armed
    bool        m_wsprTxRestoreDax{false};    // beacon changed it and owes a restore
    // The beacon armed against a host-modulating backend, so it borrowed no DAX
    // stream and no `transmit dax`. Latched by prepareWsprTransmit() and the
    // only thing releaseWsprTransmit() has to undo on that path.
    bool        m_wsprTxHostModulated{false};
    quint32     m_daxTxClientHandle{0};  // Tracked for diagnostics only — not consulted in routing.
    bool        m_daxTxCreatePending{false};
    QSet<quint32> m_deadDaxRxSeen;
    QSet<quint32> m_externalDaxTxSeen;
    QSet<quint32> m_externalDaxRxSeen;
    // #1439 nudge one-shot (#4383): stream ids we have already re-asserted
    // `slice set dax=` for. Armed on nudge send, cleared only on a real
    // `stream remove` (unregisterDaxStream). Stops the radio's own transient
    // empty-slice= unbind echo from re-triggering the nudge → #4009 storm.
    QSet<quint32> m_nudgedDaxStreams;
    WanConnection* m_wanConn{nullptr};  // non-null when connected via SmartLink
    QString  m_wanPublicIp;
    quint16  m_wanUdpPort{4991};
    QSet<int>          m_ownedSliceIds;   // slice IDs that belong to our client
    QHash<int, quint32> m_foreignSliceOwners;  // slot id → owning client handle
    QSet<int>          m_automationSliceFixtures; // disconnected bridge fixtures
    bool               m_automationSliceFixtureBaselineActive{false};
    QString            m_automationSliceFixtureBaselineModel;
    int                m_automationSliceFixtureBaselineMaxSlices{4};
    bool               m_txOwnedByUs{true};  // true when tx_client_handle matches our handle
    bool               m_fullDuplex{false};
    int                m_rttyMarkDefault{2125};
    quint32            m_txClientHandle{0};  // handle of the client that owns TX
    qint64             m_profileLoadRadioStateWriteHoldUntilMs{0};

    // #4142 — user pan writes (band/center/bandwidth) deferred while the
    // profile-load hold is armed. Field-wise coalescing lives in the queue
    // type; see ProfileLoadPanWriteQueue.h for the semantics and
    // profile_load_command_test for the contract.
    ProfileLoadPanWriteQueue m_pendingProfileLoadPanWrites;
    // Single owner of the replay schedule (stop/start re-arm — never a fan of
    // one-shot timers, which would fire a stale flush for every extension).
    QTimer m_profileLoadPanWriteFlushTimer;

    QMap<quint32, ClientInfo> m_clientInfoMap; // handle → full client info
    std::function<void()> m_multiFlexContinuation; // saved continuation during conflict pause
    QMap<quint32, QString> m_clientStations;   // handle → station name (legacy, kept in sync)
    QList<quint32> m_pendingClientDisconnects; // handles chosen before connecting
    QSet<quint32> m_announcedClientConnections; // client login notices shown this session
    QSet<quint32> m_startupClientConnections; // clients present before our connect status replay
    QElapsedTimer m_clientConnectionNoticeTimer;
    static constexpr qint64 CLIENT_CONNECTION_STARTUP_SUPPRESS_MS = 5000;
    void clearAutomationSliceFixtures();
    void restoreAutomationSliceFixtureBaseline();

    SleepInhibitor m_sleepInhibitor;     // prevents OS idle sleep while connected
    RadioInfo m_lastInfo;               // stored for auto-reconnect
    bool      m_intentionalDisconnect{false};
    // See isConnectAttemptInFlight(). Set by the connect entry points, cleared
    // by every way an attempt can end.
    bool      m_connectAttemptActive{false};
    bool      m_forcedDisconnectInProgress{false};
    GuiClientRegistrationState m_guiClientRegistrationState;
    // Suppress connection-error toasts between rebootRadio() and the next
    // successful reconnect — multiple `Connection refused` retries fire
    // while the radio is still booting and would otherwise spam the UI.
    bool      m_rebootInProgress{false};
    QTimer    m_reconnectTimer;
    // RFC #4603 PR 3: debounces operatingStateChanged into one store write.
    // The companion max-wait timer guarantees a sustained burst (continuous
    // tuning) still stores at least every interval; the disconnect path
    // flushes whatever is pending (PR #4619 review — the state most likely
    // to be lost was the operator's last edit before disconnecting).
    QTimer    m_operatingStateSaveTimer;
    QTimer    m_operatingStateMaxWaitTimer;

    // ── Network quality monitor ──
    // Take a transport snapshot from the backend seam: feed the loss window,
    // rescore the link, and beat the heartbeat if traffic actually arrived.
    void applyBackendLinkStats(const IRadioBackend::LinkStats& stats);
    // Everything a new scoring session must forget. Shared by the two paths that
    // start one — the Flex ping timer and the first backend LinkStats snapshot —
    // because when it was open-coded in both they drifted.
    void resetNetworkQualitySession();
    void startNetworkMonitor();
    void stopNetworkMonitor();
    void evaluateNetworkQuality();
    void resetNetworkHealthSamples();
    void recordNetworkHealthSample(int currentErrors, int currentPackets);
    enum class NetState { Off, Excellent, VeryGood, Good, Fair, Poor };
    void applyAdaptiveFrameRate(NetState newState, NetState oldState);
    static int fpsCapForState(NetState s);  // single source of truth; see obs. 1 in PR review
    // adaptiveWfRateForCap() moved to the public network-diagnostics section (#4261).
    void sendAdaptiveCapToPan(const QString& panId, int fpsCap);
    double networkQualityTargetScore(int pingMs) const;
    NetState networkStateForScore(double score, NetState currentState) const;
    bool usesRemoteNetworkThresholds() const;

    static constexpr int LAN_PING_FAIR_MS = 50;
    static constexpr int LAN_PING_POOR_MS = 100;
    static constexpr int REMOTE_PING_FAIR_MS = 180;
    static constexpr int REMOTE_PING_POOR_MS = 350;
    static constexpr int NETWORK_LOSS_WINDOW_SAMPLES = 10;
    static constexpr int NETWORK_MIN_LOSS_WINDOW_PACKETS = 100;

    QTimer        m_pingTimer;           // 1-second interval
    QMetaObject::Connection m_networkPingConnection;
    // RTT now measured by RadioConnection::pingRttMeasured at socket-read time
    int           m_lastPingRtt{0};      // ms
    int           m_maxPingRtt{0};       // max RTT seen this session
    int           m_lastErrorCount{0};   // snapshot for delta
    int           m_lastPacketCount{0};  // snapshot for delta
    int           m_lossSamplePackets[NETWORK_LOSS_WINDOW_SAMPLES]{};
    int           m_lossSampleErrors[NETWORK_LOSS_WINDOW_SAMPLES]{};
    int           m_lossSampleCursor{0};
    int           m_lossSampleCount{0};
    int           m_packetLossWindowPackets{0};
    int           m_packetLossWindowErrors{0};
    double        m_networkQualityScore{100.0};
    NetState      m_netState{NetState::Off};
    int           m_pingMissCount{0};          // consecutive unanswered pings
    bool          m_pingDisconnectTriggered{false};
    qint64        m_lastMultiFlexClientConnectMs{0};
    qint64        m_multiFlexPingGraceUntilMs{0};
    // Normal: disconnect after 5 unanswered pings (~5 s).
    // Poor: allow 15 (~15 s) — adaptive throttle has already cut UDP load, so
    // brief TCP stalls are more likely to be transient congestion than a dead link.
    static constexpr int PING_MISS_DISCONNECT      = 5;
    static constexpr int PING_MISS_DISCONNECT_POOR = 15;
    static constexpr qint64 MULTIFLEX_CLIENT_CONNECT_PING_GRACE_MS = 5000;
    // Minimum time at a throttled state before the throttle is allowed to lift.
    // Prevents Good<->VeryGood oscillation: reducing fps lowers UDP load which
    // improves the score, which would immediately lift the throttle and restart
    // the cycle. 5 s of hysteresis breaks the loop without delaying recovery
    // on a genuinely improving link.
    static constexpr qint64 THROTTLE_MIN_DWELL_MS = 5000;
    qint64 m_lastThrottleEngageMs{0};   // QDateTime::currentMSecsSinceEpoch() at last engage
    bool   m_pendingThrottleLift{false}; // lift deferred by min-dwell; fired in evaluateNetworkQuality()

    // ── Backend-reported transport (non-Flex families) ──
    //
    // The Flex path measures the network off m_connection (TCP ping RTT) and
    // m_panStream (VITA-49 counters). A family that owns neither has both null,
    // and every getter below used to answer a flat zero for it — so the whole
    // network readout rendered "connected, 0 kbps, 0 packets" on a link that was
    // working. This is the same data arriving from the other side of the seam.
    //
    // Populated ONLY from IRadioBackend::linkStatsUpdated, and only a backend
    // that overrides linkStats() ever sends one. `reported` staying false is
    // what keeps the Flex path bit-for-bit unchanged: every fallback below is
    // gated on it, and the Flex sources are always consulted first.
    IRadioBackend::LinkStats m_linkStats;

    // What this backend's transport can MEASURE, as opposed to what it measured
    // this second. m_linkStats above holds live counters and stopNetworkMonitor()
    // drops it, because those numbers belong to the session that just ended —
    // but "does this wire have a round trip to time" is a fact about the WIRE and
    // stays true while it is down. Without the distinction a disconnected HL2
    // falls back to the Flex branch, and lastPingRtt()'s 0 renders as "< 1 ms":
    // the exact claim HERMES.md § 21.3 exists to forbid, one state transition
    // later. Latched sticky-once-true so a window that closes with no samples in
    // it cannot flip a readout mid-session, and reset in teardownBackend(),
    // because only a new backend can change the answer.
    struct BackendLinkShape {
        bool reports   = false;   // a backend has published a LinkStats at all
        bool hasRtt    = false;   // ...and that transport times a round trip
        bool hasTiming = false;   // ...and it measures delivery spacing
    };
    BackendLinkShape m_backendLinkShape;

    // True when the network figures come from the backend seam rather than from
    // a Flex PanadapterStream. The single predicate every fallback branches on.
    bool usesBackendLinkStats() const { return !m_panStream && m_linkStats.reported; }

    // Network diagnostics — byte counters for rate calculation

public:
    // Network diagnostics getters
    int     lastPingRtt()      const { return m_lastPingRtt; }
    int     maxPingRtt()       const { return m_maxPingRtt; }

    // Whether the current transport has a round trip to time at all.
    //
    // Not every link does. Protocol 1 is a one-way stream — EP2 out on a wall
    // clock, EP6 back free-running, and nothing in either direction answering
    // anything in the other — so there is no RTT to report and no honest way to
    // fake one. Every RTT readout must ask this first: lastPingRtt() answers 0
    // when nothing measured it, and 0 renders as "< 1 ms", which is a link
    // quality claim the app never made a measurement to support.
    // Answers for the WIRE, not for the moment — which is why the last branch
    // consults the latched shape rather than the live snapshot. While connected
    // the snapshot is authoritative; between sessions m_linkStats is cleared and
    // the honest answer is still "this transport never had an RTT", not the Flex
    // default.
    bool    hasLinkRtt() const {
        if (m_panStream)
            return true;                        // Flex VITA-49 stack: TCP ping RTT
        if (m_linkStats.reported)
            return m_linkStats.rttMs >= 0;      // live snapshot from the seam
        return !m_backendLinkShape.reports || m_backendLinkShape.hasRtt;
    }
    // Whether the transport has produced a delivery-timing window yet. Same trap
    // as the RTT: the seam's -1 means "not measured", the getters clamp it to 0
    // for the charts, and formatNetworkMs(0) renders "< 1 ms". A backend is
    // `reported` from its first tick on purpose (so the readouts leave the Flex
    // branch immediately), but its first gap window has not closed yet — so for
    // about a second the pane would advertise sub-millisecond delivery it has
    // not measured.
    bool    hasLinkTiming() const {
        if (m_panStream)
            return true;
        if (m_linkStats.reported)
            return m_linkStats.gapMs >= 0;
        return !m_backendLinkShape.reports || m_backendLinkShape.hasTiming;
    }
    // Whether per-stream (Audio / FFT / Waterfall / Meter / DAX) sequence
    // counters exist. They are a property of the Flex VITA-49 multiplex, where
    // each category is a separately-sequenced stream on one socket. A transport
    // that carries everything in one stream has nothing to break down, and
    // rendering five rows of "0 / 0 packets" for it invents a distinction the
    // wire does not have.
    bool    hasStreamCategoryStats() const { return m_panStream != nullptr; }
    bool    pendingThrottleLift() const { return m_pendingThrottleLift; }
    int     currentAdaptiveFpsCap() const;  // 0 = throttle inactive
    // The 1..100 waterfall RATE the adaptive throttle caps to for a given fps
    // cap — NOT milliseconds, see core/WaterfallRate.h for why the distinction
    // is load-bearing. Public so MainWindow can recognize (and suppress) that
    // cap's own status echo while distinguishing it from a real radio/profile
    // update (#4261).
    int     adaptiveWfRateForCap(int fpsCap) const;
    QString networkQuality()   const;
    int     packetLossWindowSeconds() const { return NETWORK_LOSS_WINDOW_SAMPLES; }
    int     packetLossWindowDrops() const { return m_packetLossWindowErrors; }
    int     packetLossWindowPackets() const { return m_packetLossWindowPackets; }
    double  packetLossPercent() const;
    int     audioPacketGapMs() const;
    int     audioPacketGapMaxMs() const;
    int     audioPacketJitterMs() const;
    int     packetDropCount()  const;
    int     packetTotalCount() const;
    qint64  rxBytes()          const;
    qint64  txBytes()          const;
    QString targetRadioIp()    const;
    QString selectedSourceMode() const;
    QString selectedSourcePath() const;
    QString localTcpEndpoint() const;
    QString localUdpEndpoint() const;
    bool    firstUdpPacketSeen() const;

    // Per-category stream stats (Audio, FFT, Waterfall, Meter, DAX)
    PanadapterStream::CategoryStats categoryStats(PanadapterStream::StreamCategory cat) const;
    QVector<PanadapterStream::AudioStreamDiagnostics> audioStreamDiagnostics() const;
    void resetAudioStreamDiagnostics();
};

} // namespace AetherSDR
