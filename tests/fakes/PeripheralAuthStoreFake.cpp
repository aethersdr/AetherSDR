#include "PeripheralAuthStoreFake.h"
#include "gui/PeripheralAuthStore.h"
#include "core/PeripheralAuthCodeValidation.h"

#include <QHostAddress>
#include <QObject>
#include <QPointer>
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
std::array<bool, 3> deleting{};
bool nextClearOk = true;
bool backendAvailable = true;
bool clearDeferred = false;
FakePeripheralAuthStore::ReadFailure nextReadFailure = FakePeripheralAuthStore::ReadFailure::None;
std::function<void()> pendingClear;
}

void FakePeripheralAuthStore::setNextClearResult(bool ok)
{
    nextClearOk = ok;
}

void FakePeripheralAuthStore::setBackendAvailable(bool available)
{
    backendAvailable = available;
}

void FakePeripheralAuthStore::setNextReadFailure(ReadFailure failure)
{
    nextReadFailure = failure;
}

void FakePeripheralAuthStore::deferClear(bool defer)
{
    clearDeferred = defer;
}

void FakePeripheralAuthStore::finishClear()
{
    clearDeferred = false;
    const auto finish = std::exchange(pendingClear, {});
    if (finish) {
        finish();
    }
}

QString PeripheralAuthStore::endpoint(const QString& configuredHost,
                                      const QString& peerAddress, quint16 port)
{
    QHostAddress peer;
    if (port == 0 || !peer.setAddress(peerAddress)) {
        return {};
    }
    const QString host = configuredHost.trimmed();
    QHostAddress literal;
    if (host.isEmpty() || literal.setAddress(host)) {
        return peer.toString() + QLatin1Char(':') + QString::number(port);
    }
    return QStringLiteral("host:") + host.toLower() + QLatin1Char(':') + QString::number(port);
}

QString PeripheralAuthStore::configuredEndpoint(const QString& configuredHost, quint16 port)
{
    const QString host = configuredHost.trimmed();
    if (port == 0 || host.isEmpty()) {
        return {};
    }
    QHostAddress literal;
    if (literal.setAddress(host)) {
        return literal.toString() + QLatin1Char(':') + QString::number(port);
    }
    return QStringLiteral("host:") + host.toLower() + QLatin1Char(':') + QString::number(port);
}

void PeripheralAuthStore::load(Device device, const QString& endpoint, QObject* context,
                               std::function<void(const LoadResult&)> callback)
{
    if (!context) {
        return;
    }
    QTimer::singleShot(0, context, [callback = std::move(callback), device, endpoint]() {
        const std::size_t index = static_cast<std::size_t>(device);
        const Entry& entry = entries.at(index);
        const LoadResult result = !deleting.at(index) && endpoint == entry.endpoint && !entry.code.isEmpty()
            ? LoadResult{entry.code, LoadStatus::Found}
            : LoadResult{{}, LoadStatus::Missing};
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

void PeripheralAuthStore::clear(Device device, QObject* context,
                                 std::function<void(ClearResult)> callback)
{
    deleting.at(static_cast<std::size_t>(device)) = true;
    if (clearDeferred) {
        pendingClear = [device, guard = QPointer<QObject>(context), callback = std::move(callback)]() mutable {
            clear(device, guard.data(), std::move(callback));
        };
        return;
    }
    deleting.at(static_cast<std::size_t>(device)) = false;
    if (!backendAvailable) {
        entries.at(static_cast<std::size_t>(device)) = {};
        if (context && callback) {
            QTimer::singleShot(0, context, [callback = std::move(callback)]() {
                callback(ClearResult::SessionCleared);
            });
        }
        return;
    }
    save(device, {}, {}, context, [callback = std::move(callback)](bool ok) {
        if (callback) {
            callback(ok ? ClearResult::Cleared : ClearResult::Failed);
        }
    });
}

void PeripheralAuthStore::clearForEndpoint(Device device, const QString& endpoint,
                                           QObject* context,
                                           std::function<void(ClearResult)> callback)
{
    const auto failure = std::exchange(nextReadFailure, FakePeripheralAuthStore::ReadFailure::None);
    if (failure != FakePeripheralAuthStore::ReadFailure::None) {
        const ClearResult result = failure == FakePeripheralAuthStore::ReadFailure::Unavailable
            ? ClearResult::SessionCleared : ClearResult::Failed;
        if (result == ClearResult::SessionCleared) {
            entries.at(static_cast<std::size_t>(device)) = {};
        }
        QTimer::singleShot(0, context, [callback, result]() { callback(result); });
        return;
    }
    const Entry& entry = entries.at(static_cast<std::size_t>(device));
    if (!entry.code.isEmpty() && (endpoint.isEmpty() || entry.endpoint != endpoint)) {
        const ClearResult result = endpoint.isEmpty() ? ClearResult::UnknownOwner : ClearResult::Cleared;
        QTimer::singleShot(0, context, [callback, result]() { callback(result); });
        return;
    }
    clear(device, context, std::move(callback));
}

std::optional<PeripheralAuthStore::CodeAvailability> PeripheralAuthStore::cachedStatus(
    Device device, const QString& endpoint)
{
    if (endpoint.isEmpty()) {
        return std::nullopt;
    }
    const Entry& entry = entries.at(static_cast<std::size_t>(device));
    const bool found = !deleting.at(static_cast<std::size_t>(device))
        && endpoint == entry.endpoint && !entry.code.isEmpty();
    return CodeAvailability{found ? LoadStatus::Found : LoadStatus::Missing, found};
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
