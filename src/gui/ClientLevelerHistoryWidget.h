#pragma once

#include <QWidget>

#include <array>

namespace AetherSDR {

// The leveler's applied gain over the last few seconds, newest at the right,
// on a 0..+20 dB axis with the Max gain ceiling dashed across it.
class ClientLevelerHistoryWidget : public QWidget {
    Q_OBJECT

public:
    static constexpr int kPoints = 150;  // 5 s at the 30 Hz the owner polls

    explicit ClientLevelerHistoryWidget(QWidget* parent = nullptr);

    void push(float gainDb);
    void clear();
    void setMaxGainDb(float db);
    void setActive(bool active);

    QSize sizeHint() const override { return {480, 140}; }
    QSize minimumSizeHint() const override { return {200, 60}; }

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    std::array<float, kPoints> m_history{};
    int m_head{0};
    int m_count{0};
    float m_maxGainDb{5.0f};
    bool m_active{false};
};

}  // namespace AetherSDR
