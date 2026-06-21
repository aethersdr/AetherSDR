#pragma once

#include <QString>
#include <QMap>
#include <QPointer>
#include <QElapsedTimer>
#include <functional>

namespace AetherSDR {

class RadioModel;
class SliceModel;

// Pure protocol handler for Hamlib rigctld emulation.
// No I/O — receives a text line, returns the response string.
// Shared by both the TCP server and the PTY virtual serial port.
class RigctlProtocol {
public:
    explicit RigctlProtocol(RadioModel* model);
    // On client disconnect, best-effort remove a TX slice we created on demand for
    // split (so it isn't orphaned when WSJT-X etc. drops without a clean teardown).
    ~RigctlProtocol();

    // Process one command line (may contain ';' or '|'-separated batch commands).
    // '|' separator enables extended responses joined by '|' (rigctld pipe mode).
    // Returns the complete response string to send back to the client.
    QString handleLine(const QString& line);

    // Which slice this protocol instance controls.
    void setSliceIndex(int idx) { m_sliceIndex = idx; }
    int  sliceIndex() const     { return m_sliceIndex; }

    // Extended response mode (prepend '+' to enable).
    void setExtendedMode(bool on) { m_extended = on; }
    bool extendedMode() const     { return m_extended; }

private:
    QString handleLineImpl(const QString& line);

    // Process a single command (short or long form).
    // Returns the response string.
    QString processCommand(const QString& cmd);

    // Individual command handlers
    QString cmdGetFreq(const QString& vfo = {});
    QString cmdSetFreq(const QString& args);
    QString cmdGetMode(const QString& vfo = {});
    QString cmdSetMode(const QString& args);
    QString cmdGetVfo();
    QString cmdSetVfo(const QString& arg);
    QString cmdGetPtt();
    QString cmdSetPtt(const QString& arg);
    QString cmdGetInfo();
    QString cmdGetRigInfo();
    QString cmdGetSplitVfo();
    QString cmdSetSplitVfo(const QString& args);
    QString cmdGetSplitFreq();
    QString cmdSetSplitFreq(const QString& args);
    QString cmdGetSplitMode();
    QString cmdSetSplitMode(const QString& args);
    QString cmdGetLevel(const QString& arg);
    QString cmdSetLevel(const QString& args);
    QString cmdGetFunc(const QString& arg);
    QString cmdSetFunc(const QString& args);
    QString cmdGetRit();
    QString cmdSetRit(const QString& arg);
    QString cmdGetXit();
    QString cmdSetXit(const QString& arg);
    QString cmdGetAnt();
    QString cmdSetAnt(const QString& arg);
    QString cmdGetTs();
    QString cmdSetTs(const QString& arg);
    QString cmdGetCtcssTone();
    QString cmdSetCtcssTone(const QString& arg);
    QString cmdGetDcd();
    QString cmdGetTrn();
    QString cmdSetTrn(const QString& arg);
    QString cmdVfoOp(const QString& arg);
    QString cmdGetVfoInfo(const QString& arg);
    QString cmdPower2mW(const QString& args);
    QString cmdMW2power(const QString& args);
    QString cmdDumpState();
    QString cmdSendMorse(const QString& text);  // b <text> / \send_morse
    QString cmdStopMorse();                     // \stop_morse
    QString cmdSetKeySpeed(const QString& arg); // \set_level KEYSPD <wpm>

