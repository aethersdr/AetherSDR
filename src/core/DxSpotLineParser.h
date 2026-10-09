#pragma once

#include "core/DxSpot.h"

#include <QString>

// Pure, dependency-light parsing for DX-cluster telnet lines — split out from
// DxClusterClient (the QTcpSocket client) so it can be unit tested without
// pulling in logging/socket infrastructure (mirrors N1MMSpotParser).
namespace AetherSDR::DxSpotLineParser {

// True for a pre-login banner line naming GoCluster (N2WQ/GoCluster). The
// banner is operator-configurable; a node that drops the name is parsed with
// the generic layout, which still works — it just keeps the tail in the comment.
// A false positive (another server's MOTD naming GoCluster) is harmless:
// parseSpotLine() checks every tail column and falls back when they don't fit.
bool isGoClusterBanner(const QString& line);

// Parse a "DX de" spot line. With goCluster set, GoCluster's fixed 15-column
// tail ("[ ][path][ ][grid4][ ][conf][ ]HHMMZ") is split off first, by
// position from the line end; a line that fails that structural check falls
// back to the generic layout. The comment has its internal padding collapsed,
// and a leading "<mode> <n> dB" report fills snr/hasSnr.
bool parseSpotLine(const QString& line, DxSpot& spot, bool goCluster = false);

// Read the skimmer-style report that leads a comment: "CW 23 dB", "FT8 +12 dB".
// Only the token straight after the first word counts, so free text such as
// "20 dB over S9" is never read as an SNR.
bool extractReportSnr(const QString& comment, int& snr);

} // namespace AetherSDR::DxSpotLineParser
