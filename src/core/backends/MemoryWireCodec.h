#pragma once

#include "core/backends/MemoryDelta.h"

#include <QMap>
#include <QString>

namespace AetherSDR {

// One decoder for a memory-slot kv-set, shared by every producer of one.
//
// Two callers reach this: FlexBackend::decodeMemoryStatus, decoding what the
// radio reported, and the local memory bank, decoding the `memory set` a client
// just issued against a radio that has no memory storage of its own. They MUST
// agree — a slot written locally and a slot read back from a Flex have to land
// in MemoryEntry identically or the same CSV export would depend on which radio
// happened to be connected. Keeping the decode in one place is what makes that
// structural rather than a thing two copies are trusted to preserve.
//
// Contract (unchanged from the Flex decoder this was lifted from): present-only
// — absent keys leave their optional disengaged so the model keeps the slot's
// prior value; numerics are ok-guarded, so a malformed *present* value is
// dropped rather than applied as 0. Text rides raw: the protocol space-encoding
// (0x7f→' ') and the NUL/control-byte sanitisation are a models/ concern applied
// in RadioModel::applyMemoryChanges, so this stays free of any models/ include.
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
