#pragma once

#include "models/CwxModel.h"
#include "core/backends/flex/FlexCwxControls.h"
#include <QList>
#include <QVariant>

// Wire fixtures exercise the production backend encoder, not a duplicate
// encoder or an above-seam raw command. No transport is installed or opened.
class CwxWireSpy final : public QObject, public QList<QList<QVariant>> {
public:
    CwxWireSpy(AetherSDR::CwxModel* model, bool replies)
    {
        if (replies) {
            QObject::connect(model, &AetherSDR::CwxModel::replyControlRequested, this,
                [this](const AetherSDR::CwTextRequest& request, int epoch, int chars) {
                    append({AetherSDR::FlexCwxControls::encode(request), epoch, chars});
                });
        } else {
            QObject::connect(model, &AetherSDR::CwxModel::controlRequested, this,
                [this](const AetherSDR::CwTextRequest& request) {
                    append({AetherSDR::FlexCwxControls::encode(request)});
                });
        }
    }
};
