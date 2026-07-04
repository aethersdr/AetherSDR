#include "models/UsbCableModel.h"

#include <QCoreApplication>
#include <QMap>
#include <QSignalSpy>
#include <QString>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

QMap<QString, QString> kv(std::initializer_list<std::pair<QString, QString>> pairs)
{
    QMap<QString, QString> m;
    for (const auto& p : pairs) m.insert(p.first, p.second);
    return m;
}

void testTypeChangeEmitsRemovedThenAdded()
{
    UsbCableModel model;
    QSignalSpy addedSpy(&model, &UsbCableModel::cableAdded);
    QSignalSpy removedSpy(&model, &UsbCableModel::cableRemoved);
    QSignalSpy changedSpy(&model, &UsbCableModel::cableChanged);

    model.applyStatus("SN1", kv({{"type", "invalid"}, {"name", "New Cable"}}));
    report("first status for a new cable emits cableAdded",
           addedSpy.count() == 1 && removedSpy.count() == 0);

    addedSpy.clear();
    model.applyStatus("SN1", kv({{"type", "cat"}, {"speed", "9600"}}));
    report("retyping an existing cable emits cableRemoved then cableAdded, not cableChanged",
           removedSpy.count() == 1 && addedSpy.count() == 1 && changedSpy.count() == 0);
}

void testStaleFieldsDoNotSurviveRetype()
{
    UsbCableModel model;

    model.applyStatus("SN1", kv({{"type", "cat"}, {"auto_report", "0"}, {"speed", "38400"}}));
    report("cat cable initial fields set (auto_report=0, speed=38400)",
           !model.cables()["SN1"].autoReport && model.cables()["SN1"].speed == 38400);

    model.applyStatus("SN1", kv({{"type", "bit"}}));
    const auto& cable = model.cables()["SN1"];
    report("retype to bit resets autoReport to struct default (true)", cable.autoReport == true);
    report("retype to bit resets speed to struct default (9600)", cable.speed == 9600);
    report("retype to bit updates cable.type", cable.type == "bit");
}

void testRoutineResyncOnlyEmitsChanged()
{
    UsbCableModel model;
    model.applyStatus("SN1", kv({{"type", "cat"}, {"speed", "9600"}}));

    QSignalSpy addedSpy(&model, &UsbCableModel::cableAdded);
    QSignalSpy removedSpy(&model, &UsbCableModel::cableRemoved);
    QSignalSpy changedSpy(&model, &UsbCableModel::cableChanged);

    // Radio re-sends the same type on a routine status resync — no churn.
    model.applyStatus("SN1", kv({{"type", "cat"}, {"speed", "19200"}}));
    report("identical type= on resync only emits cableChanged",
           changedSpy.count() == 1 && addedSpy.count() == 0 && removedSpy.count() == 0);
    report("resync still applies the updated field", model.cables()["SN1"].speed == 19200);
}

void testLdpaStatusParsing()
{
    UsbCableModel model;
    model.applyStatus("SN1", kv({
        {"type", "ldpa"},
        {"band", "4"},
        {"preamp", "1"},
        {"source", "active_slice"},
    }));

    const auto& cable = model.cables()["SN1"];
    report("ldpa band parsed", cable.ldpaBand == "4");
    report("ldpa preamp parsed", cable.preamp == true);
    report("ldpa source parsed", cable.source == "active_slice");
}

void testBcdSubtypeChangeDoesNotRecreate()
{
    UsbCableModel model;
    model.applyStatus("SN1", kv({{"type", "bcd"}, {"name", "My BCD"}}));

    QSignalSpy addedSpy(&model, &UsbCableModel::cableAdded);
    QSignalSpy removedSpy(&model, &UsbCableModel::cableRemoved);
    QSignalSpy changedSpy(&model, &UsbCableModel::cableChanged);

    // bcd/vbcd/bcd_vbcd are one UsbCableType family in FlexLib
    // (StringToUsbCableType collapses all three to BCD) -- a sub-type change
    // between them must not trigger a remove/recreate.
    model.applyStatus("SN1", kv({{"type", "vbcd"}, {"polarity", "active_low"}}));
    report("bcd sub-type change only emits cableChanged, not remove/recreate",
           changedSpy.count() == 1 && addedSpy.count() == 0 && removedSpy.count() == 0);
    report("bcd sub-type change updates cable.type", model.cables()["SN1"].type == "vbcd");
    report("bcd sub-type change preserves unrelated fields (name)",
           model.cables()["SN1"].name == "My BCD");
}

void testBaseFieldsSurvivePartialRetypeConfirm()
{
    UsbCableModel model;
    model.applyStatus("SN1", kv({
        {"type", "cat"}, {"name", "My Cable"},
        {"enable", "1"}, {"plugged_in", "1"}, {"log", "1"},
    }));

    // A real type-family change (cat -> bit), but the confirming status is
    // partial and omits name=/enable=/plugged_in=/log= -- FlexLib's Radio.cs
    // ParseUsbCableStatus reuses these base identity fields across a type
    // change rather than blanking them, so they must survive the recreate.
    model.applyStatus("SN1", kv({{"type", "bit"}}));
    const auto& cable = model.cables()["SN1"];
    report("partial retype confirm preserves name", cable.name == "My Cable");
    report("partial retype confirm preserves enabled", cable.enabled == true);
    report("partial retype confirm preserves present", cable.present == true);
    report("partial retype confirm preserves loggingEnabled", cable.loggingEnabled == true);
    report("partial retype confirm still updates type", cable.type == "bit");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testTypeChangeEmitsRemovedThenAdded();
    testStaleFieldsDoNotSurviveRetype();
    testRoutineResyncOnlyEmitsChanged();
    testLdpaStatusParsing();
    testBcdSubtypeChangeDoesNotRecreate();
    testBaseFieldsSurvivePartialRetypeConfirm();

    return g_failed == 0 ? 0 : 1;
}
