#pragma once

namespace AetherSDR {

// Generic radio-message classification enums, split out of the (vendor-tagged)
// CommandParser.h so code above the radio seam can consume them without pulling
// in the SmartSDR wire parser (aetherd RFC step 2.4 / EB3). These describe
// message *category* and *severity*, not any vendor's wire format — a UI slot
// that reacts to "an Error-severity radio message arrived" needs only this, not
// the parser that produced it. CommandParser.h includes this header, so the
// ParsedMessage struct and existing includers keep compiling unchanged.

// Line type in the SmartSDR TCP protocol (V/H/R/S/M prefixes). Kept here as the
// generic classification; the actual wire decode lives in CommandParser.
enum class MessageType {
    Version,
    Handle,
    Response,
    Status,
    Message,
    Unknown
};

// Severity of an informational/warning/error/fatal ("M") radio message. Info is
// logged silently; Warning and above surface to the user. (SmartSDR encodes
// this in bits 24-25 of the message number per FlexLib `(num >> 24) & 0x3`; the
// values are the generic severity ladder, independent of that encoding.)
enum class MessageSeverity {
    Info    = 0,
    Warning = 1,
    Error   = 2,
    Fatal   = 3
};

}  // namespace AetherSDR
