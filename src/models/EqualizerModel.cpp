#include "EqualizerModel.h"
#include <QDebug>

namespace MasterSDR {

// Band key strings used in protocol messages (e.g. "63Hz", "1000Hz")
static constexpr const char* kBandKeys[] = {
    "63", "125", "250", "500", "1000", "2000", "4000", "8000"
};

// Short labels for the UI
static constexpr const char* kBandLabels[] = {
    "63", "125", "250", "500", "1k", "2k", "4k", "8k"
};

EqualizerModel::EqualizerModel(QObject* parent)
    : QObject(parent)
{
    m_txBands.fill(0);
    m_rxBands.fill(0);
}

QString EqualizerModel::bandKey(Band b)
{
    return kBandKeys[b];
}

QString EqualizerModel::bandLabel(Band b)
{
    return kBandLabels[b];
}

// ── Status parsing ──────────────────────────────────────────────────────────

void EqualizerModel::applyTxEqStatus(const QMap<QString, QString>& kvs)
{
    applyEqStatus(kvs, true);
}

void EqualizerModel::applyRxEqStatus(const QMap<QString, QString>& kvs)
{
    applyEqStatus(kvs, false);
}

void EqualizerModel::applyEqStatus(const QMap<QString, QString>& kvs, bool isTx)
{
    auto& bands   = isTx ? m_txBands : m_rxBands;
    bool& enabled = isTx ? m_txEnabled : m_rxEnabled;

    if (kvs.contains("mode"))
        enabled = (kvs["mode"] == "1");

    for (int i = 0; i < BandCount; ++i) {
        const QString key = QString("%1Hz").arg(kBandKeys[i]);
        if (kvs.contains(key))
            bands[i] = kvs[key].toInt();
    }

    if (isTx)
        emit txStateChanged();
    else
        emit rxStateChanged();
}

// ── Commands ────────────────────────────────────────────────────────────────

void EqualizerModel::setTxEnabled(bool on)
{
    m_txEnabled = on;
    emit commandReady(QString("eq txsc mode=%1").arg(on ? "1" : "0"));
    emit txStateChanged();
}

void EqualizerModel::setRxEnabled(bool on)
{
    m_rxEnabled = on;
    emit commandReady(QString("eq rxsc mode=%1").arg(on ? "1" : "0"));
    emit rxStateChanged();
}

void EqualizerModel::setTxBand(Band b, int dB)
{
    m_txBands[b] = dB;
    emit commandReady(QString("eq txsc %1Hz=%2").arg(kBandKeys[b]).arg(dB));
    emit txStateChanged();
}

void EqualizerModel::setRxBand(Band b, int dB)
{
    m_rxBands[b] = dB;
    emit commandReady(QString("eq rxsc %1Hz=%2").arg(kBandKeys[b]).arg(dB));
    emit rxStateChanged();
}

} // namespace MasterSDR
