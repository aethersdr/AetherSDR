// The VITA-49 sequence-error log line (#6285): its rate limit and its text,
// then the same line from PanadapterStream itself. Socket-free: the limiter
// takes explicit timestamps, and meter packets are fed straight to
// processDatagram() on a stream that never binds. The prefix is what the docs
// Log Analyzer rule vita-sequence-errors matches.

#include "core/backends/flex/PanadapterStream.h"
#include "core/backends/flex/VitaSequenceLossReport.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QLoggingCategory>
#include <QString>
#include <QStringList>
#include <QtEndian>

#include <cstdio>

namespace AetherSDR {
class VitaSequenceLossLogTestAccess {
public:
    // A meter packet: header word0 carries the 4-bit sequence count, word3's
    // low 16 bits the packet class code; one meter id/value pair as payload.
    static void feedMeter(PanadapterStream& stream, quint32 streamId, int seq)
    {
        QByteArray packet(PanadapterStream::VITA49_HEADER_BYTES + 4, '\0');
        auto* raw = reinterpret_cast<uchar*>(packet.data());
        qToBigEndian<quint32>(0x18000000u | (quint32(seq & 0x0F) << 16), raw);
        qToBigEndian<quint32>(streamId, raw + 4);
        qToBigEndian<quint32>(PanadapterStream::PCC_METER, raw + 12);
        qToBigEndian<quint16>(1, raw + PanadapterStream::VITA49_HEADER_BYTES);
        stream.processDatagram(packet);
    }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {

QStringList g_warnings;

void capture(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (type == QtWarningMsg) {
        g_warnings << QString::fromLatin1(ctx.category ? ctx.category : "") + ": " + msg;
    }
}

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    {
        VitaSequenceLossLimiter l;
        l.start(1000);
        report("first error reports at once", l.recordError(4000));
        report("first report counts one error", l.reportErrors() == 1);
        report("first window runs from the first packet", l.reportWindowMs() == 3000);

        int lines = 0;
        for (qint64 t = 4001; t < 14000; t += 10) {
            if (l.recordError(t)) {
                ++lines;
            }
        }
        report("no second line inside the interval", lines == 0);

        report("the first error after the interval reports", l.recordError(14000));
        report("it carries every error held back plus itself", l.reportErrors() == 1001);
        report("its window is the interval", l.reportWindowMs() == 10000);
    }

    {
        // A steady error stream yields one line per interval, never one per packet.
        VitaSequenceLossLimiter l;
        l.start(0);
        int lines = 0;
        int counted = 0;
        for (qint64 t = 0; t <= 60000; t += 5) {
            if (l.recordError(t)) {
                ++lines;
                counted += l.reportErrors();
            }
        }
        report("one line per 10 s over a minute", lines == 7);
        report("lines account for every error up to the last report", counted == 12001);
    }

    {
        // An isolated error long after the previous report still reports.
        VitaSequenceLossLimiter l;
        l.start(0);
        l.recordError(100);
        report("isolated later error reports", l.recordError(500000));
        report("isolated later error counts one", l.reportErrors() == 1);
    }

    {
        // A stream re-created inside the interval: the rate limit holds across
        // the restart, and errors held from the old instance are dropped.
        VitaSequenceLossLimiter l;
        l.start(0);
        report("restart: first instance reports", l.recordError(100));
        l.recordError(200);  // held
        l.start(2000);
        report("restart: a new instance's error inside the interval is held", !l.recordError(3000));
        report("restart: the next report is due after the interval", l.recordError(10100));
        report("restart: it counts only the new instance's errors", l.reportErrors() == 2);
        report("restart: its window runs from the new instance", l.reportWindowMs() == 8100);
    }

    {
        // A stream that never saw start() still reports a sane window.
        VitaSequenceLossLimiter l;
        report("no start(): reports", l.recordError(7000));
        report("no start(): zero window", l.reportWindowMs() == 0);
    }

    {
        const QString line = formatVitaSequenceLoss(QStringLiteral("FFT"), 0x40000000u,
                                                    12, 10400, 31, 52310);
        report("line text",
               line == QStringLiteral("PanadapterStream: VITA-49 sequence errors on FFT stream "
                                      "0x40000000: 12 in the last 10 s, 31 since the stream "
                                      "started (52310 packets received)"));
        report("stream id padded to 8 hex digits, as the radio lists it",
               formatVitaSequenceLoss(QStringLiteral("audio"), 0x04000008u, 1, 0, 1, 2)
                   .contains(QStringLiteral(" stream 0x04000008: ")));
        report("analyzer prefix",
               line.startsWith(QStringLiteral("PanadapterStream: VITA-49 sequence errors on ")));
    }

    {
        // The product's default rules: aether.vita49 is declared QtWarningMsg,
        // so the line must be a warning to reach a default log.
        QLoggingCategory::setFilterRules(QStringLiteral("aether.*.debug=false"));
        qInstallMessageHandler(capture);
        PanadapterStream stream;  // no init(), no socket
        const quint32 id = 0x7000000Au;
        for (int seq = 0; seq < 4; ++seq) {
            VitaSequenceLossLogTestAccess::feedMeter(stream, id, seq);
        }
        report("in-order packets log nothing", g_warnings.isEmpty());
        VitaSequenceLossLogTestAccess::feedMeter(stream, id, 7);  // 4..6 missing
        report("a gap logs one warning", g_warnings.size() == 1);
        report("on aether.vita49 with the stream, category and counts",
               g_warnings.value(0) == QStringLiteral(
                   "aether.vita49: PanadapterStream: VITA-49 sequence errors on meter stream "
                   "0x7000000a: 1 in the last 0 s, 1 since the stream started (5 packets received)"));
        for (int i = 0; i < 50; ++i) {
            VitaSequenceLossLogTestAccess::feedMeter(stream, id, i * 3);
        }
        report("a burst of gaps inside 10 s adds no line", g_warnings.size() == 1);
        report("the live counter still has every error",
               stream.categoryStats(PanadapterStream::CatMeter).errors > 40);
        qInstallMessageHandler(nullptr);
    }

    {
        // Disconnect, then a reconnect whose stream reuses the id. The first
        // packet is measured against nothing, not against the previous
        // session's last count, and the line's counts start over. A fresh
        // stream, so the limiter has no recent report that would hide a false
        // line.
        g_warnings.clear();
        qInstallMessageHandler(capture);
        PanadapterStream stream;  // no init(), no socket
        const quint32 id = 0x7000000Bu;
        for (int seq = 0; seq < 3; ++seq) {
            VitaSequenceLossLogTestAccess::feedMeter(stream, id, seq);
        }
        const int totalBefore = stream.packetTotalCount();
        stream.clearRegisteredStreams();
        VitaSequenceLossLogTestAccess::feedMeter(stream, id, 0);  // previous session ended at 2
        report("reconnect: a reused stream id logs no sequence error", g_warnings.isEmpty());
        report("reconnect: no error counted",
               stream.packetErrorCount() == 0
                   && stream.categoryStats(PanadapterStream::CatMeter).errors == 0);
        report("reconnect: the cumulative packet total is kept",
               stream.packetTotalCount() == totalBefore + 1);
        VitaSequenceLossLogTestAccess::feedMeter(stream, id, 1);
        VitaSequenceLossLogTestAccess::feedMeter(stream, id, 5);  // 2..4 missing
        report("reconnect: a real gap still logs at once",
               g_warnings.size() == 1
                   && g_warnings.value(0) == QStringLiteral(
                          "aether.vita49: PanadapterStream: VITA-49 sequence errors on meter stream "
                          "0x7000000b: 1 in the last 0 s, 1 since the stream started (3 packets received)"));

        // A pan or waterfall closed and re-created with the same id restarts too.
        // The limiter reported just above, so it would hold a line back: the
        // error count is what shows the restart.
        const int errorsBefore = stream.packetErrorCount();
        stream.unregisterPanStream(id);
        VitaSequenceLossLogTestAccess::feedMeter(stream, id, 9);  // previous instance ended at 5
        report("unregister pan: a re-created stream counts no sequence error",
               stream.packetErrorCount() == errorsBefore);
        stream.unregisterWfStream(id);
        VitaSequenceLossLogTestAccess::feedMeter(stream, id, 2);  // previous instance ended at 9
        report("unregister waterfall: a re-created stream counts no sequence error",
               stream.packetErrorCount() == errorsBefore);
        stream.unregisterDaxStream(id);
        VitaSequenceLossLogTestAccess::feedMeter(stream, id, 12);
        report("unregister DAX: a re-created stream counts no sequence error",
               stream.packetErrorCount() == errorsBefore);
        stream.unregisterIqStream(id);
        VitaSequenceLossLogTestAccess::feedMeter(stream, id, 4);
        report("unregister IQ: a re-created stream counts no sequence error",
               stream.packetErrorCount() == errorsBefore);
        qInstallMessageHandler(nullptr);
    }

    std::printf("%s\n", g_failed ? "FAILED" : "PASSED");
    return g_failed ? 1 : 0;
}
