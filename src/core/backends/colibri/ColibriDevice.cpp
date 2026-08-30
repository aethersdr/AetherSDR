#include "core/backends/colibri/ColibriDevice.h"

#include <QLoggingCategory>
#include <QMetaType>

#include <algorithm>

Q_LOGGING_CATEGORY(lcColibriDev, "aether.colibri.device")

namespace AetherSDR::colibri {

ColibriDevice::ColibriDevice(QObject* parent) : QObject(parent)
{
    // iqBlockReady crosses from the library's thread to the I/O thread as a
    // queued connection; without the registration Qt drops the emission with
    // only a warning.
    qRegisterMetaType<std::vector<std::complex<float>>>(
        "std::vector<std::complex<float>>");
    qRegisterMetaType<ColibriDevice::Params>(
        "AetherSDR::colibri::ColibriDevice::Params");
}

ColibriDevice::~ColibriDevice()
{
    closeDevice();
}

bool COLIBRI_CALL ColibriDevice::onRx(ColibriComplex* iq, std::uint32_t len,
                                      bool adcOverload, void* user)
{
    auto* self = static_cast<ColibriDevice*>(user);
    if (!self || !self->m_acceptBlocks.load(std::memory_order_acquire))
        return false;   // stop delivering; the stream is being torn down

    self->m_samples.fetch_add(len, std::memory_order_relaxed);
    self->m_blocks.fetch_add(1, std::memory_order_relaxed);
    if (self->m_adcOverload.exchange(adcOverload, std::memory_order_relaxed)
        != adcOverload) {
        emit self->adcOverloadChanged(adcOverload);
    }

    // ColibriComplex and std::complex<float> are layout-identical (two floats,
    // re then im), so this is one copy into the signal payload and nothing
    // else on the library's thread.
    std::vector<std::complex<float>> block(len);
    if (len > 0)
        std::copy_n(reinterpret_cast<const std::complex<float>*>(iq), len,
                    block.begin());
    emit self->iqBlockReady(block);
    return true;
}

void ColibriDevice::openDevice(const Params& params)
{
    if (m_dev) {
        emit openFailed(QStringLiteral("ColibriNANO already open"));
        return;
    }

    auto& lib = ColibriLib::instance();
    QString error;
    if (!lib.ensureLoaded(params.dllPath, &error)) {
        emit openFailed(error);
        return;
    }

    const int srIndex = colibriSampleRateIndex(params.sampleRateHz);
    if (srIndex < 0) {
        emit openFailed(QStringLiteral("unsupported sample rate %1 Hz")
                            .arg(params.sampleRateHz));
        return;
    }

    if (!lib.open(&m_dev, params.deviceIndex) || !m_dev) {
        m_dev = nullptr;
        emit openFailed(QStringLiteral("could not open ColibriNANO #%1 "
                                       "(unplugged, or in use by another program?)")
                            .arg(params.deviceIndex));
        return;
    }
    // Latched for the whole open lifetime, so the discovery poll never
    // enumerates the FTDI bus under our running stream.
    lib.setDeviceInUse(true);

    m_frequencyHz = params.frequencyHz;
    m_preampDb = params.preampDb;
    lib.setFrequency(m_dev, static_cast<std::uint32_t>(std::max(0.0, m_frequencyHz)));
    lib.setPreamp(m_dev, static_cast<float>(m_preampDb));

    m_samples.store(0);
    m_blocks.store(0);
    m_adcOverload.store(false);
    m_acceptBlocks.store(true, std::memory_order_release);
    if (!lib.start(m_dev, srIndex, &ColibriDevice::onRx, this)) {
        m_acceptBlocks.store(false, std::memory_order_release);
        lib.close(m_dev);
        m_dev = nullptr;
        lib.setDeviceInUse(false);
        emit openFailed(QStringLiteral("ColibriNANO opened but the IQ stream "
                                       "would not start"));
        return;
    }
    m_streaming = true;
    qCInfo(lcColibriDev) << "streaming at" << params.sampleRateHz << "Hz, NCO"
                         << m_frequencyHz << "Hz, preamp" << m_preampDb << "dB";
    emit opened();
}

void ColibriDevice::closeDevice()
{
    if (!m_dev)
        return;
    auto& lib = ColibriLib::instance();
    // Refuse further blocks BEFORE stop(): the library may already be inside
    // the callback on its own thread, and the flag is what makes that final
    // delivery a no-op instead of an emit into a dying stream.
    m_acceptBlocks.store(false, std::memory_order_release);
    if (m_streaming) {
        lib.stop(m_dev);
        m_streaming = false;
    }
    lib.close(m_dev);
    m_dev = nullptr;
    lib.setDeviceInUse(false);
    qCInfo(lcColibriDev) << "closed";
}

bool ColibriDevice::restart(int sampleRateHz)
{
    if (!m_dev)
        return false;
    const int srIndex = colibriSampleRateIndex(sampleRateHz);
    if (srIndex < 0)
        return false;

    auto& lib = ColibriLib::instance();
    if (m_streaming) {
        m_acceptBlocks.store(false, std::memory_order_release);
        lib.stop(m_dev);
        m_streaming = false;
    }
    m_acceptBlocks.store(true, std::memory_order_release);
    if (!lib.start(m_dev, srIndex, &ColibriDevice::onRx, this)) {
        m_acceptBlocks.store(false, std::memory_order_release);
        qCWarning(lcColibriDev) << "restart at" << sampleRateHz << "Hz failed";
        return false;
    }
    m_streaming = true;
    qCInfo(lcColibriDev) << "restarted at" << sampleRateHz << "Hz";
    return true;
}

void ColibriDevice::setFrequencyHz(double hz)
{
    m_frequencyHz = hz;
    if (m_dev)
        ColibriLib::instance().setFrequency(
            m_dev, static_cast<std::uint32_t>(std::max(0.0, hz)));
}

void ColibriDevice::setPreampDb(double db)
{
    m_preampDb = db;
    if (m_dev)
        ColibriLib::instance().setPreamp(m_dev, static_cast<float>(db));
}

}  // namespace AetherSDR::colibri
