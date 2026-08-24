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
//         "serialPort":int, "audioPort":int, "civAddress":int,
//         "civSelection":"auto"|"model"|"custom"}
//
// Deliberately NO password field. See the header.
const QString kRootKey = QStringLiteral("Icom");

constexpr const char* kFieldUsername    = "username";
constexpr const char* kFieldLastHost    = "lastHost";
constexpr const char* kFieldControlPort = "controlPort";
constexpr const char* kFieldSerialPort  = "serialPort";
constexpr const char* kFieldAudioPort   = "audioPort";
constexpr const char* kFieldCivAddress  = "civAddress";
// WHICH of the three ways the address was chosen — see IcomSettings::CivSelection.
// Absent means a pre-existing settings file, which civSelection() migrates.
constexpr const char* kFieldCivSelection = "civSelection";
constexpr const char* kSelAuto   = "auto";
constexpr const char* kSelModel  = "model";
constexpr const char* kSelCustom = "custom";

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
    const int v = readObj().value(QLatin1String(kFieldCivAddress)).toInt(IcomSettings::kDefaultCivAddress);
    if (v <= 0 || v > 0xFF)
        return IcomSettings::kDefaultCivAddress;
    return static_cast<std::uint8_t>(v);
}

IcomSettings::CivSelection IcomSettings::civSelection()
{
    const QJsonObject obj = readObj();
    const QString sel = obj.value(QLatin1String(kFieldCivSelection)).toString();
    if (sel == QLatin1String(kSelAuto))
        return CivSelection::Auto;
    if (sel == QLatin1String(kSelModel))
        return CivSelection::Model;
    if (sel == QLatin1String(kSelCustom))
        return CivSelection::Custom;

    // MIGRATION — a settings file written before this field existed.
    //
    // Back then the connect panel had one free-text hex box, and it wrote
    // kDefaultCivAddress whenever that box was left BLANK. So a stored 0xA4 is
    // very nearly always "the operator never touched this", not "the operator
    // deliberately chose the IC-705": choosing it required typing A4 into a
    // field whose placeholder already said the default was A4.
    //
    // Reading it as Auto is also the safe direction of the two. On an actual
    // IC-705 auto-detect resolves to 0xA4 and nothing changes; on any other
    // radio it repairs the exact silent-dead-session this feature exists to
    // cure. Reading it as a pin would carry that dead session forward.
    //
    // Anything OTHER than the default had to be typed, so it migrates to Custom
    // and keeps pinning the destination.
    if (!obj.contains(QLatin1String(kFieldCivAddress)))
        return CivSelection::Auto;
    return civAddress() == kDefaultCivAddress ? CivSelection::Auto : CivSelection::Custom;
}

void IcomSettings::setCivAddressAuto()
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldCivSelection)] = QLatin1String(kSelAuto);
    // The address is left where it is ON PURPOSE. Switching to Auto and back to
    // Custom in one sitting should not silently discard the value that was
    // typed, and nothing reads it while the selection says Auto.
    writeObj(obj);
}

void IcomSettings::setCivAddressFromModel(std::uint8_t address)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldCivAddress)] = int(address);
    obj[QLatin1String(kFieldCivSelection)] = QLatin1String(kSelModel);
    writeObj(obj);
}

void IcomSettings::setCivAddress(std::uint8_t address)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldCivAddress)] = int(address);
    obj[QLatin1String(kFieldCivSelection)] = QLatin1String(kSelCustom);
    writeObj(obj);
}

void IcomSettings::reset()
{
    writeObj({});
}

}  // namespace AetherSDR
