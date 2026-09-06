#pragma once

#include "ControlResourceStore.h"

#include <QObject>
#include <QSet>
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
                         QObject* parent = nullptr);

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
    QSet<SliceModel*> m_slices;
    QSet<PanadapterModel*> m_panadapters;
};

} // namespace control
} // namespace AetherSDR
