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

    // HOW the operator expressed that address, which decides whether the wire
    // is allowed to overrule it. Three states, not two, because "A2" means
    // different things depending on where it was typed:
    //
    //   Auto    nobody chose. Seed from the model the handshake names, then
    //           adopt whatever answers the broadcast 0x19 0x00.
    //   Model   the operator picked a model from the list. That is a SHORTCUT
    //           for an address, not a device selection, so a radio that reports
    //           a different address is correcting a stale pick and wins.
    //   Custom  the operator typed a hex address. On a shared CI-V bus — Icom's
    //           own RS-BA1 server can front one — that is the operator SELECTING
    //           WHICH DEVICE to talk to, so it must survive contact with a
    //           broadcast reply from some other device on the same bus.
    //
    // Collapsing Model and Custom loses exactly one of those two behaviours, and
    // which one you lose is not a matter of taste: keeping Custom overridable
    // breaks device selection, and keeping Model pinned breaks the operator who
    // changed the address on the radio after picking its model here.
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
