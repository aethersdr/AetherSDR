#pragma once

#include "models/MemoryEntry.h"

#include <QString>

namespace AetherSDR {

// A scheduled on-air net: a tuning preset plus a recurrence rule, reminder
// lead-time and metadata. Unlike MemoryEntry (radio-authoritative), this is
// client-owned state: works without a radio, uses no radio memory slot, and
// persists locally as versioned JSON (NetScheduleStore). Recurrence is an
// RFC 5545 RRULE plus local time-of-day and IANA zone; the firing instant is
// computed lazily so local times survive DST (NetRecurrence).
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
