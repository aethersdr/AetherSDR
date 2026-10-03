#pragma once

#include <QString>

#include <cstdint>

class QJsonObject;

namespace AetherSDR {

// Owned configuration for the Icom backend: one nested JSON object under the
// root key "Icom", read and written atomically, with one place to default.
// THE PASSWORD IS NOT HERE (RFC #4603 E): IcomCredentials owns it in
// QtKeychain. ("Icom", "Password") is registered in
// SettingsCredentialPolicy::kDocFieldCredentials, so AppSettings strips it and
// SettingsSanitizer redacts it if ever written. The protocol only obfuscates
// the password with a reversible substitution table.
class IcomSettings {
public:
    // The operator's network username on the radio. NOT a secret — the radio
    // pairs it with a password and the username alone grants nothing.
    static QString username();
    static void setUsername(const QString& username);

    // Client-owned, opt-in connection policy, stored in the Icom document.
    static bool wakeOnConnect();
    static void setWakeOnConnect(bool enabled);

    // The last host connected to, so the connect dialog can offer it back.
    static QString lastHost();
    static void setLastHost(const QString& host);

    // The three UDP ports. Defaults are Icom's, and all three are
    // operator-changeable ON THE RADIO — which is why they are settings rather
    // than constants. A mismatch presents as a connect timeout that names the
    // wrong cause, so the dialog exposes them.
    static quint16 controlPort();
    static quint16 serialPort();
    static quint16 audioPort();
    static void setPorts(quint16 control, quint16 serial, quint16 audio);

    // Connect-by-IP NAT convenience: the public forwards are one sequential
    // triplet (control, CI-V, audio). The highest valid base is 65533 so the
    // derived audio port remains inside the UDP port range.
    static quint16 defaultBasePort();
    static quint16 maximumBasePort();
    static bool usesDefaultPorts();
    static void setBasePort(quint16 basePort);

    // The radio's CI-V address. Seeded here and CORRECTED at runtime from the
    // 0x19 0x00 reply — never trusted as final, because the address is
    // user-changeable and several Icom models speak this same transport.
    static std::uint8_t civAddress();

    // How the operator expressed the CI-V address, deciding whether the wire may
    // overrule it:
    //   Auto    nobody chose: query 19 00 at broadcast, adopt the sole responder.
    //   Model   picked from the model list — a shortcut for an address, so a radio
    //           reporting a different address corrects it and wins.
    //   Custom  typed hex — on a shared CI-V bus (e.g. behind Icom's RS-BA1 server)
    //           this SELECTS THE DEVICE and must survive other devices' replies.
    // Model and Custom can't be merged: each needs the opposite override rule.
    enum class CivSelection { Auto, Model, Custom };
    static CivSelection civSelection();

    // Auto: no address is carried to the backend at all.
    static void setCivAddressAuto();
    // Model: `address` came from knownModels(), so the wire may correct it.
    static void setCivAddressFromModel(std::uint8_t address);
    // Custom: `address` was typed, so it pins the destination.
    static void setCivAddress(std::uint8_t address);

    // Exposed so the connect UI can tell "the operator chose this" from "nobody
    // has set one", and leave its field blank in the second case rather than
    // presenting the IC-705's address as a deliberate choice.
    static constexpr std::uint8_t kDefaultCivAddress = 0xA4;

    // Restore every field to its default.
    static void reset();

private:
    static QJsonObject readObj();
    static void writeObj(const QJsonObject& obj);
};

}  // namespace AetherSDR
