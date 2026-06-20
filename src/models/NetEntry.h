#pragma once

#include "models/MemoryEntry.h"

#include <QString>

namespace AetherSDR {

// A scheduled amateur-radio "net": a recurring on-air gathering on a fixed
// frequency/mode at a regular time. Conceptually a memory channel (the tuning
// preset) plus a recurrence rule, a reminder lead-time, and descriptive
// metadata.
//
// Unlike MemoryEntry — which is radio-authoritative and lives in the radio's
// memory slots (Constitution Principles II & III) — a NetEntry is purely
// operator-scoped client state (Principle XIII): it must work whether or not a
// radio is connected and never consumes a radio memory slot. It is persisted
// locally as versioned JSON (see NetScheduleStore).
//
// The recurrence is stored as an RFC 5545 RRULE string (portable, the standard
// every calendar interoperates on) plus the local wall-clock time-of-day and an
// IANA timezone id. The firing instant is always computed lazily from those so
// "20:00 local" stays correct across DST transitions (see NetRecurrence).
struct NetEntry {
    QString     id;                      // stable UUID (no braces) — merge/import key
    QString     name;                    // user label, e.g. "Tuesday County ARES Net"
    bool        enabled{true};

    // Recurrence (see NetRecurrence for the supported RRULE subset).
    QString     rrule;                   // e.g. "FREQ=WEEKLY;BYDAY=TU"
    QString     startDate;               // DTSTART date, ISO "yyyy-MM-dd"; anchors INTERVAL phase
    QString     timeOfDay{"20:00"};      // local wall-clock "HH:MM" in `timezone`
    QString     timezone{"Etc/UTC"};     // IANA id; "Etc/UTC" for UTC/Zulu nets
    int         reminderLeadMinutes{10}; // fire the reminder this many minutes before start
    int         durationMinutes{60};     // informational; how long the net runs

    // Tuning preset — reuses the radio memory-channel shape so recall can drive
    // the same MemoryRecallPolicy command builders the memory dialog uses.
    MemoryEntry preset;

    QString     notes;                   // free text: NCS callsign, NetLogger name, etc.
};

} // namespace AetherSDR
