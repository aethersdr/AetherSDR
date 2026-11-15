#pragma once

#include "CanonWindow.h"
#include "models/RadioModel.h"

#include <QPointer>
#include <functional>

class QLabel;
class QPushButton;

namespace AetherSDR {

// Observes the current radio's PD service; calibration arm never keys RF.
class AetherPDDialog : public CanonWindow {
    Q_OBJECT
public:
    using TokenLoader = std::function<void(const QString&, QObject*,
                                           std::function<void(const QString&)>)>;
    explicit AetherPDDialog(RadioModel* radio, QWidget* parent = nullptr);
    AetherPDDialog(RadioModel* radio, TokenLoader loadToken, QWidget* parent = nullptr);

public slots:
    void done(int result) override;

private:
    void refresh();
    void render();
    void request(aetherpd::Operation operation);
    bool current() const;

    QPointer<RadioModel> m_radio;
    RadioControlSession m_session;
    QString m_serial;
    TokenLoader m_loadToken;
    quint64 m_loadGeneration{0};
    bool m_loading{false};
    bool m_closed{false};
    QString m_notice;
    QLabel* m_state{nullptr};
    QLabel* m_detail{nullptr};
    QLabel* m_receipt{nullptr};
    QLabel* m_message{nullptr};
    QPushButton* m_refresh{nullptr};
    QPushButton* m_start{nullptr};
    QPushButton* m_arm{nullptr};
    QPushButton* m_cancel{nullptr};
    QPushButton* m_stop{nullptr};
    QPushButton* m_restore{nullptr};
};

} // namespace AetherSDR
