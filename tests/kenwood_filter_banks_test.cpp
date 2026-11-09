#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"

#include <QCoreApplication>
#include <QMetaType>
#include <QMap>
#include <QPointer>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static QVariantMap chronologyStatus(const KenwoodBackend& b) { return b.filterBankStatus(); }
    static void chronologyPublish(KenwoodBackend& b) { b.publishFilterBanks(); }
    static void chronologyFeed(KenwoodBackend& b, const QByteArray& bytes) { b.adopt(bytes); }
    static void authenticate(KenwoodBackend& b) {
        b.m_clock.start(); b.m_protocol.start("fixture-user", "fixture-password", false, 0);
        b.m_protocol.receive("##CN1;", 1); b.m_protocol.receive("##ID1;", 2); b.ready();
    }
    static void message(KenwoodBackend& b, const QByteArray& bytes) { b.adopt(bytes); }
    static void batch(KenwoodBackend& b, const QList<QByteArray>& bytes) {
        const quint64 batch = ++b.m_controlIngressSequence;
        for (const QByteArray& message : bytes) { b.adopt(message, batch); }
    }
    static void intent(KenwoodBackend& b, bool active) { b.m_keyIntent = active; b.adoptFilterBank({}, ++b.m_controlIngressSequence); }
    static void tuneIntent(KenwoodBackend& b, bool active) {
        if (active) { b.m_tune.emplace(); } else { b.m_tune.reset(); }
        ++b.m_tuneSerial; b.m_keyIntent = false;
        b.adoptFilterBank({}, ++b.m_controlIngressSequence);
    }
    static QVariantMap fmStatus(const KenwoodBackend& b) { return b.fmWidthStatus(); }
    static void fmPoll(KenwoodBackend& b, qint64 now) { b.pollFmWidth(now); }
    static void fmIntent(KenwoodBackend& b, bool active) { b.m_keyIntent = active; b.refreshReceiverOptionsContext(); b.refreshFmWidthSafety(); }
    static void fmTransport(KenwoodBackend& b, std::function<bool(const QByteArray&)> sink) { b.m_transport.control = std::move(sink); }
    static QVariantMap nativeStatus(const KenwoodBackend& b) { return b.nativeFilterStatus(); }
    static bool axesPending(const KenwoodBackend& b) { return b.m_filterBankAxesPending; }
};
}
namespace {
using namespace AetherSDR;
using namespace AetherSDR::kenwood;
void require(bool value, const char* message) { if (!value) { std::cerr << message << '\n'; std::exit(1); } }
struct Fixture {
    QList<QByteArray> commands;
    QVariantMap state;
    QMap<quint64, QString> errors;
    QMap<quint64, QVariant> results;
    std::function<void(const QByteArray&)> onWrite;
    bool accepts = true;
    KenwoodBackend backend{KenwoodBackend::Transport{[this](const QByteArray& bytes) {
        commands.append(bytes); if (onWrite) { onWrite(bytes); } return accepts;
    }, {}}};
    quint64 sequence = 1;
    Fixture() {
        QObject::connect(&backend, &IRadioBackend::extensionStatus, &backend, [this](const QString&, const QString& kind, const QVariantMap& value) { if (kind == "filterBanks") { state = value; } });
        QObject::connect(&backend, &IRadioBackend::extensionResult, &backend, [this](quint64 id, const QVariant& value) {
            results[id] = value; if (value.typeId() == QMetaType::QVariantMap && value.toMap().contains("controls")) { state = value.toMap(); }
        });
        QObject::connect(&backend, &IRadioBackend::extensionError, &backend, [this](quint64 id, const QString& error) { errors[id] = error; });
    }
    void observe(const QByteArray& bytes) { KenwoodBackendTestAccess::message(backend, bytes); }
    void ready() {
        KenwoodBackendTestAccess::authenticate(backend);
        for (const QByteArray& message : {QByteArray("ID024"), QByteArray("FR0"), QByteArray("FT1"), QByteArray("FA00014074000"),
            QByteArray("FB00007074000"), QByteArray("OM03"), QByteArray("SF0000140740003"), QByteArray("SF1000070740003"), QByteArray("TS0"), QByteArray("RX"), QByteArray("MO20")}) { observe(message); }
    }
    void get() { backend.invokeExtension("kenwood", "filter.banks.get", sequence++); }
    void readbacks(bool fm = false) {
        observe("EX00610 003"); observe("FL001");
        for (int bank = 0; bank < 3; ++bank) {
            observe("FL1" + QByteArray::number(bank) + (fm ? "51500" : "00050"));
            observe("FL2" + QByteArray::number(bank) + (fm ? "3" : "1"));
            observe("FL3" + QByteArray::number(bank) + "1");
        }
        observe("SL010"); observe("SH0005");
    }
    QVariantMap row(const QString& id) const {
        for (const QVariant& item : state.value("controls").toList()) { if (item.toMap().value("id") == id) { return item.toMap(); } } return {};
    }
    quint64 edit(const QString& id, const QVariant& value, bool reset = false, const QVariant& context = {}) {
        const quint64 request = sequence++;
        QVariantMap argument{{"id", id}, {"context", context.isValid() ? context : state.value("context")}};
        if (!reset) { argument.insert("value", value); }
        backend.invokeExtension("kenwood", reset ? "filter.banks.reset" : "filter.banks.set", request, argument);
        return request;
    }
};
void parsingAndAdmission() {
    Fixture f; f.ready(); f.commands.clear(); f.get();
    require(f.commands.contains("EX00610;") && f.commands.contains("FL0;") && !f.commands.contains("FL12;"), "bounded initial query reads menu and active option, does not assume bank C");
    require(!f.row("bank2.roofing").value("editable").toBool(), "unknown count dims filter C");
    f.observe("FL301"); f.commands.clear(); f.edit("bank0.afType", 2);
    require(!f.row("bank0.afType").value("editable").toBool() && !f.commands.contains("FL302;"),
            "known field without active bank readback cannot edit an unknown receive-filter context");
    f.readbacks();
    require(f.state.value("controls").toList().size() == 10 && f.row("bank0.roofing").value("actualBandwidthHz").toInt() == 500
        && f.row("bank0.roofing").value("value").toInt() == 0, "Auto selected independently from actual roofing bandwidth");
    const QVariantMap prior = f.state;
    for (const QByteArray& bad : {QByteArray("FL0"), QByteArray("FL00"), QByteArray("FL002"), QByteArray("FL031"), QByteArray("FL001x"),
        QByteArray("FL1000026"), QByteArray("FL1001501"), QByteArray("FL1090050"), QByteArray("FL203"), QByteArray("FL209"),
        QByteArray("FL303"), QByteArray("FL309"), QByteArray("EX00610 001"), QByteArray("EX00610 03")}) { f.observe(bad); }
    require(prior == f.state, "malformed, setter shaped, unavailable and reset sentinel reports do not replace observed state");
    f.commands.clear();
    for (const QVariant& invalid : {QVariant("1"), QVariant(1.0), QVariant(-1), QVariant(6), QVariant(true)}) {
        require(f.errors.contains(f.edit("bank0.roofing", invalid)), "bank setter validates exact integer enum");
    }
    require(f.errors.contains(f.edit("active", 9)) && f.errors.contains(f.edit("bank9.roofing", 0))
        && f.errors.contains(f.edit("active", 0, true)), "invalid bank and active reset refused");
    require(f.commands.isEmpty(), "invalid operations have no wire side effects");
    f.observe("FL000"); f.commands.clear();
    require(f.errors.contains(f.edit("bank0.roofing", 1)), "270 choice requires installed option");
    f.observe("EX00610 002"); f.commands.clear();
    require(f.errors.contains(f.edit("active", 2)) && f.errors.contains(f.edit("bank2.afType", 1)), "two-bank menu cannot select or edit C");
    require(f.commands.isEmpty(), "unsupported option and bank paths never write");
}
void choicesAndResets() {
    for (int bank = 0; bank < 3; ++bank) {
        for (int field = 1; field <= 3; ++field) {
            const QString name = field == 1 ? "roofing" : field == 2 ? "ifShape" : "afType";
            const QString id = QStringLiteral("bank%1.%2").arg(bank).arg(name);
            const QByteArray query = "FL" + QByteArray::number(field) + QByteArray::number(bank);
            for (int value = 0; value < (field == 1 ? 6 : 3); ++value) {
                Fixture f; f.ready(); f.get(); f.readbacks(); f.commands.clear();
                const int old = f.row(id).value("value").toInt();
                require(f.results.value(f.edit(id, value)).toBool(), "valid native choice handoff acknowledged");
                require(f.commands == QList<QByteArray>{query + QByteArray::number(value) + ';', query + ';'}, "each bank choice emits exact setter and parameterized readback query");
                require(f.row(id).value("pending").toBool() && f.row(id).value("value").toInt() == old, "pending bank choice retains observed value");
                f.observe(query + QByteArray::number(value) + (field == 1 ? "0050" : ""));
                require(!f.row(id).value("pending").toBool() && f.row(id).value("value").toInt() == value, "bank state adopts only readback");
            }
            Fixture f; f.ready(); f.get(); f.readbacks(); f.commands.clear();
            const int old = f.row(id).value("value").toInt();
            f.edit(id, {}, true);
            require(f.commands == QList<QByteArray>{query + "9;", query + ';'} && f.row(id).value("value").toInt() == old, "all nine resets send native9 then query without assumed default");
            f.observe(query + '9' + (field == 1 ? "0050" : ""));
            require(f.row(id).value("pending").toBool(), "reset9 cannot masquerade as readback");
            f.observe(query + '2' + (field == 1 ? "0050" : ""));
            require(f.row(id).value("value").toInt() == 2 && !f.row(id).value("pending").toBool(), "reset adopts actual nonassumed native value");
        }
    }
}
void allNativeAxisModes() {
    struct Mode { char code; QString name; bool styles; bool shared; bool widthOnly; };
    const QList<Mode> modes{{'1', "LSB", true, true, false}, {'2', "USB", true, true, false},
        {'3', "CW", false, true, false}, {'4', "FM", false, false, false}, {'5', "AM", false, false, false},
        {'6', "RTTY", false, false, true}, {'7', "CWR", false, true, false}, {'9', "RTTYR", false, false, true},
        {'A', "PSK", false, false, true}, {'B', "PSKR", false, false, true}, {'C', "DIGL", true, true, false},
        {'D', "DIGU", true, true, false}, {'E', "DFM", false, false, false}, {'F', "DAM", false, false, false}};
    for (const Mode& mode : modes) {
        for (int style = 0; style < (mode.styles ? 2 : 1); ++style) {
            Fixture f; f.ready(); f.get(); f.readbacks();
            f.observe(QByteArray("OM0") + mode.code); f.observe(QByteArray("SF000014074000") + mode.code);
            f.observe("EX00611 " + QByteArray::number(style).rightJustified(3, '0'));
            f.observe("EX00612 " + QByteArray::number(style).rightJustified(3, '0'));
            f.commands.clear(); f.observe("FL011");
            require(f.commands.contains("SL0;") && f.commands.contains("SH0;") != mode.widthOnly,
                    "bank refresh queries only native axes present for exact mode and style");
            int sharedEdges = 0;
            QObject::connect(&f.backend, &IRadioBackend::sliceChanged, &f.backend,
                [&](int, const SliceDelta& delta) { if (delta.filterLow || delta.filterHigh) { ++sharedEdges; } });
            const QVariantList axes = KenwoodBackendTestAccess::nativeStatus(f.backend).value("axes").toList();
            require(axes.size() == (mode.widthOnly ? 1 : 2), "all fourteen modes expose exact native axis count");
            const int lowCount = axes[0].toMap().value("valuesHz").toList().size();
            f.observe("SL0" + QByteArray::number(lowCount).rightJustified(2, '0'));
            if (!mode.widthOnly) {
                const int highCount = axes[1].toMap().value("valuesHz").toList().size();
                f.observe("SH0" + QByteArray::number(highCount).rightJustified(3, '0'));
            }
            require(KenwoodBackendTestAccess::axesPending(f.backend), "out-of-domain native index cannot qualify axis completeness");
            f.observe("SL000");
            if (!mode.widthOnly) {
                require(KenwoodBackendTestAccess::axesPending(f.backend), "two-axis native mode waits for valid high index");
                f.observe("SH0000");
            }
            require(!KenwoodBackendTestAccess::axesPending(f.backend)
                && KenwoodBackendTestAccess::nativeStatus(f.backend).value("editable").toBool(),
                    "complete fresh native axes recover every mode independent of signed shared-edge projection");
            require(sharedEdges == (mode.shared ? 1 : 0), "AM/FM/FSK/PSK native recovery fabricates no shared signed edges");
            if (!mode.shared) {
                bool advertised = false;
                const RadioCapabilities caps = f.backend.capabilities();
                require(caps.receiveFilterControl.has_value(), "native recovery preserves explicit shared filter contract");
                for (const ReceiveFilterMode& range : caps.receiveFilterControl->modes) { if (range.mode == mode.name) { advertised = true; } }
                require(!advertised, "native-only mode remains unsupported by shared filter contract");
            }
        }
    }
    Fixture reentrant; reentrant.ready(); reentrant.get(); reentrant.readbacks(); reentrant.observe("FL011");
    int oldEdges = 0; bool switched = false;
    QObject::connect(&reentrant.backend, &IRadioBackend::sliceChanged, &reentrant.backend,
        [&](int, const SliceDelta& delta) { if (delta.filterLow || delta.filterHigh) { ++oldEdges; } });
    QObject::connect(&reentrant.backend, &IRadioBackend::capabilitiesChanged, &reentrant.backend, [&] {
        if (!switched && !KenwoodBackendTestAccess::axesPending(reentrant.backend)) { switched = true; reentrant.observe("FL001"); }
    });
    reentrant.observe("SL000"); reentrant.observe("SH0080");
    require(switched && oldEdges == 0 && KenwoodBackendTestAccess::axesPending(reentrant.backend),
            "readiness callback bank switch cannot publish old precomputed edges into new context");
}
void predispatchOldReports() {
    for (bool statusConsumer : {false, true}) {
        for (const QString& id : {QStringLiteral("active"), QStringLiteral("bank0.ifShape")}) {
            Fixture f; f.ready(); f.get(); f.readbacks();
            const QByteArray query = id == "active" ? QByteArray("FL0") : QByteArray("FL20");
            const QByteArray oldReport = id == "active" ? QByteArray("FL001") : QByteArray("FL201");
            const int wanted = id == "active" ? 1 : 2;
            bool injected = false;
            bool noEarlyQueries = false;
            const auto inject = [&] {
                if (!injected) {
                    injected = true; f.observe(oldReport);
                    noEarlyQueries = f.commands.isEmpty();
                }
            };
            if (statusConsumer) {
                QObject::connect(&f.backend, &IRadioBackend::extensionStatus, &f.backend,
                    [&](const QString&, const QString& kind, const QVariantMap&) { if (kind == "filterBanks") { inject(); } });
            } else {
                QObject::connect(&f.backend, &IRadioBackend::capabilitiesChanged, &f.backend, inject);
            }
            f.commands.clear(); const quint64 request = f.edit(id, wanted);
            require(injected && noEarlyQueries && f.results.value(request).toBool(),
                    "direct old matching predispatch report cannot cancel producer or query old bank axes");
            require(f.commands == QList<QByteArray>{query + QByteArray::number(wanted) + ';', query + ';'}
                    && f.row(id).value("pending").toBool(),
                    "original setter still dispatches and waits for an actual post-handoff report");
            f.observe("SL010"); f.observe("SH0005");
            require(KenwoodBackendTestAccess::axesPending(f.backend), "pre-handoff old report did not release pending-axis hold");
            f.observe(oldReport); f.observe("SL010"); f.observe("SH0005");
            require(!f.row(id).value("pending").toBool() && !KenwoodBackendTestAccess::axesPending(f.backend),
                    "post-handoff actual unchanged readback and fresh axes may resolve refused or unchanged choice");
        }
    }
    for (const QString& id : {QStringLiteral("active"), QStringLiteral("bank0.ifShape")}) {
        Fixture f; f.ready(); f.get(); f.readbacks(); bool injected = false;
        const QByteArray external = id == "active" ? QByteArray("FL011") : QByteArray("FL200");
        QObject::connect(&f.backend, &IRadioBackend::capabilitiesChanged, &f.backend, [&] {
            if (!injected) { injected = true; f.observe(external); }
        });
        f.commands.clear(); const quint64 request = f.edit(id, 2);
        require(injected && f.errors.contains(request) && !f.commands.contains(id == "active" ? QByteArray("FL02;") : QByteArray("FL202;")),
                "genuine external predispatch change supersedes old producer instead of acknowledging it");
        require(f.row(id).value("value").toInt() == (id == "active" ? 1 : 0), "superseding physical observation remains authoritative");
    }
}
void reentrantConsumerQueries() {
    Fixture f; f.ready(); f.get(); f.readbacks();
    int callbacks = 0;
    bool querySuppressed = false;
    bool staleInactiveRefused = false;
    QObject::connect(&f.backend, &IRadioBackend::capabilitiesChanged, &f.backend, [&] {
        if (callbacks++ == 0) {
            f.get();
            querySuppressed = f.commands.isEmpty();
            staleInactiveRefused = f.errors.contains(f.edit("bank1.afType", 2));
        }
    });
    f.commands.clear(); f.edit("active", 1);
    require(callbacks > 0 && querySuppressed && staleInactiveRefused,
            "actual capability consumer GET returns status without pre-setter queries and uncertain active target denies other bank writes");
    require(f.commands == QList<QByteArray>{"FL01;", "FL0;"}, "consumer reentry cannot put old bank queries ahead of active setter");
    f.commands.clear(); f.get();
    require(f.commands.contains("EX00610;") && f.commands.contains("FL0;") && f.commands.contains("FL11;"),
            "explicit query after handoff may recover pending bank readback");
    f.commands.clear();
    require(f.errors.contains(f.edit("bank1.afType", 2)) && f.commands.isEmpty(),
            "fully known destination B cannot be misclassified inactive while A-to-B selection awaits confirmation");
    f.observe("FL011");
    require(f.row("bank1.afType").value("editable").toBool(), "fresh active B confirmation restores independently observed bank editor");
}
void pendingAxisWindow() {
    for (const QString& id : {QStringLiteral("active"), QStringLiteral("bank0.roofing"), QStringLiteral("bank0.ifShape"), QStringLiteral("bank0.afType")}) {
        Fixture f; f.ready(); f.get(); f.readbacks();
        const int observed = f.row(id).value("value").toInt();
        int edges = 0;
        bool retiredBeforeWrite = false;
        int filterBoundaries = 0;
        int physicalSourceBoundaries = 0;
        QObject::connect(&f.backend, &IRadioBackend::sliceChanged, &f.backend, [&](int, const SliceDelta& delta) {
            if (delta.filterLow || delta.filterHigh) { ++edges; }
            if (delta.receiveTuneContextChanged) { ++physicalSourceBoundaries; }
            if (delta.receiveFilterContextChanged) {
                ++filterBoundaries;
                retiredBeforeWrite = f.commands.isEmpty();
                f.backend.setSliceFilter(0, -500, 500);
            }
        });
        f.commands.clear(); f.edit(id, observed);
        require(retiredBeforeWrite && filterBoundaries == 1 && physicalSourceBoundaries == 0
            && KenwoodBackendTestAccess::axesPending(f.backend), "affecting setter retires filter intent before dispatch without claiming a physical receiver-source change");
        require(!f.commands.contains("SL0;") && !f.commands.contains("SH0;"), "axis retirement does not query or requalify old bank before setter readback");
        f.observe("SL010"); f.observe("SH0005"); f.backend.setSliceFilter(0, -500, 500);
        require(edges == 0 && KenwoodBackendTestAccess::axesPending(f.backend), "old pending-window axis reports and reentrant shared setter cannot restore old bank");
        const QByteArray query = id == "active" ? QByteArray("FL0") : id.endsWith("roofing") ? QByteArray("FL10")
            : id.endsWith("ifShape") ? QByteArray("FL20") : QByteArray("FL30");
        const QByteArray reply = query + QByteArray::number(observed) + (id == "active" ? "1" : id.endsWith("roofing") ? "0050" : "");
        KenwoodBackendTestAccess::batch(f.backend, {reply, "SL010", "SH0005"});
        require(edges == 0 && KenwoodBackendTestAccess::axesPending(f.backend), "unchanged bank confirmation still excludes same-batch old axes");
        f.observe("SL010"); f.observe("SH0005");
        require(edges == 1 && filterBoundaries >= 2 && physicalSourceBoundaries == 0
            && !KenwoodBackendTestAccess::axesPending(f.backend), "unchanged confirmed bank restores filter qualification without retiring physical receiver identity");
    }
    for (bool mode : {false, true}) {
        Fixture transition; transition.ready(); transition.get(); transition.readbacks();
        int leakedEdges = 0;
        QObject::connect(&transition.backend, &IRadioBackend::sliceChanged, &transition.backend,
            [&](int, const SliceDelta& delta) { if (delta.filterLow || delta.filterHigh) { ++leakedEdges; } });
        KenwoodBackendTestAccess::batch(transition.backend, {mode ? QByteArray("OM02") : QByteArray("FR1"), "SL010", "SH0005"});
        require(leakedEdges == 0 && KenwoodBackendTestAccess::axesPending(transition.backend),
                "already-qualified bank context switch rejects coalesced old source or mode axis reports");
    }
    Fixture inactive; inactive.ready(); inactive.get(); inactive.readbacks(); inactive.commands.clear();
    int inactiveBoundaries = 0;
    QObject::connect(&inactive.backend, &IRadioBackend::sliceChanged, &inactive.backend,
        [&](int, const SliceDelta& delta) { if (delta.receiveFilterContextChanged || delta.receiveTuneContextChanged) { ++inactiveBoundaries; } });
    inactive.edit("bank1.afType", 2);
    require(!KenwoodBackendTestAccess::axesPending(inactive.backend) && inactiveBoundaries == 0,
            "inactive bank setter preserves filter and physical receiver context ownership");
    Fixture count; count.ready(); count.get(); count.readbacks(); count.observe("FL021"); count.observe("SL010"); count.observe("SH0005");
    count.observe("EX00610 002");
    require(!count.row("active").contains("value") && !count.row("bank0.afType").value("editable").toBool(), "menu count two with observed C withdraws contradictory active-bank authority");
    count.observe("FL021");
    require(!count.row("active").contains("value"), "contradictory C report cannot requalify two-bank snapshot");
    count.observe("FL001");
    require(count.row("active").value("value").toInt() == 0, "coherent fresh active-bank report restores native identity without inferred default");
}
void contextAndReentry() {
    Fixture f; f.ready(); f.get(); f.readbacks();
    const QVariant oldContext = f.state.value("context");
    int edges = 0;
    QObject::connect(&f.backend, &IRadioBackend::sliceChanged, &f.backend, [&](int, const SliceDelta& delta) { if (delta.filterLow || delta.filterHigh) { ++edges; } });
    KenwoodBackendTestAccess::batch(f.backend, {"FL011", "SL010", "SH0005"});
    require(edges == 0 && KenwoodBackendTestAccess::axesPending(f.backend), "coalesced old-bank axes never publish and remain unavailable");
    f.commands.clear(); f.backend.setSliceFilter(0, 300, 2500);
    require(f.commands.isEmpty() && f.errors.contains(f.edit("bank0.afType", 2, false, oldContext)), "shared and stale native edits refused before fresh bank axes");
    f.observe("SL010"); f.observe("SH0005");
    require(edges == 1 && !KenwoodBackendTestAccess::axesPending(f.backend), "separate fresh bank axis readbacks restore shared qualification");
    f.observe("OM04"); f.observe("SF0000140740004"); f.get(); f.readbacks(true); f.commands.clear();
    require(!f.row("bank0.roofing").value("editable").toBool() && !f.row("bank0.ifShape").value("editable").toBool(), "FM roofing and IF shape are visible read-only");
    f.edit("bank0.roofing", 0); f.edit("bank0.ifShape", {}, true);
    require(f.commands.isEmpty(), "FM prevents setter and reset wire effects");
    f.observe("OM03"); f.observe("SF0000140740003"); f.get(); f.readbacks();
    bool once = false;
    const QMetaObject::Connection c = QObject::connect(&f.backend, &IRadioBackend::extensionStatus, &f.backend,
        [&](const QString&, const QString& kind, const QVariantMap&) {
            if (!once && kind == "filterBanks" && f.row("bank0.afType").value("pending").toBool()) { once = true; f.observe("FR1"); }
        });
    f.commands.clear(); f.edit("bank0.afType", 2);
    require(once && !f.commands.contains("FL302;"), "reentrant source switch cancels pending producer before native write");
    QObject::disconnect(c);
    f.observe("SF1000070740003"); f.get(); f.readbacks();
    const QVariant roleContext = f.state.value("context");
    f.observe("TS1"); f.commands.clear(); f.edit("bank0.afType", 2, false, roleContext);
    require(f.commands.isEmpty(), "TF-SET withdraws current bank write authority");
    f.observe("TS0"); f.get(); f.readbacks(); KenwoodBackendTestAccess::intent(f.backend, true);
    f.commands.clear(); f.edit("bank0.afType", 2);
    require(f.commands.isEmpty(), "local key intent withdraws bank write authority");
    KenwoodBackendTestAccess::intent(f.backend, false); f.get(); f.readbacks();
    const QVariant beforeTune = f.state.value("context");
    KenwoodBackendTestAccess::tuneIntent(f.backend, true); f.commands.clear(); f.edit("bank0.afType", 2, false, beforeTune);
    require(f.commands.isEmpty() && !f.row("bank0.afType").value("editable").toBool(), "pending tune preflight without key flag still withdraws bank authority");
    KenwoodBackendTestAccess::tuneIntent(f.backend, false);
    f.backend.disconnectRadio(); f.commands.clear(); f.edit("bank0.afType", 2);
    require(f.commands.isEmpty(), "disconnected bank edits cannot replay prior authority");
}
}

