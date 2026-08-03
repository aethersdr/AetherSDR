#pragma once
#include <QtGlobal>
#include "INativeVideoWriter.h"

#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <mfapi.h>
#  include <mfidl.h>
#  include <mfreadwrite.h>
#endif

namespace AetherSDR {

class WmfVideoWriter : public INativeVideoWriter {
public:
    WmfVideoWriter();
    ~WmfVideoWriter() override;

    bool open(const QString& filePath, int width, int height, int fps) override;
    void close() override;
    bool writeVideoFrame(const QImage& frame, qint64 ptsUs) override;
    bool writeAudioSamples(const QByteArray& pcmData, qint64 ptsUs) override;

private:
    // Minimal scoped COM pointer: the setup path has many early-return failure
    // branches and hand-written Release() calls leaked on every one of them.
    template <typename T>
    class ComScoped {
    public:
        ComScoped() = default;
        ~ComScoped() { reset(); }
        ComScoped(const ComScoped&) = delete;
        ComScoped& operator=(const ComScoped&) = delete;

        T** operator&() { reset(); return &m_ptr; }
        T* operator->() const { return m_ptr; }
        T* get() const { return m_ptr; }
        explicit operator bool() const { return m_ptr != nullptr; }
        void reset()
        {
            if (m_ptr) {
                m_ptr->Release();
                m_ptr = nullptr;
            }
        }

    private:
        T* m_ptr{nullptr};
    };

    static constexpr DWORD kInvalidStreamIndex = 0xFFFFFFFFu;

    IMFSinkWriter* m_sinkWriter{nullptr};
    DWORD m_videoStreamIndex{kInvalidStreamIndex};
    DWORD m_audioStreamIndex{kInvalidStreamIndex};
    int m_width{0};
    int m_height{0};
    int m_fps{0};
    bool m_initialized{false};
    bool m_comInitializedByUs{false};
    bool m_mfStarted{false};
};

} // namespace AetherSDR
