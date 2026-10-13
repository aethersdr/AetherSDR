#include "TestSettingsProfile.h"
#include "RadioSetupControlFixture.h"
#include "core/AppSettings.h"
#include "core/LocalMemoryStore.h"
#include "core/backends/sim/SimBackend.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

using namespace AetherSDR;

namespace AetherSDR {
class StoredControlTestAccess {
public:
    static void memory(RadioModel& model, const MemoryDelta& delta) { model.applyMemoryChanges(delta); }
    static void profile(RadioModel& model, const ProfileDelta& delta) { model.applyProfileChanges(delta); }
    static bool flush(RadioModel& model) { return model.m_localMemories.flush(); }
};
}

namespace {
int failures{0};
void check(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
MemoryEntry channel()
{
    MemoryEntry entry;
    entry.name = QStringLiteral("Two meter repeater");
    entry.group = QStringLiteral("Local channels");
    entry.owner = QStringLiteral("N0CALL");
    entry.mode = QStringLiteral("FM"); entry.freq = 146.52;
    entry.offsetDir = QStringLiteral("down"); entry.repeaterOffset = 0.6;
    entry.toneMode = QStringLiteral("ctcss_tx"); entry.toneValue = 88.5;
    entry.rxFilterLow = 100; entry.rxFilterHigh = 3000;
    return entry;
}
void seed()
{
    const QJsonObject document = QJsonDocument::fromJson(LocalMemoryStore::serialize({})).object();
    check(AppSettings::instance().setRadioFeature("local", "", "MemoryBank", 1, document), "isolated bank seeded");
}
void encoders()
{
    using Kind = ProfileRequest::Kind;
    using Action = ProfileRequest::Action;
    RadioSetupTestBackend backend;
    const std::vector<std::pair<ProfileRequest, QString>> cases{
        {{Kind::Global, Action::Refresh}, "profile global info"},
        {{Kind::Transmit, Action::Refresh}, "profile tx info"},
        {{Kind::Mic, Action::Refresh}, "profile mic info"},
        {{Kind::Global, Action::Load, "My global"}, "profile global load \"My global\""},
        {{Kind::Transmit, Action::Load, "My TX"}, "profile tx load \"My TX\""},
        {{Kind::Mic, Action::Load, "My Mic"}, "profile mic load \"My Mic\""},
        {{Kind::Global, Action::Save, "My global"}, "profile global save \"My global\""},
        {{Kind::Transmit, Action::Create, "My TX*"}, "profile transmit create \"My TX\""},
        {{Kind::Mic, Action::Create, "My Mic"}, "profile mic create \"My Mic\""},
        {{Kind::Global, Action::Remove, "My global"}, "profile global delete \"My global\""},
        {{Kind::Transmit, Action::Remove, "My TX*"}, "profile transmit delete \"My TX\""},
        {{Kind::Mic, Action::Remove, "My Mic"}, "profile mic delete \"My Mic\""},
        {{Kind::Global, Action::AutoSave, {}, true}, "profile autosave on"},
        {{Kind::Global, Action::AutoSave, {}, false}, "profile autosave off"}
    };
    for (const auto& [request, expected] : cases) {
        backend.commands.clear();
        check(backend.requestProfile(request) == ReceiveDispatch::Dispatched
            && backend.commands == QStringList{expected}, "profile encoder matches SDK command");
    }
    backend.commands.clear();
    for (const QString& name : {QString(), QStringLiteral(" "), QStringLiteral("Bad\nname"),
             QStringLiteral("Bad\"name"), QStringLiteral("Bad|name"), QStringLiteral("Bad\\name")}) {
        check(backend.requestProfile({Kind::Global, Action::Save, name}) == ReceiveDispatch::Unsupported,
              "unsafe profile name refused");
    }
    check(backend.requestProfile({Kind::Mic, Action::Create, "***"}) == ReceiveDispatch::Unsupported,
          "normalized-empty profile refused");
    check(backend.requestProfile({Kind::Transmit, Action::Save, "Existing"}) == ReceiveDispatch::Unsupported,
          "TX overwrite cannot be manufactured");
    check(backend.commands.isEmpty(), "invalid profiles have no effects");

    MemoryRequest update{MemoryRequest::Action::Update, 7, MemoryPatch::fromEntry(channel())};
    backend.requestMemory(update);
    const QString encoded = backend.commands.value(0);
    check(encoded.startsWith("memory set 7 group=Local" + QString(QChar(0x7f)) + "channels owner=N0CALL freq=146.520000"),
          "memory text uses DEL-space and fixed precision");
    check(encoded.contains("repeater=down repeater_offset=0.600000 tone_mode=ctcss_tx tone_value=88.5"),
          "memory repeater fields preserve SDK units");
    check(encoded.endsWith("rtty_mark=2125 rtty_shift=170 digl_offset=2210 digu_offset=1500"),
          "complete memory snapshot carries digital offsets");
    backend.commands.clear();
    MemoryRequest recall{MemoryRequest::Action::Recall, 7, {}};
    recall.sliceId = 3; recall.recall = channel();
    backend.requestMemory(recall);
    check(backend.commands == QStringList{"memory apply 7", "slice tune 3 146.520000 autopan=0", "slice set 3 step=100",
        "slice set 3 repeater_offset_dir=down fm_repeater_offset_freq=0.600000 tx_offset_freq=-0.600000 fm_tone_mode=ctcss_tx fm_tone_value=88.5"},
        "recall preserves ordered retune/step/repeater correction");
    backend.commands.clear();
    update.patch.freq = std::numeric_limits<double>::infinity();
    check(backend.requestMemory(update) == ReceiveDispatch::Unsupported && backend.commands.isEmpty(), "nonfinite memory refuses before dispatch");
    update.patch = {}; update.patch.name = QStringLiteral("Bad\nname");
    check(!update.valid(), "control bytes refused in typed memory");
    update.patch = {}; update.patch.diglOffset = -100;
    check(update.valid(), "negative digital offsets retain CSV compatibility");
}
void localBank()
{
    seed();
    RadioModel model;
    int index = -1; int calls = 0;
    model.createMemory(channel(), &model, [&](const RadioControlReply& result, int created) {
        ++calls; check(result.kind == RadioControlReply::Kind::Accepted, "local create accepted"); index = created;
    });
    check(calls == 0, "local completion is queued, not recursive");
    QCoreApplication::processEvents();
    check(calls == 1 && index >= 0 && model.memories().value(index).name == channel().name, "complete local slot is published");
    MemoryPatch patch; patch.name = QStringLiteral("Changed name");
    check(model.requestMemory({MemoryRequest::Action::Update, index, patch}, &model) == ReceiveDispatch::LocalOnly,
          "local edit has honest local-only receipt");
    check(StoredControlTestAccess::flush(model), "typed bank flushes durably");
    const auto persisted = LocalMemoryStore::parse(QJsonDocument(AppSettings::instance().radioFeatureExact("local", "", "MemoryBank")).toJson());
    check(persisted.memories.value(index).name == "Changed name", "database readback proves persistence");
    MemoryDelta imported; imported.index = 20; imported.importSource = "icom:test"; imported.importKey = "42";
    imported.freq = 14.074; imported.mode = "DIGU"; imported.nativeFilter = 2; imported.dataMode = 1;
    StoredControlTestAccess::memory(model, imported);
    const int importedIndex = model.memories().lastKey();
    MemoryPatch mode; mode.mode = "USB";
    model.requestMemory({MemoryRequest::Action::Update, importedIndex, mode}, &model);
    const MemoryEntry changed = model.memories().value(importedIndex);
    check(changed.dataMode == 0 && changed.nativeFilter == 2 && changed.importKey == "42",
          "typed mode edits preserve provenance and update native DATA flag");
    model.requestMemory({MemoryRequest::Action::Remove, index, {}}, &model);
    check(!model.memories().contains(index), "typed local remove publishes removal");
}
void asynchronous()
{
    RadioModel model;
    RadioSetupTestBackend* backend = installRadioSetupBackend(model);
    int completed = 0;
    model.createMemory(channel(), &model, [&](const RadioControlReply& result, int index) {
        ++completed; check(result.kind == RadioControlReply::Kind::Accepted && index == 42, "radio create complete only after populate");
    });
    check(backend->commands == QStringList{"memory create"}, "engine owns create sequence");
    const RadioControlCompletion created = backend->replies.at(0);
    created({RadioControlReply::Kind::Accepted, 0, "42"});
    check(backend->commands.size() == 2 && backend->commands.last().startsWith("memory set 42 ") && completed == 0,
          "create reply starts one population, not early success");
    const RadioControlCompletion populated = backend->replies.at(1);
    populated({RadioControlReply::Kind::Accepted, 0, {}});
    populated({RadioControlReply::Kind::Accepted, 0, {}});
    created({RadioControlReply::Kind::Accepted, 0, "43"});
    check(completed == 1 && backend->commands.size() == 2, "duplicate replies cannot repeat operations");

    backend->commands.clear(); backend->replies.clear();
    model.createMemory(channel(), &model, [&](const RadioControlReply&, int) { ++completed; });
    const RadioControlCompletion stale = backend->replies.at(0);
    RadioSetupControlTestAccess::replaceConnectionGeneration(model);
    stale({RadioControlReply::Kind::Accepted, 0, "43"});
    check(backend->commands == QStringList{"memory create"} && completed == 2, "stale create never populates replacement session");

    backend->commands.clear(); backend->replies.clear();
    model.createMemory(channel(), &model, [&](const RadioControlReply& result, int index) {
        check(result.kind == RadioControlReply::Kind::Failed && index == 44, "failed population reported after rollback");
    });
    const RadioControlCompletion first = backend->replies.at(0);
    first({RadioControlReply::Kind::Accepted, 0, "44"});
    const RadioControlCompletion rejected = backend->replies.at(1);
    rejected({RadioControlReply::Kind::Failed, 17, "field refused"});
    check(backend->commands.last() == "memory remove 44", "positive refusal rolls back original created slot");
    const RadioControlCompletion cleanup = backend->replies.at(2);
    cleanup({RadioControlReply::Kind::Accepted, 0, {}});
    check(!model.memories().contains(44), "rollback reconciles the cache");

    backend->commands.clear(); backend->replies.clear();
    MemoryPatch name; name.name = "Older request";
    model.requestMemory({MemoryRequest::Action::Update, 42, name}, &model);
    const RadioControlCompletion oldEdit = backend->replies.at(0);
    MemoryDelta newer; newer.index = 42; newer.name = "Radio status wins";
    StoredControlTestAccess::memory(model, newer);
    oldEdit({RadioControlReply::Kind::Accepted, 0, {}});
    check(model.memories().value(42).name == "Radio status wins", "late accepted edit cannot overwrite newer status");

    backend->commands.clear(); backend->replies.clear();
    model.requestMemory({MemoryRequest::Action::Update, 42, name}, &model);
    const RadioControlCompletion repeatedObservation = backend->replies.at(0);
    StoredControlTestAccess::memory(model, newer);
    repeatedObservation({RadioControlReply::Kind::Accepted, 0, {}});
    check(model.memories().value(42).name == "Radio status wins", "same-value new observation still fences older optimistic edit");

    backend->commands.clear(); backend->replies.clear();
    model.requestMemory({MemoryRequest::Action::Update, 42, name}, &model);
    MemoryPatch newest; newest.name = "Newest intent";
    model.requestMemory({MemoryRequest::Action::Update, 42, newest}, &model);
    const RadioControlCompletion earlier = backend->replies.at(0);
    const RadioControlCompletion later = backend->replies.at(1);
    earlier({RadioControlReply::Kind::Accepted, 0, {}});
    later({RadioControlReply::Kind::Accepted, 0, {}});
    check(model.memories().value(42).name == "Newest intent", "two pending edits retain newest accepted intent");
    StoredControlTestAccess::memory(model, newer);

    backend->commands.clear(); backend->replies.clear();
    model.createMemory(channel(), &model, [](const RadioControlReply&, int) {});
    const RadioControlCompletion reusedCreate = backend->replies.at(0);
    reusedCreate({RadioControlReply::Kind::Accepted, 0, "46"});
    const RadioControlCompletion refusedPopulate = backend->replies.at(1);
    MemoryDelta retired; retired.index = 46; retired.removed = true;
    StoredControlTestAccess::memory(model, retired);
    MemoryDelta replacement; replacement.index = 46; replacement.name = "Another client's slot";
    StoredControlTestAccess::memory(model, replacement);
    refusedPopulate({RadioControlReply::Kind::Failed, 17, "field refused"});
    check(backend->commands.size() == 2 && model.memories().value(46).name == "Another client's slot",
          "rollback never deletes a retired/reused slot in the same session");

    backend->commands.clear(); backend->replies.clear();
    auto owner = std::make_unique<QObject>();
    model.createMemory(channel(), owner.get(), [](const RadioControlReply&, int) {});
    const RadioControlCompletion abandoned = backend->replies.at(0);
    owner.reset();
    abandoned({RadioControlReply::Kind::Accepted, 0, "45"});
    check(backend->commands == QStringList{"memory create"}, "destroyed reply owner stops continuation");

    backend->commands.clear(); backend->replies.clear();
    int timedOut = 0;
    model.requestMemory({MemoryRequest::Action::Update, 42, name}, &model,
        [&](const RadioControlReply& result) {
        ++timedOut; check(result.kind == RadioControlReply::Kind::Failed, "silent transport has bounded failure");
    });
    const RadioControlCompletion late = backend->replies.at(0);
    QEventLoop loop;
    QTimer::singleShot(31000, &loop, &QEventLoop::quit);
    loop.exec();
    late({RadioControlReply::Kind::Accepted, 0, {}});
    check(timedOut == 1 && model.memories().value(42).name == "Radio status wins",
          "timeout completes once and late response has no cache effects");
}

void unreadableBank()
{
    QJsonObject future = QJsonDocument::fromJson(LocalMemoryStore::serialize({})).object();
    future.insert(QStringLiteral("version"), 99);
    check(AppSettings::instance().setRadioFeature("local", "", "MemoryBank", 99, future), "future bank seeded");
    {
        RadioModel model;
        int replies = 0;
        model.createMemory(channel(), &model, [&](const RadioControlReply& result, int index) {
            ++replies;
            check(result.kind == RadioControlReply::Kind::Failed && index < 0, "typed create refuses newer schema");
        });
        QCoreApplication::processEvents();
        check(replies == 1 && model.memories().isEmpty(), "refused bank cannot publish a phantom memory");
        check(AppSettings::instance().radioFeatureExact("local", "", "MemoryBank") == future,
              "typed refusal leaves future database document intact");
    }
    seed();
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("stored-control-routing-test"));
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) { return 2; }
    AppSettings::instance().load();
    encoders(); localBank(); unreadableBank(); asynchronous();
    return failures == 0 ? 0 : 1;
}