void verifyFilterBanksProducerChronology()
{
    using Backend = AetherSDR::kenwood::KenwoodBackend;
    using Probe = AetherSDR::kenwood::KenwoodBackendTestAccess;
    Backend backend(Backend::Transport{[](const QByteArray&) { return true; }, {}});
    const QVariant initial = Probe::chronologyStatus(backend).value("statusRevision");
    require(initial.typeId() == QMetaType::QString && initial.toString() == "0", "filterBanks initial getter has canonical QString revision zero");
    Probe::authenticate(backend);
    for (const QByteArray& bytes : {QByteArray("ID024"), QByteArray("FR0"), QByteArray("FA00014074000"), QByteArray("SF0000140740003"), QByteArray("TS0"), QByteArray("RX")}) { Probe::chronologyFeed(backend, bytes); }
    bool nested = false; QList<QVariantMap> delivered;
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionStatus, &backend,
        [&](const QString&, const QString& statusKind, const QVariantMap&) {
            if (statusKind != "filterBanks" || nested) { return; }
            nested = true; Probe::chronologyFeed(backend, "TS1");
        });
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionStatus, &backend,
        [&](const QString&, const QString& statusKind, const QVariantMap& state) {
            if (statusKind == "filterBanks") { delivered.append(state); }
        });
    Probe::chronologyPublish(backend);
    require(nested && delivered.size() >= 2, "filterBanks real TF withdrawal publishes newer status during outer delivery");
    const quint64 newer = delivered.first().value("statusRevision").toString().toULongLong();
    const quint64 older = delivered.last().value("statusRevision").toString().toULongLong();
    require(newer > older, "filterBanks producer stamps snapshot before nested newer delivery");
    for (const QVariantMap& state : delivered) {
        const QVariant revision = state.value("statusRevision");
        require(revision.typeId() == QMetaType::QString && revision.toString() == QString::number(revision.toString().toULongLong()), "filterBanks every revision is canonical decimal QString");
    }
    QVariantMap queried;
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionResult, &backend,
        [&](quint64 request, const QVariant& result) { if (request == 901) { queried = result.toMap(); } });
    backend.invokeExtension("kenwood", "filter.banks.get", 901);
    require(queried.value("statusRevision") == Probe::chronologyStatus(backend).value("statusRevision")
        && queried.value("statusRevision").toString().toULongLong() >= newer, "filterBanks GET carries latest producer revision after stale outer delivery");
    const quint64 beforeReset = queried.value("statusRevision").toString().toULongLong();
    backend.disconnectRadio(); Probe::chronologyPublish(backend);
    require(Probe::chronologyStatus(backend).value("statusRevision").toString().toULongLong() > beforeReset, "filterBanks disconnect never resets lifetime publication chronology");
}


