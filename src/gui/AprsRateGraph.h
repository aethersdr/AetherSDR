#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

namespace AetherSDR {

// Two-series time histogram for the Digi tab: heard APRS frames vs fill-in
// repeats. 10-second buckets covering the last six hours; the paint path
// folds those into the operator-selected window.
class AprsRateGraph : public QWidget {
    Q_OBJECT

public:
    explicit AprsRateGraph(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setAccentToken(const QString& token);
    void setWindowMinutes(int minutes);
    int windowMinutes() const { return m_windowMin; }

    void recordEvent();
    void clear();

    int eventsInWindow() const;

    QSize sizeHint() const override { return {240, 88}; }
    QSize minimumSizeHint() const override { return {160, 72}; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    int currentBucket() const;
    void advanceBucket();

    QString m_title;
    QString m_accentToken{QStringLiteral("color.accent")};
    int m_windowMin{15};
    static constexpr int kBucketSecs = 10;
    static constexpr int kMaxHours = 6;
    static constexpr int kBuckets = (kMaxHours * 3600) / kBucketSecs;
    QVector<int> m_counts;
    int m_head{0};          // index of the current bucket
    qint64 m_headStartMs{0};
};
} // namespace AetherSDR
