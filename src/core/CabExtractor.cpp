#include "CabExtractor.h"
#include "LogManager.h"

extern "C" {
#include <mspack.h>
}

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

namespace AetherSDR {

namespace {

constexpr qint64 kMaxExtractedBytes = 500LL * 1024LL * 1024LL;

// Per-file state passed back to libmspack callbacks. mspack treats this
// as opaque — we cast to our concrete type in each callback.
struct MemFile {
    const QByteArray* readBuf{nullptr};   // non-null when this file is input
    QByteArray*       writeBuf{nullptr};  // non-null when this file is output
    qint64            offset{0};
    qint64            maxBytes{-1};
};

// Our mspack_system extension. mspack_system is the first member so a
// pointer to one can be safely cast to/from our struct.
struct MemSystem {
    mspack_system base;
    const QByteArray* cabSource{nullptr};  // cab input, set before open()
    QByteArray*       payloadSink{nullptr}; // payload output, set before extract()
    qint64            maxOutputBytes{-1};
};

// ─── mspack_system callbacks ────────────────────────────────────────────────

mspack_file* mem_open(mspack_system* self, const char* /*filename*/, int mode)
{
    auto* sys = reinterpret_cast<MemSystem*>(self);
    auto* mf = new MemFile;
    if (mode == MSPACK_SYS_OPEN_READ) {
        if (!sys->cabSource) {
            delete mf;
            return nullptr;
        }
        mf->readBuf = sys->cabSource;
    } else if (mode == MSPACK_SYS_OPEN_WRITE) {
        if (!sys->payloadSink) {
            delete mf;
            return nullptr;
        }
        mf->writeBuf = sys->payloadSink;
        mf->maxBytes = sys->maxOutputBytes;
    } else {
        // We don't support UPDATE or APPEND — cab extraction never asks for them.
        delete mf;
        return nullptr;
    }
    return reinterpret_cast<mspack_file*>(mf);
}

void mem_close(mspack_file* file)
{
    delete reinterpret_cast<MemFile*>(file);
}

int mem_read(mspack_file* file, void* buffer, int bytes)
{
    auto* mf = reinterpret_cast<MemFile*>(file);
    if (!mf->readBuf) return -1;
    const qint64 remaining = mf->readBuf->size() - mf->offset;
    const qint64 n = qMin<qint64>(remaining, bytes);
    if (n <= 0) return 0;
    std::memcpy(buffer, mf->readBuf->constData() + mf->offset, static_cast<size_t>(n));
    mf->offset += n;
    return static_cast<int>(n);
}

int mem_write(mspack_file* file, void* buffer, int bytes)
{
    auto* mf = reinterpret_cast<MemFile*>(file);
    if (!mf->writeBuf) return -1;
    if (bytes <= 0) return 0;
    if (mf->maxBytes >= 0 && bytes > mf->maxBytes - mf->writeBuf->size()) {
        return -1;
    }
    mf->writeBuf->append(static_cast<const char*>(buffer), bytes);
    mf->offset += bytes;
    return bytes;
}

int mem_seek(mspack_file* file, off_t offset, int mode)
{
    auto* mf = reinterpret_cast<MemFile*>(file);
    if (!mf->readBuf) return -1;
    qint64 newOffset = mf->offset;
    switch (mode) {
        case MSPACK_SYS_SEEK_START: newOffset = offset; break;
        case MSPACK_SYS_SEEK_CUR:   newOffset += offset; break;
        case MSPACK_SYS_SEEK_END:   newOffset = mf->readBuf->size() + offset; break;
        default: return -1;
    }
    if (newOffset < 0 || newOffset > mf->readBuf->size())
        return -1;
    mf->offset = newOffset;
    return 0;
}

off_t mem_tell(mspack_file* file)
{
    return reinterpret_cast<MemFile*>(file)->offset;
}

void mem_message(mspack_file* /*file*/, const char* format, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    qCWarning(lcFirmware) << "libmspack:" << buf;
}

void* mem_alloc(mspack_system* /*self*/, size_t bytes)
{
    return std::malloc(bytes);
}

void mem_free(void* ptr)
{
    std::free(ptr);
}

void mem_copy(void* src, void* dest, size_t bytes)
{
    std::memcpy(dest, src, bytes);
}

MemSystem memorySystem(const QByteArray& source)
{
    MemSystem sys{};
    sys.base.open = &mem_open;
    sys.base.close = &mem_close;
    sys.base.read = &mem_read;
    sys.base.write = &mem_write;
    sys.base.seek = &mem_seek;
    sys.base.tell = &mem_tell;
    sys.base.message = &mem_message;
    sys.base.alloc = &mem_alloc;
    sys.base.free = &mem_free;
    sys.base.copy = &mem_copy;
    sys.cabSource = &source;
    return sys;
}

} // anonymous namespace

bool CabExtractor::extractAll(const QByteArray& cabBytes,
                               QList<QPair<QString, QByteArray>>& out)
{
    out.clear();
    m_lastError.clear();
    if (cabBytes.isEmpty()) {
        m_lastError = "empty cab buffer";
        return false;
    }

    MemSystem sys = memorySystem(cabBytes);
    // sys.payloadSink is rebound per-file inside the loop below.

    auto* cabd = mspack_create_cab_decompressor(&sys.base);
    if (!cabd) {
        m_lastError = "mspack_create_cab_decompressor failed";
        return false;
    }

    auto* cab = cabd->open(cabd, const_cast<char*>("<cab>"));
    if (!cab) {
        m_lastError = QString("cabd->open: error %1").arg(cabd->last_error(cabd));
        mspack_destroy_cab_decompressor(cabd);
        return false;
    }

    if (!cab->files) {
        m_lastError = "cabinet has no files";
        cabd->close(cabd, cab);
        mspack_destroy_cab_decompressor(cabd);
        return false;
    }

    qint64 totalBytes = 0;
    for (auto* file = cab->files; file; file = file->next) {
        if (file->length > kMaxExtractedBytes - totalBytes) {
            m_lastError = "Cabinet output exceeds size limit";
            cabd->close(cabd, cab);
            mspack_destroy_cab_decompressor(cabd);
            return false;
        }
        totalBytes += file->length;
    }
    for (auto* file = cab->files; file; file = file->next) {
        QByteArray payload;
        sys.payloadSink = &payload;
        sys.maxOutputBytes = file->length;
        const int rc = cabd->extract(cabd, file, const_cast<char*>("<out>"));
        if (rc != MSPACK_ERR_OK || payload.size() != file->length) {
            m_lastError = QString("cabd->extract(%1): error %2")
                .arg(QString::fromLocal8Bit(file->filename ? file->filename : "?"))
                .arg(rc);
            out.clear();
            cabd->close(cabd, cab);
            mspack_destroy_cab_decompressor(cabd);
            return false;
        }
        out.append({QString::fromLocal8Bit(file->filename ? file->filename : ""),
                    std::move(payload)});
    }

    cabd->close(cabd, cab);
    mspack_destroy_cab_decompressor(cabd);
    return true;
}

bool CabExtractor::extractFile(const QByteArray& cabBytes, const QString& memberName,
                              qint64 expectedSize, QByteArray& out)
{
    out.clear();
    m_lastError.clear();
    if (cabBytes.isEmpty() || memberName.isEmpty() || expectedSize <= 0
        || expectedSize > kMaxExtractedBytes) {
        m_lastError = "Invalid firmware cabinet request";
        return false;
    }
    MemSystem sys = memorySystem(cabBytes);
    sys.maxOutputBytes = expectedSize;
    const auto destroy = [](mscab_decompressor* decoder) { mspack_destroy_cab_decompressor(decoder); };
    const std::unique_ptr<mscab_decompressor, decltype(destroy)> decoder(
        mspack_create_cab_decompressor(&sys.base), destroy);
    if (!decoder) {
        m_lastError = "Cannot create cabinet decompressor";
        return false;
    }
    const auto close = [&decoder](mscabd_cabinet* cabinet) { decoder->close(decoder.get(), cabinet); };
    const std::unique_ptr<mscabd_cabinet, decltype(close)> cabinet(
        decoder->open(decoder.get(), const_cast<char*>("<cab>")), close);
    if (!cabinet) {
        m_lastError = "Cannot open firmware cabinet";
        return false;
    }
    mscabd_file* target = nullptr;
    for (mscabd_file* file = cabinet->files; file; file = file->next) {
        if (file->filename && QString::fromLatin1(file->filename) == memberName) {
            if (target) {
                m_lastError = "Duplicate firmware member in cabinet";
                return false;
            }
            target = file;
        }
    }
    if (!target || target->length != expectedSize) {
        m_lastError = "Firmware cabinet member is missing or has the wrong size";
        return false;
    }
    QByteArray payload;
    sys.payloadSink = &payload;
    const int result = decoder->extract(decoder.get(), target, const_cast<char*>("<out>"));
    if (result != MSPACK_ERR_OK || payload.size() != expectedSize) {
        m_lastError = QString("Firmware extraction failed (cabinet error %1)").arg(result);
        return false;
    }
    out = std::move(payload);
    return true;
}

bool CabExtractor::extractFirstMatchingMagic(const QByteArray& cabBytes,
                                              const QByteArray& magic,
                                              QByteArray& out)
{
    out.clear();
    QList<QPair<QString, QByteArray>> all;
    if (!extractAll(cabBytes, all))
        return false;
    for (auto& [name, data] : all) {
        if (data.startsWith(magic)) {
            out = std::move(data);
            return true;
        }
    }
    m_lastError = "no file in cab matched the requested magic";
    return false;
}

} // namespace AetherSDR