void fmPrepare(Fixture& f, char mode, int receive, bool split, int width = 0)
{
    const int transmit = split ? 1 - receive : receive;
    f.observe("FR" + QByteArray::number(receive)); f.observe("FT" + QByteArray::number(transmit));
    f.observe("TS0"); f.observe("OM0" + QByteArray(1, mode)); f.observe("OM1" + QByteArray(1, mode));
    f.observe("SF000014074000" + QByteArray(1, mode)); f.observe("SF100007074000" + QByteArray(1, mode));
    f.observe("MV0"); f.observe("QR00");
    f.backend.invokeExtension("kenwood", "filter.fm.read", 800);
    const QList<QByteArray> replies{"FR" + QByteArray::number(receive), "FT" + QByteArray::number(transmit), "TS0", "MV0", "QR00",
        "SF000014074000" + QByteArray(1, mode), "SF100007074000" + QByteArray(1, mode), "OM0" + QByteArray(1, mode), "OM1" + QByteArray(1, mode),
        "FW0" + QByteArray::number(width)};
    for (const QByteArray& reply : replies) { f.observe(reply); }
    if (split) { f.observe("FW1" + QByteArray::number(1 - width)); }
    require(!KenwoodBackendTestAccess::fmStatus(f.backend).value("refresh").toMap().value("busy").toBool(), "FM bounded preparation finishes");
}

