#pragma once

#include "CanonWindow.h"

#include <QList>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QVBoxLayout;

namespace AetherSDR {

class RadioModel;

// TX Band Settings: per-band power, inhibit and keying outputs, in a canon
// workspace window.
class TxBandDialog : public CanonWindow {
    Q_OBJECT

public:
    explicit TxBandDialog(RadioModel* model, QWidget* parent = nullptr);

protected:
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    struct Row {
        int bandId{0};
        QString bandName;
        QPointer<QLineEdit> rfPower;
        QPointer<QLineEdit> tunePower;
        QList<QPointer<QCheckBox>> toggles;  // the eight bandset columns, in order
        // The radio's values as last written into the widgets.
        int rfPowerShown{0};
        int tunePowerShown{0};
        QList<bool> togglesShown;
    };

    void buildTable();
    void rebuildTable();
    void refreshValues();

    RadioModel* m_model{nullptr};
    QString m_profile;
    std::function<bool()> m_sameSession;  // the table's radio session is still current
    QVBoxLayout* m_layout{nullptr};
    QPointer<QLabel> m_headerTitle;
    QPointer<QWidget> m_table;
    QList<Row> m_rows;
    QTimer m_refreshTimer;
    bool m_rebuildOnShow{false};
};

} // namespace AetherSDR
