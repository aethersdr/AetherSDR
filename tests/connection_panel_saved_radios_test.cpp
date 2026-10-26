// Socket-free: the title bar's saved radios come from the Connect window's
// saved manual profiles (RoutedProfilesJson). Each profile records the identity
// its connection proved (radio id, resolved address, name); checks that those
// are offered whatever the radio type, that the identified model is kept, and
// that choosing a saved radio opens Connect manually on its address.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RadioDiscovery.h"
#include "gui/ConnectionPanel.h"

#include <QApplication>
#include <QComboBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool ok, const char* claim)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", claim);
    failures += !ok;
}

QJsonObject profile(const QString& family, const QString& radioId, const QString& address,
                    const QString& name, const QString& model = QString())
{
    QJsonObject identity;
    identity["family"] = family;
    if (!radioId.isEmpty()) {
        identity["radio_id"] = radioId;
        identity["address"] = address;
        identity["name"] = name;
    }
    QJsonObject p;
    p["schema_version"] = 1;
    p["identity"] = identity;
    if (!model.isEmpty()) {
        p["model"] = model;
    }
    return p;
}

QJsonObject storedProfiles()
{
    return QJsonDocument::fromJson(AppSettings::instance()
        .value("RoutedProfilesJson", "{}").toString().toUtf8()).object();
}