void fmWidthDomainAndTerminals()
{
    for (char mode : {'4', 'E'}) {
        for (int receive = 0; receive < 2; ++receive) {
            for (bool split : {false, true}) {
                for (int value = 0; value < 2; ++value) {
                    Fixture f; f.ready(); f.commands.clear(); fmPrepare(f, mode, receive, split);
                    const QVariantMap status = KenwoodBackendTestAccess::fmStatus(f.backend);
                    if (!status.value("editable").toBool()) { std::cerr << "FM case mode=" << mode << " receive=" << receive << " split=" << split << " value=" << value << " reason=" << status.value("reason").toString().toStdString() << '\n'; }
                    require(status.value("editable").toBool(), "FM/DFM both physical receive VFOs support owned width Apply");
                    require(f.commands.contains("FW1;") == split, "simplex skips right query; split permits right display query");
                    f.commands.clear();
                    f.backend.invokeExtension("kenwood", "filter.fm.set", 801, QVariantMap{{"revision", status.value("revision")}, {"value", value}});
                    require(f.commands.first() == (value ? "FW01;" : "FW00;"), "both native widths preserve fixed ignored setter selector");
                    require(!f.results.value(801).toMap().value("verified").toBool(), "write remains unverified before independent effect evidence");
                }
            }
        }
    }
    for (const QByteArray& malformed : {QByteArray("FW02"), QByteArray("FW0"), QByteArray("FW000"), QByteArray("FW0x")}) {
        Fixture f; f.ready(); fmPrepare(f, '4', 0, false); f.observe(malformed);
        const QVariantMap status = KenwoodBackendTestAccess::fmStatus(f.backend);
        require(!status.value("editable").toBool() && !status.value("displays").toList()[0].toMap().value("known").toBool(), "malformed current width withdraws native baseline");
    }
    for (const QByteArray& role : {QByteArray("TS1"), QByteArray("MV1"), QByteArray("QR10"), QByteArray("TX0"), QByteArray("OM03")}) {
        Fixture f; f.ready(); fmPrepare(f, '4', 0, false); const QVariantMap before = KenwoodBackendTestAccess::fmStatus(f.backend);
        f.observe(role); f.commands.clear();
        f.backend.invokeExtension("kenwood", "filter.fm.set", 802, QVariantMap{{"revision", before.value("revision")}, {"value", 1}});
        require(f.errors.contains(802) && f.commands.isEmpty(), "changed TF/memory/TX/mode context rejects stale width without wire");
    }
    {
        Fixture f; f.ready(); f.backend.invokeExtension("kenwood", "filter.fm.read", 803);
        KenwoodBackendTestAccess::fmPoll(f.backend, 1000000);
        require(!KenwoodBackendTestAccess::fmStatus(f.backend).value("refresh").toMap().value("busy").toBool(), "timeout terminates owned read without retry");
    }
    {
        Fixture f; f.ready(); KenwoodBackendTestAccess::fmTransport(f.backend, [](const QByteArray&) { return false; });
        f.backend.invokeExtension("kenwood", "filter.fm.read", 804);
        require(!KenwoodBackendTestAccess::fmStatus(f.backend).value("refresh").toMap().value("busy").toBool(), "transport refusal terminates read");
    }
    {
        Fixture f; f.ready(); fmPrepare(f, '4', 0, false); f.backend.disconnectRadio();
        const QVariantMap status = KenwoodBackendTestAccess::fmStatus(f.backend);
        require(!status.value("connected").toBool() && !status.value("editable").toBool() && !status.value("displays").toList()[0].toMap().value("known").toBool(), "disconnect clears native width authority");
    }
}

