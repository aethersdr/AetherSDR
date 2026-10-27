// Pure-codec tests for the RF-KIT REST/JSON protocol layer.
//
// Two things matter here:
//
//  1. The documents are asserted against the shapes in the vendor's "RFKIT API
//     and SDK Documentation" (OpenAPI 0.9.0), not against this codec's own
//     output (constitution Principle I/IV).
//
//  2. These bytes come straight off a LAN peer (Principle VII): oversized
//     bodies, non-JSON, wrong shapes, out-of-range antennas and control enums
//     the codec cannot interpret must all yield nullopt rather than a
//     half-filled struct, and the request builders must refuse out-of-range
//     antennas so they never reach the wire.

#include "core/RfkitProtocol.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <cstdio>

using namespace AetherSDR::Rfkit;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

bool nearlyEqual(float a, float b, float tolerance = 0.001f)
{
    return std::fabs(a - b) <= tolerance;
}

const QByteArray kPowerJson =
    R"({"temperature":{"value":41.5,"unit":"°C"},"voltage":{"value":50.2,"unit":"V"},)"
    R"("current":{"value":12.3,"unit":"A"},"forward":{"value":1200,"max_value":1500,"unit":"W"},)"
    R"("reflected":{"value":30,"max_value":300,"unit":"W"},)"
    R"("swr":{"value":1.4,"max_value":10.0,"unit":""}})";

}  // namespace

