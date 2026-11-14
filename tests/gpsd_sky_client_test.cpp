// The AetherGPS sky view's gpsd client (tools/aether-gps): a SKY report's
// satellites parse and are bounded, a DOP-only SKY and other classes are not
// a sky view, constellations come from gnssid or gpsd's PRN numbering, the
// stream reassembles lines split across reads and survives an overlong one,
// and only LAN or tailnet addresses are tried. The bundled AetherGPS
// packages match their pins and the legacy one is a valid waveform.
// Socket-free; files only in a temporary dir.

#include "core/AetherGpsPackage.h"
#include "core/GpsdSkyClient.h"
#include "core/LegacyWaveformPackage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace {

class StreamProbe : public GpsdSkyClient {
public:
    using GpsdSkyClient::ingest;
    using GpsdSkyClient::pendingBytes;
};

int g_failed = 0;

void check(bool ok, const char* label)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok) {
        ++g_failed;
    }
}

// The shape a FLEX-8600's gpsd 3.22 sends (2026-11-13), trimmed to three.
const QByteArray kSky =
    R"({"class":"SKY","device":"/dev/ttyGPS","xdop":0.9,"nSat":3,"uSat":1,"satellites":[)"
    R"({"PRN":5,"gnssid":0,"svid":5,"el":41.0,"az":120.0,"ss":38.0,"used":true},)"
    R"({"PRN":70,"gnssid":6,"svid":6,"el":12.0,"az":300.0,"ss":0.0,"used":false},)"
    R"({"PRN":301,"gnssid":2,"svid":1,"ss":21.0,"used":false}]})";

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    {
        const auto sky = gpsdsky::parseSky(kSky);
        check(sky && sky->size() == 3, "a SKY report's satellites parse");
        const GpsSatellite& gps = sky->at(0);
        check(gps.prn == 5 && gps.gnssId == 0 && gps.elevationDeg == 41.0
                  && gps.azimuthDeg == 120.0 && gps.snrDbHz == 38.0 && gps.used && gps.heard()
                  && gps.hasSkyPosition(),
              "every field of a used satellite");
        check(!sky->at(1).heard() && !sky->at(1).used, "ss 0 is listed but not heard");
        check(!sky->at(2).hasSkyPosition() && sky->at(2).heard(),
              "no el/az: in the list, off the plot");
    }

    check(!gpsdsky::parseSky(R"({"class":"SKY","device":"/dev/ttyGPS","hdop":1.1})"),
          "a DOP-only SKY is not a sky view (it would blank the plot)");
    check(!gpsdsky::parseSky(R"({"class":"TPV","mode":3})") && !gpsdsky::parseSky("garbage")
              && !gpsdsky::parseSky(""),
          "other classes and non-JSON are refused");

    {
        const auto sky = gpsdsky::parseSky(
            R"({"class":"SKY","satellites":[{"PRN":0},{"PRN":7,"el":-5,"az":10,"ss":250},)"
            R"({"PRN":8,"el":45,"az":400,"ss":-3,"gnssid":42},{"PRN":9,"el":45,"az":360}]})");
        check(sky && sky->size() == 3, "a PRN of 0 is dropped");
        check(!sky->at(0).hasSkyPosition() && !sky->at(0).heard(),
              "below the horizon, and an impossible signal, are dropped");
        check(!sky->at(1).hasSkyPosition() && !sky->at(1).heard() && sky->at(1).gnssId == -1,
              "azimuth over 360, a negative signal and an unknown gnssid are dropped");
        check(sky->at(2).hasSkyPosition() && sky->at(2).azimuthDeg == 0.0, "azimuth 360 is north");
        const auto unknown = gpsdsky::parseSky(
            R"({"class":"SKY","satellites":[{"PRN":1,"el":0.0,"az":0.0,"ss":0.0},)"
            R"({"PRN":2,"el":0.0,"az":12.0}]})");
        check(unknown && !unknown->at(0).hasSkyPosition(),
              "el 0 az 0 is gpsd's unknown position, not the north rim");
        check(unknown->at(1).hasSkyPosition(), "on the horizon at another azimuth is a position");
    }
    {
        QByteArray many = R"({"class":"SKY","satellites":[)";
        for (int i = 1; i <= 300; ++i) {
            many += QByteArray(i > 1 ? "," : "") + R"({"PRN":)" + QByteArray::number(i) + "}";
        }
        many += "]}";
        const auto sky = gpsdsky::parseSky(many);
        check(sky && sky->size() == 128, "at most 128 satellites are kept");
    }

    auto system = [](int prn, int gnss = -1) {
        GpsSatellite s;
        s.prn = prn;
        s.gnssId = gnss;
        return gpsdsky::constellation(s);
    };
    using C = gpsdsky::Constellation;
    check(system(5, 6) == C::Glonass && system(5, 2) == C::Galileo && system(5, 3) == C::BeiDou,
          "gnssid decides the constellation when sent");
    check(system(12) == C::Gps && system(70) == C::Glonass && system(133) == C::Sbas
              && system(195) == C::Qzss && system(210) == C::BeiDou && system(305) == C::Galileo
              && system(999) == C::Unknown,
          "without gnssid, gpsd's PRN ranges decide");

    {
        StreamProbe client;
        QList<QList<GpsSatellite>> views;
        QObject::connect(&client, &GpsdSkyClient::skyUpdated,
                         [&](const QList<GpsSatellite>& s) { views.append(s); });
        client.ingest(R"({"class":"VERSION","release":"3.22"})" "\r\n" + kSky.left(40));
        check(views.isEmpty() && client.gpsdRelease() == QLatin1String("3.22"),
              "VERSION is noted; half a SKY waits");
        client.ingest(kSky.mid(40) + "\r\n" + R"({"class":"TPV","mode":3})" "\n");
        check(views.size() == 1 && views.first().size() == 3, "the rest of the line completes it");
        client.ingest(QByteArray(70 * 1024, 'x'));
        client.ingest(QByteArray("tail of the overlong line\n") + kSky + "\n");
        check(views.size() == 2, "an overlong line is dropped and the next one parses");
        check(client.pendingBytes() == 0, "nothing of the overlong line is kept");
    }
    {
        // Valid JSON over the bound: only the length can refuse it.
        QByteArray big = kSky;
        big.insert(big.size() - 1, QByteArray(R"(,"pad":")") + QByteArray(70 * 1024, 'x') + "\"");
        check(gpsdsky::parseSky(big).has_value(), "the oversized report is itself a valid SKY");
        StreamProbe client;
        int views = 0;
        QObject::connect(&client, &GpsdSkyClient::skyUpdated, [&] { ++views; });
        client.ingest(big + "\n");
        check(views == 0, "a valid SKY over 64 KiB in one read is refused");
        client.ingest(big.left(40000));
        check(client.pendingBytes() == 40000, "a partial line under the bound waits");
        client.ingest(big.mid(40000) + "\n");
        check(views == 0, "the same report split into two reads under the bound is refused");
        client.ingest(kSky + "\n");
        check(views == 1, "the next report still parses");
        client.ingest(QByteArray(70 * 1024, 'y'));
        check(client.pendingBytes() == 0, "an unterminated tail over the bound is not kept");
    }

    {
        // A FLEX-8600's gpsd alternates a GPS+GLONASS SKY with a full one
        // that adds Galileo (2026-11-14); the view must not flicker.
        const QByteArray partial = R"({"class":"SKY","satellites":[{"PRN":5,"gnssid":0,"el":40,"az":10,"ss":30},)"
                                   R"({"PRN":70,"gnssid":6,"el":20,"az":200,"ss":20}]})";
        const QByteArray full = R"({"class":"SKY","satellites":[{"PRN":5,"gnssid":0,"el":40,"az":10,"ss":31},)"
                                R"({"PRN":70,"gnssid":6,"el":20,"az":200,"ss":20},)"
                                R"({"PRN":301,"gnssid":2,"el":60,"az":90,"ss":25}]})";
        StreamProbe client;
        QList<int> sizes;
        QList<GpsSatellite> last;
        QObject::connect(&client, &GpsdSkyClient::skyUpdated, [&](const QList<GpsSatellite>& v) {
            sizes.append(int(v.size()));
            last = v;
        });
        client.ingest(full + "\n" + partial + "\n" + full + "\n" + partial + "\n");
        check(sizes == QList<int>({3, 3, 3, 3}), "alternating partial and full reports give a steady view");
        check(last.size() == 3 && last.at(0).prn == 5 && last.at(0).snrDbHz == 30.0,
              "merged by PRN, each satellite from its latest report");
        // Galileo already missed the last partial above; two more make three.
        client.ingest(partial + "\n" + partial + "\n");
        check(sizes.last() == 3, "a satellite stays while missing from 3 reports in a row");
        client.ingest(partial + "\n");
        check(sizes.last() == 2, "and leaves once missing from 4");
    }
    {
        GpsdDetail d;
        check(gpsdsky::applyDetail(QJsonDocument::fromJson(
                  R"({"class":"SKY","hdop":7.0,"vdop":7.0,"pdop":10.0,"tdop":8.24,"gdop":13.29})").object(), d)
                  && d.hdop == 7.0 && d.pdop == 10.0 && d.gdop == 13.29,
              "SKY dilutions of precision");
        gpsdsky::applyDetail(QJsonDocument::fromJson(R"({"class":"SKY","hdop":6.0,"gdop":250})").object(), d);
        check(d.hdop == 6.0 && d.gdop == 13.29 && d.pdop == 10.0,
              "a DOP missing or out of range keeps its last value");
        check(gpsdsky::applyDetail(QJsonDocument::fromJson(
                  R"({"class":"TPV","mode":3,"eph":133.0,"epv":161.0,"ept":0.005,"lat":1,"lon":2})").object(), d)
                  && d.mode == 3 && d.ephM == 133.0 && d.epvM == 161.0 && d.eptS == 0.005,
              "TPV fix mode and estimated errors");
        gpsdsky::applyDetail(QJsonDocument::fromJson(R"({"class":"TPV","mode":1})").object(), d);
        check(d.mode == 1 && std::isnan(d.ephM) && std::isnan(d.eptS),
              "errors gpsd leaves out are unknown, not stale");
        check(!gpsdsky::applyDetail(QJsonDocument::fromJson(R"({"class":"TPV","mode":9})").object(), d)
                  && d.mode == 1,
              "an impossible mode is refused");
        check(gpsdsky::applyDetail(QJsonDocument::fromJson(
                  R"({"class":"PPS","real_sec":1000,"real_nsec":0,"clock_sec":1000,"clock_nsec":99855})").object(), d)
                  && d.hasPps && d.ppsOffsetNs == 99855,
              "PPS: the radio clock's offset from GPS time, in ns");
        gpsdsky::applyDetail(QJsonDocument::fromJson(
            R"({"class":"PPS","real_sec":1000,"real_nsec":0,"clock_sec":999,"clock_nsec":999999000})").object(), d);
        check(d.ppsOffsetNs == -1000, "a clock just behind the pulse is negative");
        gpsdsky::applyDetail(QJsonDocument::fromJson(
            R"({"class":"PPS","real_sec":1000,"real_nsec":0,"clock_sec":999,"clock_nsec":99855})").object(), d);
        check(d.ppsOffsetNs == 99855,
              "a pulse gpsd labels one second off reads as its offset within the second (FLEX-8600)");
        check(!gpsdsky::applyDetail(QJsonDocument::fromJson(
                  R"({"class":"PPS","real_sec":1000,"real_nsec":0,"clock_sec":5000,"clock_nsec":0})").object(), d)
                  && d.ppsOffsetNs == 99855
                  && !gpsdsky::applyDetail(QJsonDocument::fromJson(
                         R"({"class":"PPS","real_sec":1000,"real_nsec":2000000000,"clock_sec":1000,"clock_nsec":0})").object(), d),
              "a clock far off, or nanoseconds out of range, are refused");
        check(gpsdsky::applyDetail(QJsonDocument::fromJson(
                  R"({"class":"DEVICES","devices":[{"path":"/dev/ttyGPS","driver":"NMEA0183","bps":38400},)"
                  R"({"path":"/dev/pps0","driver":"PPS"}]})").object(), d)
                  && d.device == QLatin1String("/dev/ttyGPS") && d.driver == QLatin1String("NMEA0183")
                  && d.bps == 38400 && d.ppsDevice == QLatin1String("/dev/pps0"),
              "DEVICES: the receiver and the PPS source");
        check(!gpsdsky::applyDetail(QJsonDocument::fromJson(R"({"class":"VERSION"})").object(), d),
              "other classes are not detail");
    }

    check(gpsdsky::reachableRadio(false, QHostAddress(QStringLiteral("192.168.50.100")))
              && gpsdsky::reachableRadio(false, QHostAddress(QStringLiteral("100.83.76.76")))
              && gpsdsky::reachableRadio(false, QHostAddress(QStringLiteral("169.254.1.2"))),
          "a LAN, tailnet or link-local radio is tried");
    check(gpsdsky::reachableRadio(false, QHostAddress(QStringLiteral("fd7a:115c:a1e0::1")))
              && !gpsdsky::reachableRadio(false, QHostAddress(QStringLiteral("fd00::1"))),
          "the shim's IPv6 tailnet address is tried; other IPv6 is not");
    check(!gpsdsky::reachableRadio(true, QHostAddress(QStringLiteral("192.168.50.100")))
              && !gpsdsky::reachableRadio(false, QHostAddress(QStringLiteral("8.8.8.8")))
              && !gpsdsky::reachableRadio(false, QHostAddress()),
          "SmartLink, public and empty addresses are not");

    // ── the bundled packages ──────────────────────────────────────────────
    for (const AetherGpsPackage::Pin& pin : {AetherGpsPackage::kLegacy, AetherGpsPackage::kContainer}) {
        QTemporaryDir dir;
        QString error;
        const QString path = aethergps::writeBundledPackage(pin, dir.path(), &error);
        QFile f(path);
        const QByteArray bytes = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        check(!path.isEmpty() && bytes.size() == pin.size
                  && QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()
                         == QByteArray(pin.sha256),
              "a bundled package is written and matches its pin");
    }
    {
        QTemporaryDir dir;
        QString error;
        const LegacyWaveformPackageInfo info = inspectLegacyWaveformPackageFile(
            aethergps::writeBundledPackage(AetherGpsPackage::kLegacy, dir.path(), &error));
        check(info.valid && info.waveformName == QLatin1String(kAetherGpsWaveformName)
                  && info.version == QLatin1String(AetherGpsPackage::kVersion),
              "the legacy one is a valid waveform named AetherGPS, at the pinned version");
        QTemporaryDir other;
        QString why;
        AetherGpsPackage::Pin wrong = AetherGpsPackage::kLegacy;
        wrong.resource = ":/cty.dat";
        const QString none = aethergps::writeBundledPackage(wrong, other.path(), &why);
        check(none.isEmpty() && !why.isEmpty()
                  && !QFile::exists(QDir(other.path()).filePath(QString::fromLatin1(wrong.fileName))),
              "a resource that doesn't match the pin is refused and nothing is written");
    }

    std::printf(g_failed ? "\n%d gpsd sky tests failed.\n" : "\nAll gpsd sky tests passed.\n",
                g_failed);
    return g_failed ? 1 : 0;
}