void fmWidthMalformedReentryAndStrictArguments()
{
    {
        Fixture f; f.ready(); fmPrepare(f, '4', 0, false);
        bool nested = false;
        QObject::connect(&f.backend, &IRadioBackend::extensionStatus, &f.backend, [&](const QString&, const QString& kind, const QVariantMap& status) {
            if (nested || kind != "fmWidth") { return; }
            const QVariantMap left = status.value("displays").toList()[0].toMap();
            if (!left.value("known").toBool() || left.value("value").toInt() != 1) { return; }
            nested = true; f.observe("FW0x");
        });
        KenwoodBackendTestAccess::batch(f.backend, {"FW01", "FW00"});
        require(nested && !KenwoodBackendTestAccess::fmStatus(f.backend).value("displays").toList()[0].toMap().value("known").toBool(), "newer malformed width cannot be revived by old coalesced remainder");
        f.observe("FW00");
        require(KenwoodBackendTestAccess::fmStatus(f.backend).value("displays").toList()[0].toMap().value("known").toBool()
            && !KenwoodBackendTestAccess::fmStatus(f.backend).value("editable").toBool(), "fresh native observation recovers fact without owned Apply authority");
    }
    for (char mode : QByteArray("1235679ABCDF")) {
        Fixture f; f.ready(); fmPrepare(f, mode, 0, false);
        require(!KenwoodBackendTestAccess::fmStatus(f.backend).value("editable").toBool(), "all twelve non-FM modes keep FW setter unavailable");
    }
    {
        Fixture f; f.ready(); fmPrepare(f, '4', 0, false);
        const QVariant revision = KenwoodBackendTestAccess::fmStatus(f.backend).value("revision");
        for (const QVariant& invalid : {QVariant(true), QVariant(1.0), QVariant("1"), QVariant(-1), QVariant(2)}) {
            f.commands.clear(); f.backend.invokeExtension("kenwood", "filter.fm.set", 806, QVariantMap{{"revision", revision}, {"value", invalid}});
            require(f.errors.contains(806) && f.commands.isEmpty(), "native width rejects coercion and out-of-domain values");
        }
        f.commands.clear(); f.backend.invokeExtension("kenwood", "filter.fm.set", 807, QVariantMap{{"revision", revision}, {"value", 1}, {"target", 1}});
        require(f.errors.contains(807) && f.commands.isEmpty(), "native width rejects invented target selector");
    }
}

