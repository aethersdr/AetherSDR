#include "core/backends/icom/IcomSettings.h"

#include "core/AppSettings.h"
#include "core/backends/icom/IcomProtocol.h"

#include <QJsonDocument>
#include <QtGlobal>
#include <QJsonObject>

namespace AetherSDR {
namespace {

// Single nested-JSON key holding this backend's config (Principle V).
// Shape: {"username":string, "lastHost":string, "controlPort":int,
//         "serialPort":int, "audioPort":int, "civAddress":int}
//
// Deliberately NO password field. See the header.
const QString kRootKey = QStringLiteral("Icom");

constexpr const char* kFieldUsername    = "username";
constexpr const char* kFieldLastHost    = "lastHost";
constexpr const char* kFieldControlPort = "controlPort";
constexpr const char* kFieldSerialPort  = "serialPort";
constexpr const char* kFieldAudioPort   = "audioPort";
constexpr const char* kFieldCivAddress  = "civAddress";

// The IC-705's factory CI-V address. A seed, not an assertion — the backend
// replaces it with whatever 0x19 0x00 reports.
constexpr int kDefaultCivAddress = 0xA4;

// Validate a port on the way OUT, not just on the way in. A hand-edited or
// truncated settings file must not be able to command a nonsense port
// (Principle VII); 0 in particular would bind an ephemeral local port and then
// tell the radio to reply to the wrong one.
quint16 portOr(const QJsonObject& obj, const char* field, quint16 fallback)
{
    const int v = obj.value(QLatin1String(field)).toInt(0);
    if (v <= 0 || v > 65535)
        return fallback;
    return static_cast<quint16>(v);
}

}  // namespace

QJsonObject IcomSettings::readObj()
{
    const QString json = AppSettings::instance().value(kRootKey, QString{}).toString();
    if (json.isEmpty())
        return {};
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

void IcomSettings::writeObj(const QJsonObject& obj)
{
    auto& s = AppSettings::instance();
    s.setValue(kRootKey,
               QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    // COMMIT. setValue() only inserts into the in-memory map and marks the row
    // dirty; save() is what runs the transaction, and there is no autosave
    // timer. Without this the host, user name and ports reach disk only when
    // some unrelated caller happens to save() later in the session — which is
    // why this looked like it worked. Hl2Settings::setSpanMhz, the sibling this
    // is modelled on, calls both.
    s.save();
}

QString IcomSettings::username()
{
    const QString stored = readObj().value(QLatin1String(kFieldUsername)).toString();
    if (!stored.isEmpty())
        return stored;
    // Headless fallback, symmetric with IcomCredentials' password env var: an
    // automation launch on a fresh profile has nothing stored and no dialog to
    // type into, so `connect ip <host> icom` would fail on a missing user name
    // even with the password supplied.
    return qEnvironmentVariable("AETHER_ICOM_USERNAME");
}

void IcomSettings::setUsername(const QString& username)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldUsername)] = username;
    writeObj(obj);
}

QString IcomSettings::lastHost()
{
    return readObj().value(QLatin1String(kFieldLastHost)).toString();
}

void IcomSettings::setLastHost(const QString& host)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldLastHost)] = host;
    writeObj(obj);
}

quint16 IcomSettings::controlPort()
{
    return portOr(readObj(), kFieldControlPort, icom::kControlPort);
}

quint16 IcomSettings::serialPort()
{
    return portOr(readObj(), kFieldSerialPort, icom::kSerialPort);
}

quint16 IcomSettings::audioPort()
{
    return portOr(readObj(), kFieldAudioPort, icom::kAudioPort);
}

void IcomSettings::setPorts(quint16 control, quint16 serial, quint16 audio)
{
    QJsonObject obj = readObj();
    // Written together because they are configured together on the radio and a
    // half-applied change is a connect failure that names the wrong cause.
    obj[QLatin1String(kFieldControlPort)] = control ? int(control) : int(icom::kControlPort);
    obj[QLatin1String(kFieldSerialPort)]  = serial ? int(serial) : int(icom::kSerialPort);
    obj[QLatin1String(kFieldAudioPort)]   = audio ? int(audio) : int(icom::kAudioPort);
    writeObj(obj);
}

std::uint8_t IcomSettings::civAddress()
{
    const int v = readObj().value(QLatin1String(kFieldCivAddress)).toInt(kDefaultCivAddress);
    if (v <= 0 || v > 0xFF)
        return kDefaultCivAddress;
    return static_cast<std::uint8_t>(v);
}

void IcomSettings::setCivAddress(std::uint8_t address)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldCivAddress)] = int(address);
    writeObj(obj);
}

void IcomSettings::reset()
{
    writeObj({});
}

}  // namespace AetherSDR
