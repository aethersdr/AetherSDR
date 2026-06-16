#pragma once

#include <QVector>
#include <QWidget>

namespace AetherSDR {

// Tiny inline trend chart: a fixed-width polyline of recent samples scaled
// to the data's own range. Used for the PSK Reporter "recent activity"
// indicator (receivers hearing us, per minute), but kept generic.
class Sparkline : public QWidget {
    Q_OBJECT

public:
    explicit Sparkline(QWidget* parent = nullptr);

    void setCapacity(int n);
    void addSample(double value);              // push one, drop oldest past cap
    void setSamples(const QVector<double>& s); // replace whole series (backfill)
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;
    QSize sizeHint() const override;

private:
    QVector<double> m_samples;
    int m_capacity{60};
};

} // namespace AetherSDR
