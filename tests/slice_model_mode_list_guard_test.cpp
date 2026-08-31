// Plan 2.4: SliceModel::setMode() refuses a mode the radio does not offer.
//
// Once a backend publishes SliceDelta::modeList (Flex always, HL2 as of
// Plan 2), an operator / CAT / rigctl / TCI request for a mode outside that
// list must be rejected rather than passed to a demodulator that will silently
// substitute something else. Status application (applyChanges) must be
// unaffected — it assigns the mode directly and is how a restored slice mode
// arrives (Principle II).

#include "models/SliceModel.h"
#include "core/backends/SliceDelta.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // An empty mode list means "unconstrained" — the guard refuses nothing.
    // (Use a non-digital-voice mode: DSTR/FreeDV also go through the
    // DigitalVoiceModeRegistry, which has its own rejection path.)
    {
        SliceModel s(1);
        s.setMode(QStringLiteral("DSB"));
        check(s.mode() == QLatin1String("DSB"),
              "no published list -> the mode-list guard refuses nothing");
    }

    // With a published list, only listed modes are accepted (case-insensitive).
    {
        SliceModel s(1);
        SliceDelta d;
        d.modeList = QStringList{QStringLiteral("USB"), QStringLiteral("LSB"),
                                 QStringLiteral("CW"), QStringLiteral("RTTY"),
                                 QStringLiteral("DFM")};
        s.applyChanges(d);

        s.setMode(QStringLiteral("RTTY"));
        check(s.mode() == QLatin1String("RTTY"), "a listed mode is accepted");

        s.setMode(QStringLiteral("usb"));
        check(s.mode() == QLatin1String("usb"),
              "a listed mode is accepted case-insensitively");

        const QString before = s.mode();
        // Non-digital-voice unlisted modes — these isolate the mode-list guard
        // (DSTR/FreeDV would also be stopped by the DV registry).
        s.setMode(QStringLiteral("DSB"));
        check(s.mode() == before, "an unlisted non-DV mode (DSB) is refused");
        s.setMode(QStringLiteral("WFM"));
        check(s.mode() == before, "an unlisted non-DV mode (WFM) is refused");
        s.setMode(QStringLiteral("DRM"));
        check(s.mode() == before, "an unlisted mode (DRM) is refused");
        // And the digital-voice ones the raw-IQ backend cannot honour.
        s.setMode(QStringLiteral("DSTR"));
        check(s.mode() == before, "an unlisted digital-voice mode (DSTR) is refused");
    }

    // Status application bypasses the guard: a restored slice mode outside the
    // currently-known list still lands (the backend is telling us, not asking).
    {
        SliceModel s(1);
        SliceDelta list;
        list.modeList = QStringList{QStringLiteral("USB"), QStringLiteral("CW")};
        s.applyChanges(list);

        SliceDelta restored;
        restored.mode = QStringLiteral("DSB");
        s.applyChanges(restored);
        check(s.mode() == QLatin1String("DSB"),
              "applyChanges() sets a mode the guard would have refused");
    }

    std::printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}
