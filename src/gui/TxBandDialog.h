#pragma once

#include "CanonWindow.h"

namespace AetherSDR {

class RadioModel;

// TX Band Settings: per-band power, inhibit and keying outputs, in a canon
// workspace window.
class TxBandDialog : public CanonWindow {
    Q_OBJECT

public:
    explicit TxBandDialog(RadioModel* model, QWidget* parent = nullptr);

private:
    RadioModel* m_model{nullptr};
};

} // namespace AetherSDR
