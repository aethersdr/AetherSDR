#pragma once

#include "PersistentDialog.h"

class QCheckBox;

namespace MasterSDR {

// Modal license-confirmation dialog gating the Antenna SWR sweep.
//
// First time the user hits Start Sweep, this dialog appears with an
// operator-responsibility disclaimer (mirrors the ATU Band Pre-Tune
// disclaimer) and two buttons: "I am licensed to use this feature"
// and "Cancel".  A "Remember my answer" checkbox persists the
// confirmation to AppSettings under SwrSweepLicenseConfirmed so
// subsequent presses go straight to the sweep without the popup.
//
// Use the static confirm() helper at the call site — it handles the
// AppSettings short-circuit, the modal exec(), and the persistence
// write so callers don't need to manage any of that themselves.
class SwrSweepLicenseDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit SwrSweepLicenseDialog(QWidget* parent = nullptr);

    // True when the user has either previously confirmed (with the
    // remember-my-answer checkbox) or just confirmed in this session.
    // Returns false if the user cancels.  Safe to call repeatedly.
    static bool confirm(QWidget* parent = nullptr);

private:
    QCheckBox* m_rememberCheck{nullptr};
};

} // namespace MasterSDR
