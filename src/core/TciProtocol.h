#pragma once
#ifdef HAVE_WEBSOCKETS

#include <QString>
#include <QStringList>
#include <optional>

namespace AetherSDR {

class RadioModel;
class SliceModel;
class TciRoutingState;
class TciTrxMap;

// TCI protocol handler — text command parser and response generator.
// No I/O — receives a command string, returns the response.
// Reference: https://github.com/ExpertSDR3/TCI (protocol v2.0)
class TciProtocol {
public:
    struct VfoRequest
    {
        int trx { -1 };
        int channel { -1 };
        long long frequencyHz { 0 };
    };

    struct SplitRequest
    {
        int trx { -1 };
        bool enabled { false };
    };

    struct TrxRequest
    {
        int trx { -1 };
        bool transmitting { false };
        QString source;
    };

    explicit TciProtocol(RadioModel* model, TciRoutingState* routingState = nullptr,
                         const TciTrxMap* trxMap = nullptr);

    // Process one TCI command (without trailing semicolon).
    // Returns response string (with trailing semicolon) or empty if no response.
    QString handleCommand(const QString& cmd);

    // Generate the init burst sent to newly connected clients.
    QString generateInitBurst();

    // After handleCommand(), if the command changed radio state,
    // this returns a notification to broadcast to other clients.
    // Returns empty if no broadcast needed.
    QString pendingNotification();
    std::optional<VfoRequest> takeVfoRequest();
    std::optional<SplitRequest> takeSplitRequest();
    std::optional<TrxRequest> takeTrxRequest();

    // After handleCommand(), if the command was a master-volume SET, this
    // returns the requested level (0-100). -1 means no master-volume change
    // was requested. The TciServer reads this to forward the request to
    // MainWindow (which owns the AudioEngine + RadioModel-lineout path).
    // Per TCI v2.0 spec, `volume:N;` is the global master volume command.
    int pendingMasterVolume() const { return m_pendingMasterVolume; }

    // TCI VOLUME wire scale is dB (-60..0, -60 = silence) per spec;
    // AetherSDR's internal master volume is 0-100 percent amplitude.
    // Conversions used by cmdVolume, the init burst, and TciServer's
    // master-volume broadcast.
    static int volumeDbFromPercent(int pct);
    static int volumePercentFromDb(double db);

    // After handleCommand(), if the command was a `tx_gain:N;` SET, this
    // returns the requested TCI TX gain (0-100); -1 means no change. TciServer
    // reads it and applies it via setTxGain() — TciProtocol can't reach
    // TciServer directly (same pattern as pendingMasterVolume).
    int pendingTxGain() const { return m_pendingTxGain; }

    // Which TCI TRX currently holds GUI focus (#4160). TciServer owns this
    // value and pushes it in — it cannot be derived reliably by scanning
    // slices for isActive(): SliceModel::setActive() sets the new slice's
    // flag optimistically (SliceModel.cpp, #3854 review) while the outgoing
    // slice keeps its flag until the radio echoes active=0, so for one round
    // trip TWO slices report active and a scan returns whichever comes first
    // in slice order. -1 = not yet known, in which case the scan is used as
    // the startup fallback (before any focus change has been observed).
    // The letter is sanitized here, not just at the TciServer call site: this
    // is the boundary the value crosses on its way to the wire, and a raw
    // radio-supplied ',' or ';' would corrupt TCI framing for every client on
    // the socket. Enforcing it in the setter keeps the invariant independent of
    // any caller remembering (Principle VII). Sanitizing is idempotent, so the
    // server sanitizing first costs nothing.
    // The IQ sample rate is shared server state (TciServer owns it across all
    // clients and all four channels), but it is announced in this client's init
    // burst — so the server seeds it here rather than the burst hardcoding a
    // default a later SET has already moved off.
    void setIqSampleRate(int rate) { m_iqSampleRate = rate; }

    void setActiveSlice(int trx, const QString& letter)
    {
        m_activeTrx = trx;
        m_activeLetter = sanitizeSliceLetter(letter);
    }
    int activeTrx() const { return m_activeTrx; }

private:
    // Resolves the focused slice into its trx and display letter, falling
    // back to a slice scan while m_activeTrx is -1. Returns false when there
    // is no model or no slice reports active.
    bool resolveActiveSlice(int& trx, QString& letter) const;