void fmWidthDisplayEligibility()
{
    for (bool tf : {false, true}) {
        for (bool split : {false, true}) {
            for (bool rightFm : {false, true}) {
                Fixture f; f.ready();
                f.observe("OM04"); f.observe("SF0000140740004");
                f.commands.clear(); f.backend.invokeExtension("kenwood", "filter.fm.read", 808);
                for (const QByteArray& reply : QList<QByteArray>{"FR0", split ? "FT1" : "FT0", tf ? "TS1" : "TS0", "MV0", "QR00",
                    "SF0000140740004", "SF1000070740004", "OM04", rightFm ? "OM14" : "OM13"}) { f.observe(reply); }
                require(f.commands.contains("FW0;"), "FM left display always uses selector0");
                f.observe("FW00");
                const bool eligible = (tf || split) && rightFm;
                require(f.commands.contains("FW1;") == eligible, "right query requires split/TF and its own FM mode");
                if (eligible) { f.observe("FW11"); }
                const QVariantMap status = KenwoodBackendTestAccess::fmStatus(f.backend);
                require(!status.value("refresh").toMap().value("busy").toBool(), "conditional display read finishes without synthetic reply");
                if (tf) { require(!status.value("editable").toBool(), "TF permits eligible observation but never RX width Apply"); }
            }
        }
    }
}