const ConnectionPanel::SavedRadio* find(const QList<ConnectionPanel::SavedRadio>& radios,
                                        const QString& id)
{
    for (const auto& radio : radios) {
        if (radio.id == id) return &radio;
    }
    return nullptr;
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("connection-panel-saved-radios"));
    if (!settingsProfile.isValid())
        return 1;
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    const QString icomId = QStringLiteral("icom:192.168.1.110");
    QJsonObject profiles;
    profiles["192.168.1.110"] = profile("icom", icomId, "192.168.1.110", "Icom");
    // The same Icom reached by host name: saved again, under the same id.
    profiles["ic9700.local"] = profile("icom", icomId, "192.168.1.110", "Icom");
    profiles["192.168.1.120"] = profile("icom", "icom:192.168.1.120", "192.168.1.120", "Icom", "IC-705");
    profiles["192.168.1.130"] = profile("icom", "icom:192.168.1.130", "192.168.1.130", "Icom", "Unknown Icom");
    profiles["10.8.0.5"] = profile("flex", "10.8.0.5", "10.8.0.5", "FLEX");
    profiles["flex.lan"] = profile("flex", "flex.lan", "10.8.0.9", "FLEX");
    profiles["10.8.0.21"] = profile("hl2", "HL2-00-1C-C0-12-34-56", "10.8.0.21", "Hermes-Lite 2");
    profiles["10.8.0.99"] = profile("flex", QString(), QString(), QString());   // saved before identities
    AppSettings::instance().setValue("RoutedProfilesJson",
        QString::fromUtf8(QJsonDocument(profiles).toJson(QJsonDocument::Compact)));

    ConnectionPanel panel;

    // ── Which profiles are offered, under what id and name ──────────────────
    const auto saved = panel.savedRadios();
    const auto* icom = find(saved, icomId);
    check(icom && icom->name == QStringLiteral("Icom") && icom->address == QStringLiteral("192.168.1.110"),
          "a saved Icom is offered under the id, address and name its connection proved");
    int icomCount = 0;
    for (const auto& r : saved) icomCount += r.id == icomId;
    check(icomCount == 1, "a host name and its resolved address are one saved radio");
    check(find(saved, "icom:192.168.1.120") && find(saved, "icom:192.168.1.120")->name == QStringLiteral("IC-705"),
          "a stored model names the radio");
    check(find(saved, "icom:192.168.1.130") && find(saved, "icom:192.168.1.130")->name == QStringLiteral("Icom"),
          "a stored \"Unknown Icom\" names nothing; the connection's name is used");
    const auto* host = find(saved, "flex.lan");
    check(host && host->key == QStringLiteral("flex.lan") && host->address == QStringLiteral("10.8.0.9"),
          "a Flex saved under a host name keeps the host name to type and the address it resolved to");
    check(find(saved, "HL2-00-1C-C0-12-34-56") != nullptr,
          "a routed HL2 is offered like any other radio type");
    check(!find(saved, "10.8.0.99"), "a profile saved before identities were recorded is not offered");
    check(saved.size() == 6, "every profile with an identity is offered once");

    // ── The identified model is kept on the profile ─────────────────────────
    RadioInfo live;
    live.family = QStringLiteral("icom");
    live.serial = icomId;
    live.model = QStringLiteral("Icom");   // the manual probe's placeholder
    live.isRouted = true;
    check(!panel.rememberSavedRadioModel(live, QStringLiteral("Icom")),
          "the probe's placeholder is not reported as stored");
    check(!storedProfiles().value("192.168.1.110").toObject().contains("model"),
          "the probe's placeholder model is not stored");
    panel.rememberSavedRadioModel(live, QStringLiteral("Unknown Icom"));
    check(!storedProfiles().value("192.168.1.110").toObject().contains("model"),
          "an Icom's \"Unknown Icom\" (identity pending or failed) is not stored");
    check(panel.rememberSavedRadioModel(live, QStringLiteral("IC-9700")),
          "storing the identified model is reported");
    check(storedProfiles().value("192.168.1.110").toObject().value("model").toString() == QStringLiteral("IC-9700")
              && storedProfiles().value("ic9700.local").toObject().value("model").toString() == QStringLiteral("IC-9700"),
          "the model the radio reported is stored on every profile of that radio");
    check(panel.rememberSavedRadioModel(live, QStringLiteral("IC-9700")),
          "a model already stored is reported as held, so the caller stops asking");
    check(find(panel.savedRadios(), icomId)->name == QStringLiteral("IC-9700"),
          "the saved radio is named by the stored model");
    panel.rememberSavedRadioModel(live, QStringLiteral("Unknown Icom"));
    check(storedProfiles().value("192.168.1.110").toObject().value("model").toString() == QStringLiteral("IC-9700"),
          "a later session that fails to identify keeps the stored model");

    RadioInfo other = live;
    other.serial = QStringLiteral("icom:192.168.1.199");
    check(!panel.rememberSavedRadioModel(other, QStringLiteral("IC-7610")),
          "a radio no profile proved is reported as not stored");
    check(!storedProfiles().value("192.168.1.120").toObject().value("model").toString().contains("7610"),
          "a radio no profile proved writes nothing");
    RadioInfo discovered = live;
    discovered.isRouted = false;
    discovered.serial = QStringLiteral("icom:192.168.1.130");
    panel.rememberSavedRadioModel(discovered, QStringLiteral("IC-7300"));
    check(storedProfiles().value("192.168.1.130").toObject().value("model").toString() == QStringLiteral("Unknown Icom"),
          "a connection that did not come from a saved profile writes nothing");

    // ── Choosing a saved radio opens Connect manually on its address ────────
    auto* ip = panel.findChild<QLineEdit*>(QStringLiteral("connectionManualIp"));
    auto* family = panel.findChild<QComboBox*>(QStringLiteral("connectionManualRadioType"));
    check(ip && family, "the manual connection controls exist");
    check(find(panel.savedRadios(), icomId)->key == QStringLiteral("ic9700.local"),
          "a radio saved under a host name and its address is offered under the host name");
    check(panel.selectSavedRadio(icomId), "a saved radio's id is selectable");
    if (ip && family) {
        check(ip->text() == QStringLiteral("ic9700.local"),
              "the host name typed is filled in, not the address it resolved to");
        check(family->currentData().toString() == QStringLiteral("icom"),
              "its radio type is restored to Icom");
    }
    check(panel.selectSavedRadio(QStringLiteral("flex.lan")) && ip && ip->text() == QStringLiteral("flex.lan"),
          "a radio saved under a host name opens on the host name");
    check(panel.selectSavedRadio(QStringLiteral("4324-1213-8600-3564"), QStringLiteral("10.8.0.5"))
              && ip && ip->text() == QStringLiteral("10.8.0.5"),
          "a recent radio's tab opens the profile saved at its address");
    check(!panel.selectSavedRadio(QStringLiteral("icom:10.9.9.9")),
          "an id that is not a saved radio is refused");

    return failures ? 1 : 0;
}