    // Command handlers — return response string or empty
    QString cmdVfo(const QStringList& args, bool isSet);
    QString cmdModulation(const QStringList& args, bool isSet);
    QString cmdTrx(const QStringList& args, bool isSet);
    QString cmdTune(const QStringList& args, bool isSet);
    QString cmdDrive(const QStringList& args, bool isSet);
    QString cmdTuneDrive(const QStringList& args, bool isSet);
    QString cmdMicLevel(const QStringList& args, bool isSet);
    QString cmdTxGain(const QStringList& args, bool isSet);
    QString cmdRitEnable(const QStringList& args, bool isSet);
    QString cmdXitEnable(const QStringList& args, bool isSet);
    QString cmdRitOffset(const QStringList& args, bool isSet);
    QString cmdXitOffset(const QStringList& args, bool isSet);
    QString cmdSplitEnable(const QStringList& args, bool isSet);
    QString cmdRxFilterBand(const QStringList& args, bool isSet);
    QString cmdCwMacrosSpeed(const QStringList& args, bool isSet);
    QString cmdCwMsg(const QStringList& args);
    QString cmdLock(const QStringList& args, bool isSet);
    QString cmdSqlEnable(const QStringList& args, bool isSet);
    QString cmdSqlLevel(const QStringList& args, bool isSet);
    QString cmdVolume(const QStringList& args, bool isSet);
    QString cmdMute(const QStringList& args, bool isSet);
    QString cmdAgcMode(const QStringList& args, bool isSet);
    QString cmdAgcGain(const QStringList& args, bool isSet);
    QString cmdRxNbEnable(const QStringList& args, bool isSet);
    QString cmdRxNrEnable(const QStringList& args, bool isSet);
    QString cmdRxAnfEnable(const QStringList& args, bool isSet);
    QString cmdRxApfEnable(const QStringList& args, bool isSet);
    // AetherSDR extensions (DVK record/play)
    QString cmdRxRecord(const QStringList& args, bool isSet);
    QString cmdRxPlay(const QStringList& args, bool isSet);
    QString cmdActiveSlice(const QStringList& args);
    QString cmdSpot(const QStringList& args);
    QString cmdSpotDelete(const QStringList& args);
    QString cmdSpotClear();
    QString cmdCwMacros(const QStringList& args);
    QString cmdCwMacrosStop();
    QString cmdStart();
    QString cmdStop();
    QString cmdTxEnable(const QStringList& args);
    QString cmdKeyer(const QStringList& args);
    QString cmdCwKeyerSpeed(const QStringList& args, bool isSet);
    QString cmdCwMacrosDelay(const QStringList& args, bool isSet);
    QString cmdCwTerminal(const QStringList& args, bool isSet);
    QString cmdDds(const QStringList& args, bool isSet);
    QString cmdIf(const QStringList& args, bool isSet);
    QString cmdRxChannelEnable(const QStringList& args, bool isSet);
    QString cmdRxVolume(const QStringList& args, bool isSet);
    QString cmdRxMute(const QStringList& args, bool isSet);
    QString cmdRxBalance(const QStringList& args, bool isSet);
    QString cmdMonEnable(const QStringList& args, bool isSet);
    QString cmdMonVolume(const QStringList& args, bool isSet);
    QString cmdRxNbParam(const QStringList& args, bool isSet);
    QString cmdRxBinEnable(const QStringList& args, bool isSet);
    QString cmdRxAncEnable(const QStringList& args, bool isSet);
    QString cmdRxDseEnable(const QStringList& args, bool isSet);
    QString cmdRxNfEnable(const QStringList& args, bool isSet);
    QString cmdDiglOffset(const QStringList& args, bool isSet);
    QString cmdDiguOffset(const QStringList& args, bool isSet);
    QString cmdSetInFocus();
    QString cmdTxFrequency();

