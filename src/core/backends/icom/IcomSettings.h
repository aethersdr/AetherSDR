#pragma once

#include <QString>

#include <cstdint>

class QJsonObject;

namespace AetherSDR {

// Owned configuration for the Icom networked-radio backend, per Constitution
// Principle V: one nested JSON object under a single root key ("Icom"), read
// and written atomically, with one place to default.
//
// THE PASSWORD IS NOT HERE, and its absence is the point.
//
// RFC #4603 proposal E: no credential is ever stored in the settings database —
// QtKeychain is the only persistent credential store. `IcomCredentials` owns
// the password. As defence in depth, ("Icom", "Password") is also registered in
// SettingsCredentialPolicy::kDocFieldCredentials, so if any future caller ever
// writes a Password field into this document, AppSettings strips it at the seam
// and SettingsSanitizer redacts it from exports and logs.
//
// That matters more here than for most credentials: the Icom protocol
// obfuscates the password with a fixed substitution table rather than
// encrypting it (see icom-oracle §2.5), so anyone with a packet capture on the
// LAN already has it. Writing it to a settings file that gets attached to bug
// reports would widen that exposure considerably, for no benefit.
class IcomSettings {
public:
    // The operator's network username on the radio. NOT a secret — the radio
    // pairs it with a password and the username alone grants nothing.
    static QString username();
    static void setUsername(const QString& username);

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

    // The radio's CI-V address. Seeded here and CORRECTED at runtime from the
    // 0x19 0x00 reply — never trusted as final, because the address is
    // user-changeable and several Icom models speak this same transport.
    static std::uint8_t civAddress();
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
