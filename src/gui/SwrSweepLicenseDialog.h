#pragma once

#include "PersistentDialog.h"

class QCheckBox;

namespace AetherSDR {

// Modal operator-licence confirmation gating the Antenna SWR sweep (mirrors the
// ATU Band Pre-Tune disclaimer). "Remember my answer" persists
// SwrSweepLicenseConfirmed. Call the static confirm(), which handles the
// short-circuit, exec() and persistence.
class SwrSweepLicenseDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit SwrSweepLicenseDialog(QWidget* parent = nullptr);

    // True when the user has either previously confirmed (with the
    // remember-my-answer checkbox) or just confirmed in this session.
    // Returns false if the user cancels.  Safe to call repeatedly.
    static bool confirm(QWidget* parent = nullptr, bool force = false);

private:
    QCheckBox* m_rememberCheck{nullptr};
};

} // namespace AetherSDR
