#pragma once

#include <QLibrary>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>

namespace AetherSDR::colibri {

// The C ABI of Expert Electronics' colibrinano_lib, verbatim from the vendor's
// LibLoader.h / common.h (the protocol authority — see
// docs/architecture/aetherd-colibri-backend-design.md). These types appear only
// under src/core/backends/colibri/ (EB3).
struct ColibriComplex {
    float re;
    float im;
};
using ColibriDescriptor = void*;

#ifdef Q_OS_WIN
#define COLIBRI_CALL __stdcall
#else
#define COLIBRI_CALL
#endif

// bool return: false tells the library to stop delivering. adcOverload rides
// every block rather than being a separate status call.
using ColibriRxCallback = bool (COLIBRI_CALL*)(ColibriComplex* iq, std::uint32_t len,
                                               bool adcOverload, void* user);

// The nine rates the receiver offers, by the index start() takes. ONE list —
// it is simultaneously the capability advertisement, the pan zoom limits and
// the set a zoom request snaps to, for the same reason HL2 keeps one list: the
// pan span IS the sample rate on this radio.
inline constexpr int kColibriSampleRatesHz[] = {
    48000, 96000, 192000, 384000, 768000, 1536000, 1920000, 2560000, 3072000,
};

// start()'s SampleRateIndex for a rate from the list above, or -1.
int colibriSampleRateIndex(int hz) noexcept;

// Process-wide handle onto colibrinano_lib. A singleton because the library
// itself is process-global state: initialize() must run exactly once, and the
// discovery poll (GUI thread) and the device (I/O thread) must share the one
// loaded instance rather than racing two LoadLibrary calls.
//
// Control calls are serialized by a mutex. The RX callback does NOT pass
// through this class at all — the library invokes it directly on its own
// thread — so the lock never sits on the sample path.
class ColibriLib {
public:
    static ColibriLib& instance();

    // Load + initialize, idempotent. `explicitPath` (from settings/params)
    // is tried first, then the executable directory, then the vendor's
    // default install locations. Returns false with `error` set when no
    // loadable library was found.
    bool ensureLoaded(const QString& explicitPath, QString* error);
    [[nodiscard]] bool isLoaded() const { return m_initialized; }
    [[nodiscard]] QString libraryPath() const { return m_path; }

    void version(std::uint32_t& major, std::uint32_t& minor, std::uint32_t& patch);
    QString information();
    std::uint32_t deviceCount();
    bool open(ColibriDescriptor* out, std::uint32_t index);
    void close(ColibriDescriptor dev);
    bool start(ColibriDescriptor dev, int sampleRateIndex, ColibriRxCallback cb, void* user);
    bool stop(ColibriDescriptor dev);
    bool setPreamp(ColibriDescriptor dev, float db);
    bool setFrequency(ColibriDescriptor dev, std::uint32_t hz);

    // True while any ColibriDevice holds an open descriptor. The discovery
    // poll checks this and SKIPS enumerating: the authority does not document
    // FTDI bus enumeration as safe under a running stream, so we do not find
    // out on the operator's receiver.
    void setDeviceInUse(bool inUse) { m_deviceInUse.store(inUse); }
    [[nodiscard]] bool deviceInUse() const { return m_deviceInUse.load(); }

private:
    ColibriLib() = default;

    using FnVoid = void (COLIBRI_CALL*)();
    using FnVersion = void (COLIBRI_CALL*)(std::uint32_t&, std::uint32_t&, std::uint32_t&);
    using FnInformation = void (COLIBRI_CALL*)(char**);
    using FnDevices = void (COLIBRI_CALL*)(std::uint32_t&);
    using FnOpen = bool (COLIBRI_CALL*)(ColibriDescriptor*, std::uint32_t);
    using FnClose = void (COLIBRI_CALL*)(ColibriDescriptor);
    using FnStart = bool (COLIBRI_CALL*)(ColibriDescriptor, int, ColibriRxCallback, void*);
    using FnStop = bool (COLIBRI_CALL*)(ColibriDescriptor);
    using FnSetPreamp = bool (COLIBRI_CALL*)(ColibriDescriptor, float);
    using FnSetFrequency = bool (COLIBRI_CALL*)(ColibriDescriptor, std::uint32_t);

    static QStringList candidatePaths(const QString& explicitPath);

    QLibrary m_lib;
    QString m_path;
    QMutex m_mutex;
    bool m_initialized = false;
    std::atomic<bool> m_deviceInUse{false};

    FnVoid m_initialize = nullptr;
    FnVoid m_finalize = nullptr;          // resolved but never called — see design doc
    FnVersion m_version = nullptr;
    FnInformation m_information = nullptr;
    FnDevices m_devices = nullptr;
    FnOpen m_open = nullptr;
    FnClose m_close = nullptr;
    FnStart m_start = nullptr;
    FnStop m_stop = nullptr;
    FnSetPreamp m_setPreamp = nullptr;
    FnSetFrequency m_setFrequency = nullptr;
};

}  // namespace AetherSDR::colibri