    // Helpers
    SliceModel* currentSlice() const;
    SliceModel* sliceForVfo(const QString& vfo) const;
    // Single front door for VFO-prefixed commands. If parts[0] is a VFO name
    // (chk_vfo=1 mode prefixes VFO-sensitive commands with one), removes it and
    // resolves the slice it addresses; otherwise returns this port's bound slice.
    // Returns nullptr for a VFO that names no slice (VFOB with no split, VFOMEM)
    // — callers map nullptr to RPRT -8. Never silently redirects to the wrong slice.
    SliceModel* takeVfoPrefix(QStringList& parts) const;
    // Resolve the TX slice. With promote=true (default) it also runs the
    // deferred split-promotion state machine (tryPromoteTxSlice) — that is the
    // behaviour the split commands rely on. Read-only resolvers (sliceForVfo,
    // serving get_freq/get_mode VFOB) pass promote=false so a query never
    // mutates radio state as a side effect.
    SliceModel* findTxSlice(bool promote = true);
    // If a split-enable arrived when only one slice existed, this promotes the
    // newly-created second slice to TX as soon as it appears in the model,
    // then applies any stashed split freq/mode from the burst that preceded it.
    void tryPromoteTxSlice();
    // Best-effort, crash-safe removal of the on-demand TX slice we created
    // (m_createdTxSliceId): re-resolves by id at removal time and skips if it's
    // already gone. No-op when we promoted an existing (operator) slice.
    void removeCreatedTxSlice();
    // Ensure a distinct TX slice exists, enabling split on demand: promote an
    // existing non-RX slice, or create one (deferred promotion). No-op if a TX
    // slice already exists. Used by set_split_vfo's enable path and by
    // set_freq/set_mode VFOB — targetable_vfo lets clients address the TX VFO
    // directly without a preceding set_split_vfo (e.g. WSJT-X Rig split).
    // recordExistingAsEnabled: when split is ALREADY engaged on a distinct slice,
    // record m_lastSplitEnable=1 only for enable-intent callers (set_split_vfo 1,
    // set_freq/set_mode VFOB). Passive set_split_freq/set_split_mode pass false so
    // they don't claim an enable this client never made (would arm a spurious
    // 1→0 reclaim on the next polled set_split_vfo 0).
    void ensureSplitTxSlice(bool recordExistingAsEnabled = true);
    // Shared on-demand establish+resolve for set_split_freq / set_split_mode:
    // ensures a split TX slice (without claiming the enable), then either stashes
    // (create still in flight → RPRT 0), applies to the resolved TX slice (RPRT 0),
    // or returns RPRT -1 if none can be resolved. Keeps the create-on-demand
    // contract in one place so the two setters can't diverge.
    QString applySplitParam(const std::function<void()>& stashPending,
                            const std::function<void(SliceModel*)>& applyToTx);
    QString rprt(int code) const;

    // Mode conversion tables
    static QString smartsdrToHamlib(const QString& mode);
    static QString hamlibToSmartSDR(const QString& mode);
    static int     hamlibModeFlag(const QString& mode);

    RadioModel* m_model;
    int  m_sliceIndex{0};
    bool m_extended{false};
    // Set when a bare `b` / `\send_morse` arrives without inline text.
    // The next line is consumed verbatim as the morse text. Hamlib spec
    // allows this two-line form and Not1MM contest CW relies on it.
    bool m_pendingMorseLine{false};
    bool m_pendingSplitEnable{false};    // set when split enabled but no second slice existed yet
    QElapsedTimer m_pendingSplitTimer;   // age of the in-flight create; clears stale pending so a
                                         // NAK'd/lost create can't wedge split "pending" forever
    bool m_pendingTxSliceChange{false};  // set when setTxSlice(true) was queued for a non-rx slice
    // Slice we intend to be TX — set synchronously when we queue setTxSlice(true)
    // so findTxSlice() returns the right slice before the event loop fires.
    // QPointer (not raw): if the user closes this slice out-of-band, it auto-nulls
    // so findTxSlice() won't hand back a freed SliceModel and crash on deref
    // (rigctld runs on the GUI thread, same thread as SliceModel, so this is safe).
    QPointer<SliceModel> m_pendingTxSlice{nullptr};
    // Stashed split freq/mode from commands that arrived before the new slice
    // existed (single-slice path).  Applied in tryPromoteTxSlice().
    double  m_pendingSplitFreqMHz{0.0};
    QString m_pendingSplitMode;
    // Id of a TX slice WE created on demand for split (NOT a promoted operator
    // slice). Removed best-effort on split-disable and on disconnect so it isn't
    // orphaned. -1 = none. Set in tryPromoteTxSlice (only reached after our create).
    int     m_createdTxSliceId{-1};

    // Tracks the last split state this client reported, so set_split_vfo only
    // reclaims TX on an actual split→non-split *transition* — not on every
    // periodic poll. -1 = unknown (first call records state without acting),
    // 0 = split disabled, 1 = split enabled.  Without this, a logger that
    // polls `set_split_vfo 0` every few seconds on a CAT channel bound to a
    // non-TX slice keeps re-seizing TX away from the slice the user chose.
    int m_lastSplitEnable{-1};
};

} // namespace AetherSDR