void fmWidthPendingIntentAba()
{
    for (bool safety : {false, true}) {
        Fixture f; f.ready(); fmPrepare(f, '4', 0, true);
        const QVariantMap before = KenwoodBackendTestAccess::fmStatus(f.backend);
        bool nested = false;
        QObject::connect(&f.backend, &IRadioBackend::extensionStatus, &f.backend, [&](const QString&, const QString& kind, const QVariantMap& status) {
            if (nested || kind != "fmWidth" || status.value("lastWrite").toMap().value("state") != "pending") { return; }
            nested = true;
            if (safety) { KenwoodBackendTestAccess::fmIntent(f.backend, true); KenwoodBackendTestAccess::fmIntent(f.backend, false); }
            else { f.observe("FT0"); f.observe("FT1"); }
        });
        f.commands.clear(); f.backend.invokeExtension("kenwood", "filter.fm.set", 805, QVariantMap{{"revision", before.value("revision")}, {"value", 1}});
        require(nested && !f.commands.contains("FW01;") && f.errors.contains(805), "same-value role/safety ABA retires pending native intent");
    }
}

void fmWidthReadAndApply()
{
    Fixture f; f.ready();
    f.observe("OM04"); f.observe("OM14"); f.observe("SF0000140740004"); f.observe("SF1000070740004"); f.observe("MV0"); f.observe("QR00");
    f.commands.clear(); f.backend.invokeExtension("kenwood", "filter.fm.get", 700);
    require(f.commands.isEmpty(), "cached FM get is passive");
    const QMap<QByteArray, QByteArray> replies{{"FR;", "FR0"}, {"FT;", "FT1"}, {"TS;", "TS0"}, {"MV;", "MV0"}, {"QR;", "QR00"},
        {"SF0;", "SF0000140740004"}, {"SF1;", "SF1000070740004"}, {"OM0;", "OM04"}, {"OM1;", "OM14"}, {"FW0;", "FW00"}, {"FW1;", "FW11"}};
    f.backend.invokeExtension("kenwood", "filter.fm.read", 701);
    for (const QByteArray& key : {QByteArray("FR;"), QByteArray("FT;"), QByteArray("TS;"), QByteArray("MV;"), QByteArray("QR;"), QByteArray("SF0;"), QByteArray("SF1;"), QByteArray("OM0;"), QByteArray("OM1;"), QByteArray("FW0;"), QByteArray("FW1;")}) {
        require(f.commands.contains(key), "explicit FM read dispatches bounded native query in order"); f.observe(replies.value(key));
    }
    f.backend.invokeExtension("kenwood", "filter.fm.get", 702);
    const QVariantMap status = f.results.value(702).toMap();
    require(status.value("editable").toBool() && status.value("displays").toList()[0].toMap().value("value").toInt() == 0
        && status.value("displays").toList()[1].toMap().value("value").toInt() == 1, "owned split display observations permit active RX setter");
    f.commands.clear();
    f.backend.invokeExtension("kenwood", "filter.fm.set", 703, QVariantMap{{"revision", status.value("revision")}, {"value", 1}});
    require(f.commands.first() == "FW01;" && !f.commands.contains("OM04;") && !f.commands.contains("SL010;"), "FM setter uses fixed ignored selector and no mode/audio-cut emulation");
    require(f.results.value(703).toMap().value("verified").toBool() == false, "native FM write never invents effect verification");
}

