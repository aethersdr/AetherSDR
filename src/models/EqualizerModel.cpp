#include "EqualizerModel.h"
#include "core/AppSettings.h"
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QtGlobal>
#include <algorithm>

namespace AetherSDR {

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
    emit commandReady(QString("eq TXsc mode=%1").arg(on ? "True" : "False"));
    emit txStateChanged();
    emit operatorEdited();
}

void EqualizerModel::setRxEnabled(bool on)
{
    m_rxEnabled = on;
    emit commandReady(QString("eq RXsc mode=%1").arg(on ? "True" : "False"));
    emit rxStateChanged();
    emit operatorEdited();
}

void EqualizerModel::setTxBand(Band b, int dB)
{
    m_txBands[b] = dB;
    emit commandReady(QString("eq TXsc %1Hz=%2").arg(kBandKeys[b]).arg(dB));
    emit txStateChanged();
    emit operatorEdited();
}

void EqualizerModel::setRxBand(Band b, int dB)
{
    m_rxBands[b] = dB;
    emit commandReady(QString("eq RXsc %1Hz=%2").arg(kBandKeys[b]).arg(dB));
    emit rxStateChanged();
    emit operatorEdited();
}

// ── Client-side persistence ─────────────────────────────────────────────────

namespace {

const QString kSettingsKey = QStringLiteral("EqApplet");
const QString kStateMember = QStringLiteral("graphicEq");
const QString kTxEnabledMember = QStringLiteral("txEnabled");
const QString kRxEnabledMember = QStringLiteral("rxEnabled");
const QString kTxBandsMember = QStringLiteral("tx");
const QString kRxBandsMember = QStringLiteral("rx");

using Bands = std::array<int, EqualizerModel::BandCount>;

// All eight or none: a short array cannot say which bands it means.
Bands bandsFromJson(const QJsonValue& value)
{
    Bands bands{};
    const QJsonArray array = value.toArray();
    if (array.size() != EqualizerModel::BandCount) {
        return bands;
    }
    for (int i = 0; i < EqualizerModel::BandCount; ++i) {
        bands[i] = qBound(EqualizerModel::kMinBandDb, array.at(i).toInt(0),
                          EqualizerModel::kMaxBandDb);
    }
    return bands;
}

QJsonArray bandsToJson(const Bands& bands)
{
    QJsonArray array;
    for (int dB : bands) {
        array.append(dB);
    }
    return array;
}

QJsonObject readSettingsObject()
{
    return QJsonDocument::fromJson(
        AppSettings::instance().value(kSettingsKey, "{}").toString().toUtf8()).object();
}

}  // namespace

bool EqualizerModel::State::anyBandNonZero() const
{
    const auto nonZero = [](int dB) { return dB != 0; };
    return std::any_of(txBands.begin(), txBands.end(), nonZero)
        || std::any_of(rxBands.begin(), rxBands.end(), nonZero);
}

EqualizerModel::State EqualizerModel::state() const
{
    State s;
    s.txEnabled = m_txEnabled;
    s.rxEnabled = m_rxEnabled;
    s.txBands = m_txBands;
    s.rxBands = m_rxBands;
    return s;
}

void EqualizerModel::restoreState(const State& s)
{
    const bool txChanged = s.txEnabled != m_txEnabled || s.txBands != m_txBands;
    const bool rxChanged = s.rxEnabled != m_rxEnabled || s.rxBands != m_rxBands;
    m_txEnabled = s.txEnabled;
    m_rxEnabled = s.rxEnabled;
    m_txBands = s.txBands;
    m_rxBands = s.rxBands;
    if (txChanged) {
        emit txStateChanged();
    }
    if (rxChanged) {
        emit rxStateChanged();
    }
}

EqualizerModel::State EqualizerModel::stateFromJson(const QJsonObject& eqAppletCfg)
{
    const QJsonObject eq = eqAppletCfg.value(kStateMember).toObject();
    State s;
    s.txEnabled = eq.value(kTxEnabledMember).toBool(false);
    s.rxEnabled = eq.value(kRxEnabledMember).toBool(false);
    s.txBands = bandsFromJson(eq.value(kTxBandsMember));
    s.rxBands = bandsFromJson(eq.value(kRxBandsMember));
    return s;
}

void EqualizerModel::stateToJson(const State& s, QJsonObject& eqAppletCfg)
{
    QJsonObject eq;
    eq.insert(kTxEnabledMember, s.txEnabled);
    eq.insert(kRxEnabledMember, s.rxEnabled);
    eq.insert(kTxBandsMember, bandsToJson(s.txBands));
    eq.insert(kRxBandsMember, bandsToJson(s.rxBands));
    eqAppletCfg.insert(kStateMember, eq);
}

EqualizerModel::State EqualizerModel::loadPersistedState()
{
    return stateFromJson(readSettingsObject());
}

bool EqualizerModel::savePersistedState(const State& s)
{
    QJsonObject cfg = readSettingsObject();
    if (stateFromJson(cfg) == s) {
        return false;
    }
    stateToJson(s, cfg);
    AppSettings& settings = AppSettings::instance();
    settings.setValue(kSettingsKey, QString::fromUtf8(
        QJsonDocument(cfg).toJson(QJsonDocument::Compact)));
    settings.save();
    return true;
}

} // namespace AetherSDR
