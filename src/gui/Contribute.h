#pragma once

#include "PersistentDialog.h"

namespace AetherSDR {

class ContributeDialog final : public PersistentDialog {
public:
    explicit ContributeDialog(QWidget* parent = nullptr);
};

} // namespace AetherSDR
