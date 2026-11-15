#pragma once

#include "core/backends/MemoryDelta.h"

#include <QMap>
#include <QString>

namespace AetherSDR {

// One decoder for a memory-slot kv-set, shared by FlexBackend::decodeMemoryStatus
// and the local memory bank (decoding a client's `memory set`), so a slot lands
// in MemoryEntry identically whichever radio is connected. Present-only: absent
// keys stay disengaged; numerics are ok-guarded, so a malformed present value is
// dropped, not applied as 0. Text rides raw: space-decoding (0x7f→' ') and
// control-byte sanitisation happen in RadioModel::applyMemoryChanges.
namespace MemoryWire {

// Decode a memory-slot kv-set into a typed delta. `removed` is set when the
// wire signalled the slot is gone — "in_use=0" or a bare "removed" key.
MemoryDelta decodeStatus(int index, const QMap<QString, QString>& kvs);

// Split a `memory set` argument tail ("group=Foo freq=14.074000 …") into its
// kv-set. Safe to split on spaces: free-text fields are space-encoded (0x7f) by
// encodeMemoryText() before they ever reach a command string, so a space in the
// tail is always a field separator. Tokens without '=' are ignored.
QMap<QString, QString> parseKvTail(const QString& tail);

}  // namespace MemoryWire

}  // namespace AetherSDR
