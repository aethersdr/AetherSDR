#pragma once

#include <QString>

namespace AetherSDR {

// Rules governing active/TX slice selection persistence and post-connect restore.
//
// FlexRadio firmware does NOT persist active or TX slice assignments across
// client reconnects: on connect, the radio enumerates existing slices in slice
// ID order and marks whatever slice was last created as active in the status
// stream.
//
// This policy coordinates capturing the operator's live selections and
// restoring them once enumeration quiesces on connect.
class SliceSelectionRestorePolicy {
public:
    // Scope key for the SliceSelection document.
    // Includes station name to isolate Multi-Flex client instances on the same radio.
    static QString radioIdForScope(bool isWan,
                                   const QString& discoverySerial,
                                   const QString& chassisSerial,
                                   const QString& stationName = QString())
    {
        QString baseId;
        if (isWan) {
            baseId = chassisSerial.trimmed();
        } else {
            baseId = !discoverySerial.trimmed().isEmpty()
                ? discoverySerial.trimmed()
                : chassisSerial.trimmed();
        }
        if (baseId.isEmpty()) {
            return QString();
        }
        if (!stationName.trimmed().isEmpty()) {
            return QStringLiteral("%1/%2").arg(baseId, stationName.trimmed());
        }
        return baseId;
    }

    // Guard: is the radioId non-empty so settings can be read or written?
    static bool scopeCanCarrySelection(const QString& radioId)
    {
        return !radioId.trimmed().isEmpty();
    }

    // May the TX slice choice be recorded?
    static bool shouldCaptureTxSlice(bool activeOrDisconnecting, int liveSliceCount,
                                     bool txOwnedByUs)
    {
        return activeOrDisconnecting && liveSliceCount > 0 && txOwnedByUs;
    }

    // May the restore re-assert a stored TX-slice assignment?
    static bool mayAssertTxSlice(bool txOwnedByUs) { return txOwnedByUs; }

    // May this active-slice change be recorded as the operator's preference?
    static bool shouldPersist(bool fromRadioEchoOrSystem,
                              bool bandRecallInFlight,
                              bool profileLoadHeld,
                              bool fromRestore)
    {
        return !fromRadioEchoOrSystem && !bandRecallInFlight && !profileLoadHeld
            && !fromRestore;
    }

    // Are slice letters ready for selection matching?
    //
    // On older firmware that never sends index_letter (SliceModel.h:34),
    // slicesWithLetterFromRadio stays 0 and letter() falls back to 'A' + sliceId,
    // so letters are immediately ready. On modern firmware, letters are ready
    // when all enumerated slices have received index_letter status.
    static bool lettersReadyForSelection(int enumeratedSliceCount,
                                          int slicesWithLetterFromRadio)
    {
        if (enumeratedSliceCount <= 0) return false;
        return slicesWithLetterFromRadio == 0 || slicesWithLetterFromRadio >= enumeratedSliceCount;
    }

    // ── The post-connect restore window ────────────────────────────────────

    void onConnected()
    {
        m_windowOpen = true;
        m_operatorSelected = false;
        ++m_generation;
    }

    void onDisconnected() { m_windowOpen = false; }

    int generation() const { return m_generation; }

    void noteOperatorSelection() { m_operatorSelected = true; }

    void onSettleTimeout(int generation)
    {
        if (generation == m_generation) {
            m_windowOpen = false;
        }
    }

    bool restoreAllowed() const { return m_windowOpen && !m_operatorSelected; }

    enum class RestoreAction {
        Skip,
        Defer,
        Apply,
    };

    static RestoreAction restoreAction(bool allowed,
                                       bool profileLoadHeld,
                                       bool scopeReady)
    {
        if (!allowed) {
            return RestoreAction::Skip;
        }
        if (profileLoadHeld || !scopeReady) {
            return RestoreAction::Defer;
        }
        return RestoreAction::Apply;
    }

    void consume() { m_windowOpen = false; }

private:
    bool m_windowOpen{false};
    bool m_operatorSelected{false};
    int m_generation{0};
};

}  // namespace AetherSDR