void fmWidthUnsentAndContextFences()
{
    Fixture f; f.ready();
    bool cancelled = false;
    QObject::connect(&f.backend, &IRadioBackend::extensionStatus, &f.backend, [&](const QString&, const QString& kind, const QVariantMap& status) {
        if (kind != "fmWidth" || cancelled || status.value("refresh").toMap().value("pendingKey") != "FR") { return; }
        cancelled = true;
        f.observe("FR1");
        f.backend.invokeExtension("kenwood", "filter.fm.cancel", 711, QVariantMap{{"token", status.value("refresh").toMap().value("token")}});
    });
    f.commands.clear(); f.backend.invokeExtension("kenwood", "filter.fm.read", 710);
    require(cancelled && !f.commands.contains("FR;"), "predispatch bank/source fact cancels unsent read without query credit");
    f.backend.invokeExtension("kenwood", "filter.fm.get", 712);
    require(!f.results.value(712).toMap().value("editable").toBool(), "unowned source fact cannot authorize width Apply");
    f.commands.clear(); f.backend.invokeExtension("kenwood", "filter.fm.set", 713, QVariantMap{{"revision", f.results.value(712).toMap().value("revision")}, {"value", true}});
    require(f.commands.isEmpty() && f.errors.contains(713), "FM setter rejects bool coercion and unowned baseline");
}

int main(int argc, char** argv) {
    TestSettingsProfile profile(QStringLiteral("kenwood-filter-banks"));
    QCoreApplication app(argc, argv);
    verifyFilterBanksProducerChronology(); require(profile.isValid(), "isolated settings");
    fmWidthReadAndApply(); fmWidthUnsentAndContextFences(); fmWidthDomainAndTerminals(); fmWidthPendingIntentAba(); fmWidthMalformedReentryAndStrictArguments(); fmWidthDisplayEligibility();
    parsingAndAdmission(); choicesAndResets(); allNativeAxisModes(); predispatchOldReports(); reentrantConsumerQueries(); pendingAxisWindow(); contextAndReentry(); return 0;
}
