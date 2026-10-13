#pragma once

#include <QString>

namespace AetherSDR {

// GoCluster-specific spot handling: one JSON object under "GoCluster",
// read/written atomically. Shape:
//   {"hideUnverified": {"cluster": false, "rbn": false}}
// hideUnverified.<feed> drops spots GoCluster tags with the '?' confidence on
// that SpotHub feed ("cluster" = Cluster tab, "rbn" = RBN tab). Client-side on
// purpose: GoCluster saves REJECT CONFIDENCE per callsign on the server, where
// turning it off here could not undo it. Process-wide, through AppSettings.
class GoClusterSettings {
public:
    static constexpr const char* kFeedCluster = "cluster";
    static constexpr const char* kFeedRbn     = "rbn";

    static bool hideUnverified(const QString& feed);
    static void setHideUnverified(const QString& feed, bool on);
};

} // namespace AetherSDR
