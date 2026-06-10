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
#include <QStringList>

#include "ClientDisconnectDialog.h"  // QList<ClientDisconnectDialog::Client> returns

class QKeyEvent;

namespace AetherSDR {

class RadioModel;
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