    // Helpers
    SliceModel* sliceForTrx(int trx) const;
    SliceModel* sliceForVfo(int trx, int channel) const;
    int txTrx() const;

public:
    // Mode conversion (public for TciServer broadcast use)
    static QString smartsdrToTci(const QString& mode);
    // `ok` reports whether `mode` was a recognised TCI modulation name. The
    // return value falls back to "USB" when it isn't, so a caller that must
    // reject an unrecognised name rather than silently substitute USB has to
    // check `ok` — which is why it is not defaulted (#4523).
    static QString tciToSmartSDR(const QString& mode, bool* ok);

    // Slice display letter for `active_slice` (#4160), public for the same
    // reason. `index_letter` is radio-supplied, and a stray ',' or ';' in it
    // would corrupt the TCI framing for every client, so it is reduced to the
    // short alphanumeric label it is meant to be (Principle VII).
    static QString sanitizeSliceLetter(const QString& letter);

    // Map a slice to its contiguous TCI TRX index (0..N-1) within the
    // owned-slice list.  Falls back to the raw Flex sliceId() if the
    // slice is not in the model's list.
    static int tciTrxForSlice(RadioModel* model, const SliceModel* slice);

    // The single scan for "which trx is the TX slice", returning -1 when none
    // is marked. Both TX-trx resolvers build on this so the scan lives in one
    // place; they differ ONLY in how they map the -1 sentinel (txTrx() below
    // returns 0 for the request/response wire; TciServer's async broadcast
    // resolves -1 against a cached last-known trx, #4161). Keeping the scan
    // shared means a future change to slice iteration can't drift one path.
    static int txSliceTrxOrNone(RadioModel* model);

    // Resolve a contiguous TCI receiver index, then the legacy raw Flex slice
    // id, then the first slice for compatibility. Shared by parser and server
    // command paths so GET and SET never target different receivers.
    static SliceModel* resolveSliceForTrx(RadioModel* model, int trx);

    // Same resolution WITHOUT the first-slice fallback: an unresolvable trx
    // returns nullptr instead of silently addressing slices[0]. Use on any
    // path that keys the radio (#4547) — the compatibility fallback is a
    // reasonable guess for a read, but under PTT it transmits on a slice the
    // client never asked for, on that slice's band and antenna. The positional
    // and raw-id steps are unchanged, so a correctly-addressed legacy client
    // still resolves; only the guess is withdrawn.
    static SliceModel* resolveSliceForTrxStrict(RadioModel* model, int trx);

    static long long mhzToHz(double mhz);

    // IQ center (DDS) for a slice = its populated panadapter center in Hz.
    // Falls back to the slice frequency while the pan is absent or its center
    // still holds the model placeholder (#3910, #3913 review).
    static long long ddsCenterHz(RadioModel* model, const SliceModel* slice);

    // Extract the text from `cw_macros:<trx>,<text>` (#4997). A base-10
    // integer first argument is always a receiver address: an in-range value
    // is stripped and an out-of-range/stale value fails closed. A nonnumeric
    // first argument is retained for compatibility with index-less clients.
    // Public and pure so the boundary rule can be tested without a radio-side
    // CW keyer; trxCount is the same dynamic count advertised to TCI clients.
    [[nodiscard]] static QString cwMacrosTextFromArgs(const QStringList& args,
                                                      int trxCount);

private:

    RadioModel* m_model;
    TciRoutingState* m_routingState;
    // #4567: stable receiver numbering; nullptr falls back to the positional
    // statics (tests construct TciProtocol without a map).
    const TciTrxMap* m_trxMap{nullptr};
    QString m_pendingNotification;
    std::optional<VfoRequest> m_vfoRequest;
    std::optional<SplitRequest> m_splitRequest;
    std::optional<TrxRequest> m_trxRequest;
    int         m_pendingMasterVolume{-1};   // -1 = no change requested
    int         m_pendingTxGain{-1};         // -1 = no change requested
    int         m_activeTrx{-1};             // -1 = focus not yet known (#4160)
    int         m_iqSampleRate{48000};       // seeded by TciServer, see setIqSampleRate
    QString     m_activeLetter;              // focused slice's display letter (#4160)
    bool        m_started{false};  // client sent START
};

} // namespace AetherSDR

#endif // HAVE_WEBSOCKETS
