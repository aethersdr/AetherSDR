#pragma once

// Pure helper functions extracted from MainWindow.cpp (#3351 Phase 0).
//
// Everything here is a stateless formatter or value transform with no
// MainWindow dependency: tooltip builders, spot-ID math, client-list
// parsing, small pixmap painters. They were file-scope statics inside
// MainWindow.cpp; they live here so the monolith decomposition can move
// method bodies into per-subsystem translation units without dragging a
// 400-line block of file-locals along to every new TU.
//
// Rule for additions: if a function needs MainWindow state (members,
// mutable file-scope statics like the shortcut-lease flags), it does NOT
// belong here — put it on the class, or leave it file-scope next to the
// state it reads.

#include <QKeySequence>
#include <QList>
#include <QPixmap>
#include <QString>
#include <QMap>
#include <QStringList>
#include <QVector>

#include "models/RadioModel.h"
#include "models/XvtrPolicy.h"

#include "ClientDisconnectDialog.h"  // QList<ClientDisconnectDialog::Client> returns

class QKeyEvent;

namespace AetherSDR {

class RadioModel;
class SpectrumWidget;
class TnfModel;
struct MemoryEntry;
struct RadioInfo;
struct WanRadioInfo;

// ─── Platform checks ─────────────────────────────────────────────────────────

// True when the macOS DAX HAL driver bundle is installed (always true on
// non-mac platforms; the caller is itself #ifdef Q_OS_MAC-gated).
bool macDaxDriverInstalled();

// ─── Network diagnostics tooltip ─────────────────────────────────────────────

QString formatNetworkMs(int ms);
QString formatNetworkSeqErrors(int errors, int packets);
QString buildNetworkTooltip(const RadioModel& model);

// ─── TNF tooltip ─────────────────────────────────────────────────────────────

long long tnfFrequencyHz(double freqMhz);
QString formatTnfFrequency(double freqMhz);
QString formatTnfDepth(int depthDb);
QString buildTnfTooltip(const TnfModel& tnfModel);

// ─── Memory / passive spot ID math ───────────────────────────────────────────
//
// Memory spots and passive local spots are folded into the spot model with
// negative indices offset by these bases so they can't collide with radio
// spot indices.

inline constexpr int kMemorySpotIdBase = 1000000;
inline constexpr int kPassiveSpotIdBase = 2000000;

int memorySpotId(int memoryIndex);
int memoryIndexFromSpotId(int spotIndex);
bool isPassiveLocalSpotId(int spotIndex);
QString memorySpotLabel(const MemoryEntry& memory);
QString memorySpotComment(const MemoryEntry& memory);

// ─── CW momentary action registry IDs ────────────────────────────────────────
//
// Shared between the keyboard-shortcut registry (MainWindow.cpp), the MIDI
// param registry, and the HID action dispatch (MainWindow_Controllers.cpp).

inline constexpr const char* kCwStraightKeyActionId = "cwkey";
inline constexpr const char* kCwLeftPaddleActionId = "cwdit";
inline constexpr const char* kCwRightPaddleActionId = "cwdah";
inline constexpr const char* kCwStraightKeyActionName = "Trigger straight key";
inline constexpr const char* kCwLeftPaddleActionName = "Trigger CW Left Paddle";
inline constexpr const char* kCwRightPaddleActionName = "Trigger CW Right Paddle";

// ─── AetherSweep SWR-sweep tuning constants ─────────────────────────────────
//
// Shared between the constructor's poll-timer setup (MainWindow.cpp) and the
// sweep state machine (MainWindow_SwrSweep.cpp).

inline constexpr double kSwrSweepStepMhz = 0.020;
inline constexpr double kSwrSweepEdgeGuardMhz = 0.005;
inline constexpr double kSwrSweepPanPaddingMhz = 0.020;
inline constexpr int kSwrSweepPollMs = 50;
inline constexpr int kSwrSweepInitialSettleMs = 350;
inline constexpr int kSwrSweepStepSettleMs = 160;
inline constexpr int kSwrSweepMaxSettleMs = 900;
inline constexpr int kSwrSweepTgxlBypassTimeoutMs = 3500;
inline constexpr int kSwrSweepTgxlRelaySettleMs = 250;
inline constexpr int kSwrSweepTuneStopWaitMs = 350;
inline constexpr int kSwrSweepTuneStopTimeoutMs = 1800;
inline constexpr int kSwrSweepTgxlRestoreTimeoutMs = 3500;
inline constexpr int kSwrSweepMaxPoints = 260;

// ─── Pan-layout restore window ───────────────────────────────────────────────
//
// Shared between the connect-time restore logic (MainWindow.cpp) and the
// multi-pan lifecycle wiring (MainWindow_Session.cpp).

inline constexpr qint64 kPanLayoutRestoreWaitingForFirstPan = -1;
inline constexpr int kPanLayoutRestoreWindowMs = 30000;

// ─── Pan layout ──────────────────────────────────────────────────────────────

// Pan count for a saved layout id (e.g. "2x2" → 4); 1 for unknown ids.
int panCountForLayoutId(const QString& layoutId);

// ─── XVTR policy summaries (diagnostics) ────────────────────────────────────
//
// Shared by the slice-tuning logs in MainWindow.cpp and the per-pan wiring
// in MainWindow_Wiring.cpp.

QVector<XvtrPolicy::Transverter> xvtrPolicyBandsFrom(
    const QMap<int, RadioModel::XvtrInfo>& xvtrs);
QString xvtrListSummary(const QVector<XvtrPolicy::Transverter>& xvtrs);
QString xvtrForBandSummary(const QString& bandName,
                           const QVector<XvtrPolicy::Transverter>& xvtrs);

// ─── Pan pixel dimensions ────────────────────────────────────────────────────
//
// Effective pan stream dimensions for a SpectrumWidget, with safe defaults
// while the widget has no real geometry yet. Shared by the connect-time
// sizing in MainWindow.cpp and the per-pan wiring in MainWindow_Wiring.cpp.

int panXpixelsFor(const SpectrumWidget* spectrum);
int panYpixelsFor(const SpectrumWidget* spectrum);
bool panPixelDimensionsReady(const SpectrumWidget* spectrum);

// ─── Misc UI ─────────────────────────────────────────────────────────────────

QPixmap buildBandStackIndicatorPixmap(bool active);
QKeySequence shortcutSequenceFromKeyEvent(const QKeyEvent* ev);

// ─── Client connection parsing (discovery / multiFLEX) ──────────────────────

QStringList splitClientField(const QString& raw);
quint32 parseClientHandle(QString text);
QList<ClientDisconnectDialog::Client> buildDisconnectClients(
    const QStringList& handles,
    const QStringList& programs,
    const QStringList& stations);
QList<ClientDisconnectDialog::Client> buildDisconnectClients(const RadioInfo& info);
QList<ClientDisconnectDialog::Client> buildDisconnectClients(const WanRadioInfo& info);
QString cleanClientDisplayText(QString value);
QString clientConnectionStatusMessage(quint32 handle,
                                      const QString& source,
                                      const QString& station,
                                      const QString& program);

} // namespace AetherSDR
