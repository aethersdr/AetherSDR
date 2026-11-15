#include "Ctr2MaxRegistration.h"

#include <QMutex>
#include <QMutexLocker>

#include <cstring>

extern "C" {
#include "ctr2max_reg.h"
}

namespace AetherSDR::Ctr2MaxRegistration {

namespace {

int readBytes(void* ctx, uint32_t offset, void* buf, uint32_t len)
{
    const auto* bytes = static_cast<const QByteArray*>(ctx);
    if (quint64(offset) + len > quint64(bytes->size()))
        return -1;
    std::memcpy(buf, bytes->constData() + offset, len);
    return 0;
}

} // namespace

Result check(const QByteArray& partition, QString* call)
{
    // ctr2max_registration() keeps its state in statics (it is shared with
    // the firmware, which has little stack): one check at a time.
    static QMutex lock;
    const QMutexLocker locker(&lock);
    char buf[80] = {};   // the hashed call is 64 characters
    // The reader only reads through its context.
    void* ctx = const_cast<QByteArray*>(&partition);
    switch (ctr2max_registration(readBytes, ctx, static_cast<uint32_t>(partition.size()), buf,
                                 sizeof buf)) {
    case CTR2MAX_REGISTERED:
        if (call)
            *call = QString::fromLatin1(buf);
        return Result::Registered;
    case CTR2MAX_NOT_REGISTERED:
        return Result::NotRegistered;
    default:
        return Result::Unreadable;
    }
}

} // namespace AetherSDR::Ctr2MaxRegistration
