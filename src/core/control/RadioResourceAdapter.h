#pragma once

#include "ControlResourceStore.h"
#include "RadioConnectionTarget.h"

#include <QObject>
#include <QSet>
#include <QPointer>
#include <QString>

namespace AetherSDR {

class PanadapterModel;
class RadioModel;
class SliceModel;

namespace control {

// Converts the existing normalized model graph into complete protocol
// resources. It is strictly observational: no model setter or backend intent
// is reachable through this adapter.
class RadioResourceAdapter final : public QObject {
    Q_OBJECT

public:
    RadioResourceAdapter(RadioModel* radio,
                         ControlResourceStore* resources,
                         QString radioSessionId,
                         QObject* parent = nullptr,
                         RadioConnectionTarget* connectionTarget = nullptr);

    [[nodiscard]] QString radioSessionId() const { return m_radioSessionId; }
    void publishAll();

private:
    void attachSlice(SliceModel* slice);
    void refreshSlice(SliceModel* slice);
    void attachPanadapter(PanadapterModel* panadapter);
    void refreshPanadapter(PanadapterModel* panadapter);
    void clearDynamicResources();
    void publishRadioSession();
    void publishSlice(SliceModel* slice);
    void publishPanadapter(PanadapterModel* panadapter);

    RadioModel* m_radio{nullptr};
    ControlResourceStore* m_resources{nullptr};
    QString m_radioSessionId;
    QPointer<RadioConnectionTarget> m_connectionTarget;
    QSet<SliceModel*> m_slices;
    // Backend capabilities are rebuilt on every RadioModel::backendCapabilities()
    // call; cache the slice-frequency authority string and refill it on the
    // same edges that republish the radio session (capabilities, rebuild,
    // connection). Empty means "read it on the next publish".
    QString m_frequencyAuthority;
    QSet<PanadapterModel*> m_panadapters;
};

} // namespace control
} // namespace AetherSDR
