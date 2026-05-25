#pragma once

#include <QString>
#include <QMap>

namespace AetherSDR {

class RadioModel;
class SliceModel;

// Pure protocol handler for Hamlib rigctld emulation.
// No I/O — receives a text line, returns the response string.
// Shared by both the TCP server and the PTY virtual serial port.
class RigctlProtocol {
public:
    explicit RigctlProtocol(RadioModel* model);

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
    QString cmdGetFreq();
    QString cmdSetFreq(const QString& arg);
    QString cmdGetMode();
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
    SliceModel* findTxSlice();         // non-const: calls tryPromoteTxSlice()
    // If a split-enable arrived when only one slice existed, this promotes the
    // newly-created second slice to TX as soon as it appears in the model,
    // then applies any stashed split freq/mode from the burst that preceded it.
    void tryPromoteTxSlice();
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
    bool m_pendingTxSliceChange{false};  // set when setTxSlice(true) was queued for a non-rx slice
    // Slice we intend to be TX — set synchronously when we queue setTxSlice(true)
    // so findTxSlice() returns the right slice before the event loop fires.
    SliceModel* m_pendingTxSlice{nullptr};
    // Stashed split freq/mode from commands that arrived before the new slice
    // existed (single-slice path).  Applied in tryPromoteTxSlice().
    double  m_pendingSplitFreqMHz{0.0};
    QString m_pendingSplitMode;

    // Tracks the last split state this client reported, so set_split_vfo only
    // reclaims TX on an actual split→non-split *transition* — not on every
    // periodic poll. -1 = unknown (first call records state without acting),
    // 0 = split disabled, 1 = split enabled.  Without this, a logger that
    // polls `set_split_vfo 0` every few seconds on a CAT channel bound to a
    // non-TX slice keeps re-seizing TX away from the slice the user chose.
    int m_lastSplitEnable{-1};
};

} // namespace AetherSDR
