#include "TnfModel.h"
#include "core/LogManager.h"
#include <QDebug>
#include <algorithm>

namespace AetherSDR {

TnfModel::TnfModel(QObject* parent)
    : QObject(parent)
{}

const TnfEntry* TnfModel::tnf(int id) const
{
    auto it = m_tnfs.constFind(id);
    return it != m_tnfs.constEnd() ? &(*it) : nullptr;
}

// ── Status parsing ──────────────────────────────────────────────────────────

void TnfModel::applyTnfStatus(int id, const QMap<QString, QString>& kvs)
{
    auto& t = m_tnfs[id];
    t.id = id;

    if (kvs.contains("freq"))
        t.freqMhz = kvs["freq"].toDouble();
    if (kvs.contains("width")) {
        // Firmware v1.4.0.0 reports width in Hz ("width=100"), but tolerate
        // older fractional-MHz forms defensively.
        const double rawWidth = kvs["width"].toDouble();
        if (rawWidth > 0.0 && rawWidth < 1.0) {
            t.widthHz = static_cast<int>(rawWidth * 1.0e6 + 0.5);
        } else {
            t.widthHz = static_cast<int>(rawWidth + 0.5);
        }
        if (t.widthHz < 10) {
            t.widthHz = 100;
        }
    }
    if (kvs.contains("depth"))
        t.depthDb = kvs["depth"].toInt();
    if (kvs.contains("permanent"))
        t.permanent = kvs["permanent"] == "1";

    qCDebug(lcProtocol) << "TnfModel: TNF" << id << "freq=" << t.freqMhz
             << "width=" << t.widthHz << "depth=" << t.depthDb;
    emit tnfChanged(id);
}

void TnfModel::applyNotchDelta(int id, const NotchDelta& delta)
{
    auto& t = m_tnfs[id];
    t.id = id;
    // Hz on the seam, MHz in the entry — the UI works in MHz because the
    // panadapter does.
    if (delta.centerHz)
        t.freqMhz = *delta.centerHz / 1.0e6;
    if (delta.widthHz)
        t.widthHz = static_cast<int>(*delta.widthHz + 0.5);
    if (delta.depthDb)
        t.depthDb = *delta.depthDb;
    if (delta.permanent)
        t.permanent = *delta.permanent;
    // delta.active is deliberately NOT carried: TnfEntry has no such field, and
    // a per-notch bypass has no representation in the overlay or the TNF menu.
    // It is backend-internal — see the note in NotchDelta.h.

    qCDebug(lcProtocol) << "TnfModel: notch" << id << "freq=" << t.freqMhz
             << "width=" << t.widthHz;
    emit tnfChanged(id);
}

void TnfModel::removeTnf(int id)
{
    if (m_tnfs.remove(id)) {
        qCDebug(lcProtocol) << "TnfModel: removed TNF" << id;
        emit tnfRemoved(id);
    }
}

void TnfModel::applyGlobalEnabled(bool on)
{
    if (m_globalEnabled == on) return;
    m_globalEnabled = on;
    emit globalEnabledChanged(on);
}

// ── Commands ────────────────────────────────────────────────────────────────

void TnfModel::createTnf(double freqMhz)
{
    // Width comes from the model's own default rather than being left to the
    // backend, so a notch is the same size wherever it is created. A Flex
    // ignores it (its wire has no width at create time) and re-reports whatever
    // the radio chose; a host-DSP backend honours it.
    emit notchCreateRequested(freqMhz * 1.0e6,
                              static_cast<double>(TnfEntry {}.widthHz));
}

void TnfModel::setTnfFreq(int id, double freqMhz)
{
    NotchDelta delta;
    delta.centerHz = freqMhz * 1.0e6;
    emit notchChangeRequested(id, delta);

    auto it = m_tnfs.find(id);
    if (it != m_tnfs.end() && !qFuzzyCompare(it->freqMhz + 1.0, freqMhz + 1.0)) {
        it->freqMhz = freqMhz;
        emit tnfChanged(id);
    }
}

void TnfModel::setTnfWidth(int id, int widthHz)
{
    const int clampedWidthHz = std::max(10, widthHz);
    NotchDelta delta;
    delta.widthHz = static_cast<double>(clampedWidthHz);
    emit notchChangeRequested(id, delta);

    auto it = m_tnfs.find(id);
    if (it != m_tnfs.end() && it->widthHz != clampedWidthHz) {
        it->widthHz = clampedWidthHz;
        emit tnfChanged(id);
    }
}

void TnfModel::setTnfDepth(int id, int depthDb)
{
    const int clampedDepthDb = std::clamp(depthDb, 1, 3);
    NotchDelta delta;
    delta.depthDb = clampedDepthDb;
    emit notchChangeRequested(id, delta);

    auto it = m_tnfs.find(id);
    if (it != m_tnfs.end() && it->depthDb != clampedDepthDb) {
        it->depthDb = clampedDepthDb;
        emit tnfChanged(id);
    }
}

void TnfModel::setTnfPermanent(int id, bool on)
{
    NotchDelta delta;
    delta.permanent = on;
    emit notchChangeRequested(id, delta);
    // Radio doesn't send status update — update locally
    auto it = m_tnfs.find(id);
    if (it != m_tnfs.end()) {
        it->permanent = on;
        emit tnfChanged(id);
    }
}

void TnfModel::requestRemoveTnf(int id)
{
    emit notchRemoveRequested(id);
    // Radio does not send a removal status — remove optimistically
    removeTnf(id);
}

void TnfModel::requestGlobalTnfEnabled(bool on)
{
    emit notchesEnabledRequested(on);
    // Optimistic update — radio echoes tnf_enabled in status, but update
    // immediately so the UI reflects the change without waiting for the echo.
    if (m_globalEnabled != on) {
        m_globalEnabled = on;
        emit globalEnabledChanged(on);
    }
}

void TnfModel::clear()
{
    m_tnfs.clear();
    // The global bypass is session state too, and it has to be reset for the
    // same reason the notches are: Hl2Backend::connectRadio() puts its own
    // m_notchesEnabled back to true, and nothing on a host-DSP backend echoes
    // the flag back the way a Flex's `tnf_enabled` status does. Left alone, an
    // operator who switched notches off before disconnecting would reconnect to
    // a toggle reading OFF over a DSP that is notching — and the first notch
    // they place would be audible with the control that governs it saying it
    // cannot be. A Flex re-reports the real value at connect either way.
    if (!m_globalEnabled) {
        m_globalEnabled = true;
        emit globalEnabledChanged(m_globalEnabled);
    }
}

} // namespace AetherSDR
