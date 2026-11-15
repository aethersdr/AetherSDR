#pragma once

#include <QString>
#include <QWidget>

#include <array>

namespace AetherSDR {

// A dB value over the last few seconds, newest at the right, on a fixed
// axis with one reference line dashed across it (the LVLR's Max gain, the
// peak control's ceiling).
class ClientDbHistoryWidget : public QWidget {
    Q_OBJECT

public:
    static constexpr int kPoints = 150;  // 5 s at the 30 Hz the owner polls

    explicit ClientDbHistoryWidget(QWidget* parent = nullptr);

    void setAxis(float minDb, float maxDb);
    void setReference(float db, const QString& label);
    void push(float db);
    void clear();
    void setActive(bool active);

    QSize sizeHint() const override { return {480, 140}; }
    QSize minimumSizeHint() const override { return {200, 60}; }

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    std::array<float, kPoints> m_history{};
    int m_head{0};
    int m_count{0};
    float m_minDb{0.0f};
    float m_maxDb{20.0f};
    float m_referenceDb{5.0f};
    QString m_referenceLabel;
    bool m_active{false};
};

}  // namespace AetherSDR