int main()
{
    // ── /power ────────────────────────────────────────────────────────────
    {
        const auto p = parsePower(kPowerJson);
        report("parsePower decodes a well-formed document", p.has_value());
        if (p) {
            report("forward watts and the device's own full scale",
                   p->forward.value == 1200 && p->forward.maxValue == 1500 && p->forward.unit == "W");
            report("reflected watts and full scale", p->reflected.value == 30 && p->reflected.maxValue == 300);
            report("swr is a float", nearlyEqual(p->swr.value, 1.4f) && nearlyEqual(p->swr.maxValue, 10.0f));
            report("temperature/voltage/current decode",
                   nearlyEqual(p->temperature.value, 41.5f) && nearlyEqual(p->voltage.value, 50.2f)
                       && nearlyEqual(p->current.value, 12.3f));
            report("units are carried through", p->current.unit == "A" && p->voltage.unit == "V");
        }
    }
    {
        // max_value is descriptive: absent means "not reported" (0), not a failure.
        const auto p = parsePower(
            R"({"temperature":{"value":30,"unit":"C"},"voltage":{"value":50,"unit":"V"},)"
            R"("current":{"value":1,"unit":"A"},"forward":{"value":0,"unit":"W"},)"
            R"("reflected":{"value":0,"unit":"W"},"swr":{"value":1.0,"unit":""}})");
        report("a missing max_value decodes as 0 (not reported)",
               p.has_value() && p->forward.maxValue == 0 && p->reflected.maxValue == 0);
    }
    report("parsePower rejects a document missing a required block",
           !parsePower(R"({"forward":{"value":1,"max_value":2,"unit":"W"}})").has_value());
    report("parsePower rejects a non-numeric value",
           !parsePower(
               R"({"temperature":{"value":"hot","unit":"C"},"voltage":{"value":1,"unit":"V"},)"
               R"("current":{"value":1,"unit":"A"},"forward":{"value":1,"unit":"W"},)"
               R"("reflected":{"value":1,"unit":"W"},"swr":{"value":1,"unit":""}})").has_value());
    report("parsePower rejects non-JSON", !parsePower("<html>not an api</html>").has_value());
    report("parsePower rejects an empty body", !parsePower(QByteArray()).has_value());
    report("parsePower rejects a JSON array", !parsePower("[1,2,3]").has_value());
    {
        // Principle VII: a body larger than any endpoint can legitimately send
        // is dropped without parsing.
        QByteArray huge = kPowerJson;
        huge.append(QByteArray(kMaxBodyBytes, ' '));
        report("an oversized body is rejected", !parsePower(huge).has_value());
    }

    // ── /data ─────────────────────────────────────────────────────────────
    {
        const auto d = parseData(
            R"({"band":{"value":20,"unit":"m"},"frequency":{"value":14200,"unit":"kHz"},"status":"OK"})");
        report("parseData decodes band, frequency and status",
               d.has_value() && d->band.value == 20 && d->band.unit == "m"
                   && d->frequency.value == 14200 && d->status == "OK");
    }
    report("parseData rejects a document without a frequency",
           !parseData(R"({"band":{"value":20,"unit":"m"},"status":"OK"})").has_value());

    // ── /info ─────────────────────────────────────────────────────────────
    {
        const auto i = parseInfo(
            R"({"device":"RFKIT","software_version":{"GUI":12,"controller":34},"custom_device_name":"Shack amp"})");
        report("parseInfo decodes identity and versions",
               i.has_value() && i->device == "RFKIT" && i->customName == "Shack amp"
                   && i->guiVersion == 12 && i->controllerVersion == 34);
    }
    report("parseInfo rejects a document with no device name",
           !parseInfo(R"({"software_version":{"GUI":1,"controller":2}})").has_value());

    // ── /tuner ────────────────────────────────────────────────────────────
    {
        const auto t = parseTuner(
            R"({"mode":"AUTO","setup":"CL","L":{"value":12,"unit":"nH"},"C":{"value":340,"unit":"pF"},)"
            R"("tuned_frequency":{"value":14200,"unit":"kHz"},"segment_size":{"value":50,"unit":"kHz"}})");
        report("parseTuner decodes mode, setup and the L/C network",
               t.has_value() && t->mode == TunerMode::Auto && t->setup == TunerSetup::CL
                   && t->l && t->l->value == 12 && t->c && t->c->value == 340);
    }
    {
        const auto t = parseTuner(R"({"mode":"BYPASS"})");
        report("parseTuner needs only mode; the rest is optional",
               t.has_value() && t->mode == TunerMode::Bypass && t->setup == TunerSetup::Unknown
                   && !t->l && !t->c);
    }
    {
        // The OpenAPI enum lists the literal false beside the names.
        const auto asBool = parseTuner(R"({"mode":false})");
        const auto asString = parseTuner(R"({"mode":"false"})");
        report("the document's literal false decodes as tuner OFF (boolean and string)",
               asBool && asBool->mode == TunerMode::Off && asString && asString->mode == TunerMode::Off);
    }
    report("an unknown tuner mode is Unknown rather than a rejection",
           parseTuner(R"({"mode":"WARP"})")->mode == TunerMode::Unknown);
    report("parseTuner rejects a document with no mode", !parseTuner(R"({"setup":"CL"})").has_value());

    // ── /operate-mode ─────────────────────────────────────────────────────
    report("OPERATE decodes", parseOperateMode(R"({"operate_mode":"OPERATE"})") == OperateMode::Operate);
    report("STANDBY decodes", parseOperateMode(R"({"operate_mode":"STANDBY"})") == OperateMode::Standby);
    // A control state this codec cannot interpret is refused, never guessed.
    report("an unknown operate mode is rejected, not guessed to a default",
           !parseOperateMode(R"({"operate_mode":"BOOST"})").has_value());
    report("a lower-case operate mode is rejected (the enum is exact)",
           !parseOperateMode(R"({"operate_mode":"operate"})").has_value());
    report("a missing operate_mode is rejected", !parseOperateMode("{}").has_value());

    // ── /operational-interface ────────────────────────────────────────────
    {
        const auto s = parseInterface(R"({"operational_interface":"TCI","error":"link lost"})");
        report("parseInterface decodes the interface and the read-only error",
               s.has_value() && s->active == OperationalInterface::Tci && s->error == "link lost");
    }
    report("all four interfaces decode",
           parseInterface(R"({"operational_interface":"UNIV"})")->active == OperationalInterface::Univ
               && parseInterface(R"({"operational_interface":"CAT"})")->active == OperationalInterface::Cat
               && parseInterface(R"({"operational_interface":"UDP"})")->active == OperationalInterface::Udp);
    report("an unknown interface is rejected",
           !parseInterface(R"({"operational_interface":"RS232"})").has_value());

    // ── Antennas ──────────────────────────────────────────────────────────
    {
        const auto a = parseAntenna(R"({"type":"INTERNAL","number":3})");
        report("an internal antenna decodes",
               a.has_value() && a->type == AntennaType::Internal && a->number == 3);
    }
    {
        // A real RF2K-S lists the external entry with no number while it is
        // not active...
        const auto a = parseAntenna(R"({"type":"EXTERNAL"})");
        report("the un-numbered external antenna decodes",
               a.has_value() && a->type == AntennaType::External && a->number == 0);
        // ...and the API document gives it a number (the external switch port,
        // readOnly) otherwise. The selection itself carries none.
        const auto b = parseAntenna(R"({"type":"EXTERNAL","number":16})");
        report("a number on the external antenna is not part of the selection",
               b.has_value() && b->type == AntennaType::External && b->number == 0);
    }
    report("internal antenna 5 is out of range (the API allows 1-4)",
           !parseAntenna(R"({"type":"INTERNAL","number":5})").has_value());
    report("internal antenna 0 is out of range",
           !parseAntenna(R"({"type":"INTERNAL","number":0})").has_value());
    report("an unknown antenna type is rejected",
           !parseAntenna(R"({"type":"MAGIC","number":1})").has_value());
    report("a non-integer antenna number is rejected",
           !parseAntenna(R"({"type":"INTERNAL","number":"one"})").has_value());
    {
        const auto list = parseAntennas(
            R"([{"type":"INTERNAL","number":1,"state":"ACTIVE"},)"
            R"({"type":"INTERNAL","number":2,"state":"AVAILABLE"},)"
            R"({"type":"EXTERNAL","state":"DISABLED"}])");
        report("parseAntennas decodes the list with states",
               list.has_value() && list->size() == 3
                   && list->at(0).state == AntennaState::Active
                   && list->at(1).state == AntennaState::Available
                   && list->at(2).state == AntennaState::Disabled
                   && list->at(2).antenna.type == AntennaType::External);
    }
    {
        // One bad entry costs only itself.
        const auto list = parseAntennas(
            R"([{"type":"INTERNAL","number":9,"state":"ACTIVE"},)"
            R"({"type":"INTERNAL","number":2,"state":"WARM"},)"
            R"(7,{"type":"INTERNAL","number":4,"state":"AVAILABLE"}])");
        report("malformed antenna entries are skipped, valid ones survive",
               list.has_value() && list->size() == 1 && list->at(0).antenna.number == 4);
    }
    {
        // The shape a real RF2K-S sends: the list wrapped in {"antennas":[...]}.
        const auto list = parseAntennas(
            R"({"antennas":[{"type":"INTERNAL","number":1,"state":"ACTIVE"},)"
            R"({"type":"INTERNAL","number":2,"state":"AVAILABLE"},)"
            R"({"type":"INTERNAL","number":3,"state":"DISABLED"},)"
            R"({"type":"INTERNAL","number":4,"state":"DISABLED"},)"
            R"({"type":"EXTERNAL","state":"AVAILABLE"}]})");
        report("parseAntennas accepts the list wrapped in an object",
               list.has_value() && list->size() == 5
                   && list->at(0).state == AntennaState::Active
                   && list->at(1).state == AntennaState::Available
                   && list->at(2).state == AntennaState::Disabled
                   && list->at(4).antenna.type == AntennaType::External
                   && list->at(4).antenna.number == 0
                   && list->at(4).state == AntennaState::Available);
    }
    {
        // The vendor's example for an active external antenna: the entry
        // carries the external switch port (1-16).
        const auto list = parseAntennas(
            R"([{"type":"INTERNAL","number":1,"state":"AVAILABLE"},)"
            R"({"type":"EXTERNAL","number":16,"state":"ACTIVE"}])");
        report("an external entry keeps the switch port it reports",
               list.has_value() && list->size() == 2
                   && list->at(1).antenna.type == AntennaType::External
                   && list->at(1).state == AntennaState::Active
                   && list->at(1).externalPort == 16);
        const auto none = parseAntennas(R"([{"type":"EXTERNAL","state":"AVAILABLE"}])");
        report("an external entry without a number has no port",
               none.has_value() && none->size() == 1 && none->at(0).externalPort == 0);
        const auto bad = parseAntennas(R"([{"type":"EXTERNAL","number":17,"state":"ACTIVE"}])");
        report("an out-of-range external port is dropped, the entry kept",
               bad.has_value() && bad->size() == 1 && bad->at(0).externalPort == 0
                   && bad->at(0).state == AntennaState::Active);
    }
    report("an antenna list that is not an array is rejected",
           !parseAntennas(R"({"type":"INTERNAL","number":1,"state":"ACTIVE"})").has_value());
    report("an empty antenna array is a valid, empty list",
           parseAntennas("[]").has_value() && parseAntennas("[]")->isEmpty());

    // ── Error body ────────────────────────────────────────────────────────
    {
        const auto e = parseApiError(R"({"status":"error","reason":"Function currently disabled"})");
        report("parseApiError decodes status and reason",
               e.has_value() && e->status == "error" && e->reason == "Function currently disabled");
    }
    report("parseApiError rejects non-JSON", !parseApiError("Conflict").has_value());

    // ── Request builders ──────────────────────────────────────────────────
    {
        const QJsonObject o = QJsonDocument::fromJson(buildOperateModeBody(OperateMode::Operate)).object();
        report("OPERATE body", o.value("operate_mode").toString() == "OPERATE" && o.size() == 1);
        const QJsonObject s = QJsonDocument::fromJson(buildOperateModeBody(OperateMode::Standby)).object();
        report("STANDBY body", s.value("operate_mode").toString() == "STANDBY");
    }
    {
        // The API document marks ExternalAntenna.number readOnly, so the request
        // carries none. (Not observed on a unit: nobody has tried it.)
        report("external antenna body carries no number",
               buildAntennaBody({AntennaType::External, 0}) == QByteArray(R"({"type":"EXTERNAL"})"));
    }
    {
        const QJsonObject o = QJsonDocument::fromJson(
            buildAntennaBody({AntennaType::Internal, 4})).object();
        report("internal antenna body",
               o.value("type").toString() == "INTERNAL" && o.value("number").toInt() == 4);
    }
    report("an out-of-range internal antenna is never built",
           buildAntennaBody({AntennaType::Internal, 5}).isEmpty()
               && buildAntennaBody({AntennaType::Internal, 0}).isEmpty());
    report("a numbered external antenna is never built",
           buildAntennaBody({AntennaType::External, 1}).isEmpty()
               && buildAntennaBody({AntennaType::External, -1}).isEmpty());
    {
        // A built body must round-trip through the parser the device's reply uses.
        const auto round = parseAntenna(buildAntennaBody({AntennaType::External, 0}));
        report("a built antenna body parses back to the same antenna",
               round.has_value() && *round == Antenna{AntennaType::External, 0});
    }

    // ── Presentation helpers ──────────────────────────────────────────────
    report("temperature in °C passes through", nearlyEqual(temperatureCelsius({40.0f, "°C"}), 40.0f));
    report("temperature in °F is converted", nearlyEqual(temperatureCelsius({104.0f, "°F"}), 40.0f));
    report("frequency in kHz formats as MHz", formatFrequency({14200, "kHz"}) == "14.200 MHz");
    report("frequency in Hz formats as MHz", formatFrequency({7074000, "Hz"}) == "7.074 MHz");
    report("frequency in MHz formats as MHz", formatFrequency({28, "MHz"}) == "28.000 MHz");
    report("an unrecognized frequency unit is shown as given, not mis-scaled",
           formatFrequency({14200, "furlongs"}) == "14200 furlongs");
    report("band is value plus unit", formatBand({20, "m"}) == "20 m");
    report("band with no unit is just the value", formatBand({20, ""}) == "20");
    report("internal antenna label", antennaLabel({AntennaType::Internal, 2}) == "2");
    report("external antenna label", antennaLabel({AntennaType::External, 0}) == "EXT");

    std::printf("\n%d RF-KIT protocol test(s) failed.\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
