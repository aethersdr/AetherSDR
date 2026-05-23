#pragma once

#include <QObject>
#include <QMap>

namespace MasterSDR {

struct TnfEntry {
    int    id{0};
    double freqMhz{0.0};
    int    widthHz{100};
    int    depthDb{1};       // 1=normal, 2=deep, 3=very deep
    bool   permanent{false};
};

class TnfModel : public QObject {
    Q_OBJECT

public:
    explicit TnfModel(QObject* parent = nullptr);

    // ── Accessors ────────────────────────────────────────────────────────
    const QMap<int, TnfEntry>& tnfs() const { return m_tnfs; }
    const TnfEntry* tnf(int id) const;
    bool globalEnabled() const { return m_globalEnabled; }

    // ── Status parsing (called from RadioModel) ─────────────────────────
    void applyTnfStatus(int id, const QMap<QString, QString>& kvs);
    void removeTnf(int id);
    void applyGlobalEnabled(bool on);

    // ── Commands (emit commandReady) ────────────────────────────────────
    void createTnf(double freqMhz);
    void setTnfFreq(int id, double freqMhz);
    void setTnfWidth(int id, int widthHz);
    void setTnfDepth(int id, int depthDb);
    void setTnfPermanent(int id, bool on);
    void requestRemoveTnf(int id);
    void requestGlobalTnfEnabled(bool on);

    void clear();

signals:
    void tnfChanged(int id);
    void tnfRemoved(int id);
    void globalEnabledChanged(bool on);
    void commandReady(const QString& cmd);

private:
    QMap<int, TnfEntry> m_tnfs;
    bool m_globalEnabled{true};
};

} // namespace MasterSDR
