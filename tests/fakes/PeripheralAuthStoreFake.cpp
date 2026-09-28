#include "PeripheralAuthStoreFake.h"
#include "gui/PeripheralAuthStore.h"
#include "core/PeripheralAuthCodeValidation.h"

#include <QHostAddress>
#include <QObject>
#include <QTimer>
#include <array>
#include <cstddef>
#include <utility>

namespace AetherSDR {
namespace {
struct Entry {
    QString endpoint;
    QString code;
};
std::array<Entry, 3> entries;
bool nextClearOk = true;
}

void FakePeripheralAuthStore::setNextClearResult(bool ok)
{
    nextClearOk = ok;
}

QString PeripheralAuthStore::endpoint(const QString& peerAddress, quint16 port)
{
    QHostAddress address;
    if (port == 0 || !address.setAddress(peerAddress)) {
        return {};
    }
    return address.toString() + QLatin1Char(':') + QString::number(port);
}

void PeripheralAuthStore::load(Device device, const QString& endpoint, QObject* context,
                               std::function<void(const LoadResult&)> callback)
{
    if (!context) {
        return;
    }
    const Entry& entry = entries.at(static_cast<std::size_t>(device));
    const LoadResult result = endpoint == entry.endpoint && !entry.code.isEmpty()
        ? LoadResult{entry.code, LoadStatus::Found}
        : LoadResult{{}, LoadStatus::Missing};
    QTimer::singleShot(0, context, [callback = std::move(callback), result]() {
        callback(result);
    });
}

void PeripheralAuthStore::save(Device device, const QString& endpoint, const QString& code,
                               QObject* context, std::function<void(bool)> callback)
{
    bool ok = true;
    if (code.isEmpty()) {
        ok = nextClearOk;
        nextClearOk = true;
        if (ok) {
            entries.at(static_cast<std::size_t>(device)) = {};
        }
    } else if (endpoint.isEmpty() || !validPeripheralAuthCode(code)) {
        ok = false;
    } else {
        entries.at(static_cast<std::size_t>(device)) = {endpoint, code};
    }
    if (context && callback) {
        QTimer::singleShot(0, context, [callback = std::move(callback), ok]() {
            callback(ok);
        });
    }
}

bool PeripheralAuthStore::persistentStoreAvailable()
{
    return true;
}

bool PeripheralAuthStore::validCode(const QString& code)
{
    return validPeripheralAuthCode(code);
}

} // namespace AetherSDR
