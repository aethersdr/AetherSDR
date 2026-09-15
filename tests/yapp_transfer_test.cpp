// Published YAPP 1.1 / FC1EBN YAPP-C byte vectors, with injected application
// bytes and temporary files. No sockets, radio emulation, or RF emissions.
#include "core/tnc/YappTransferSession.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>

using namespace AetherSDR;
static int failures = 0;
static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (false)
static QByteArray hex(const char* s) { return QByteArray::fromHex(s); }
static bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
static QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
static QByteArray header(const QByteArray& name, qint64 size)
{
    QByteArray fields = name + '\0' + QByteArray::number(size) + '\0' + "5B2E6400" + '\0';
    return hex("01") + char(fields.size()) + fields;
}
static QByteArray block(const QByteArray& data)
{
    return hex("02") + char(data.size() & 255) + data + char(YappTransferSession::checksum(data));
}
static void arm(YappTransferSession& r, const QString& dir, qint64 size)
{
    QString error;
    CHECK(r.startReceive(dir, QStringLiteral("N0AAA"), true, error));
    r.receive(hex("0501"));
    CHECK(r.takeOutbound() == hex("0601"));
    r.receive(header("test.bin", size));
}
static void receiverVectors()
{
    QTemporaryDir dir;
    YappTransferSession r;
    QByteArray data;
    for (int i = 0; i < 256; ++i) { data.append(char(i)); }
    arm(r, dir.path(), data.size());
    CHECK(r.takeOutbound() == hex("0606"));
    const QByteArray packet = block(data);
    CHECK(packet.size() == 259);
    CHECK(static_cast<unsigned char>(packet.back()) == 0x80);
    for (char c : packet) { r.receive(QByteArray(1, c)); }
    CHECK(r.snapshot().value("bytes").toInt() == 256);
    CHECK(!QFile::exists(dir.filePath("test.bin")));
    r.receive(hex("0301"));
    CHECK(r.takeOutbound() == hex("0603"));
    CHECK(readFile(dir.filePath("test.bin")) == data);
    r.receive(hex("0401"));
    CHECK(r.takeOutbound() == hex("0604"));
    CHECK(r.active()); // final bytes still need transport delivery
    CHECK(r.takeOutbound().isEmpty());
    CHECK(r.succeeded());
    CHECK(!r.active());
}
static void allFragmentBoundaries()
{
    QByteArray data;
    for (int i = 0; i < 256; ++i) { data.append(char(i)); }
    const QByteArray packet = block(data);
    for (int split = 0; split <= packet.size(); ++split) {
        QTemporaryDir dir;
        YappTransferSession r;
        arm(r, dir.path(), data.size());
        CHECK(r.takeOutbound() == hex("0606"));
        r.receive(packet.left(split));
        r.receive(packet.mid(split) + hex("03010401"));
        CHECK(r.takeOutbound() == hex("0603"));
        CHECK(r.takeOutbound() == hex("0604"));
        r.takeOutbound();
        CHECK(r.succeeded());
        CHECK(readFile(dir.filePath("test.bin")) == data);
    }
}
static void senderVectors()
{
    QTemporaryDir dir;
    CHECK(writeFile(dir.filePath("out.bin"), QByteArray("ABC")));
    YappTransferSession s;
    QString error;
    CHECK(s.startSend(dir.filePath("out.bin"), "N0BBB", error));
    CHECK(s.takeOutbound() == hex("0501"));
    s.receive(hex("0601"));
    const QByteArray h = s.takeOutbound();
    CHECK(h[0] == 1 && h.mid(2).startsWith(QByteArray("out.bin\0" "3\0", 10)));
    s.receive(hex("0606"));
    CHECK(s.takeOutbound() == hex("0203414243C6"));
    CHECK(s.takeOutbound() == hex("0301"));
    CHECK(s.takeOutbound().isEmpty());
    CHECK(!s.succeeded());
    s.receive(hex("0603"));
    CHECK(s.takeOutbound() == hex("0401"));
    CHECK(s.snapshot().value("fileAccepted").toBool());
    s.receive(hex("0604"));
    s.takeOutbound();
    CHECK(s.succeeded());
}
static void resumeAfterRestart()
{
    QTemporaryDir dir;
    {
        YappTransferSession r;
        arm(r, dir.path(), 6);
        CHECK(r.takeOutbound() == hex("0606"));
        r.receive(block("ABC"));
        r.stop("link lost");
        CHECK(!r.succeeded());
    }
    {
        YappTransferSession r;
        arm(r, dir.path(), 6);
        CHECK(r.takeOutbound() == hex("1506520033004300"));
        CHECK(r.snapshot().value("resumeOffset").toInt() == 3);
        r.receive(block("DEF") + hex("03010401"));
        CHECK(r.takeOutbound() == hex("0603"));
        CHECK(r.takeOutbound() == hex("0604"));
        r.takeOutbound();
        CHECK(r.succeeded());
        CHECK(readFile(dir.filePath("test.bin")) == "ABCDEF");
    }
}
static void resumeSenderAndRefusals()
{
    QTemporaryDir dir;
    CHECK(writeFile(dir.filePath("out.bin"), "ABCDEF"));
    for (const QByteArray& reply : {hex("1506520033004300"), hex("1506520037004300"),
                                   hex("150652002D31004300"), hex("0602")}) {
        YappTransferSession s;
        QString error;
        CHECK(s.startSend(dir.filePath("out.bin"), "N0BBB", error));
        s.takeOutbound(); s.receive(hex("0601")); s.takeOutbound(); s.receive(reply);
        const QByteArray packet = s.takeOutbound();
        if (reply == hex("1506520033004300")) {
            CHECK(packet == block("DEF"));
            CHECK(s.snapshot().value("resumeOffset").toInt() == 3);
        } else {
            CHECK(!packet.isEmpty() && packet[0] == 24);
            CHECK(!s.succeeded());
        }
    }
}
static void rejectionVectors()
{
    for (int which = 0; which < 6; ++which) {
        QTemporaryDir dir;
        YappTransferSession r;
        arm(r, dir.path(), 3);
        r.takeOutbound();
        if (which == 0) { r.receive(hex("020341424300")); } // corrupt checksum
        if (which == 1) { r.receive(block("ABCD")); } // exceeds announced size
        if (which == 2) { r.receive(hex("0301")); } // short file
        if (which == 3) { r.receive(hex("0606")); } // wrong phase
        if (which == 4) { r.receive(hex("FF00")); } // bad type
        if (which == 5) { r.cancel(); }
        const QByteArray cancel = r.takeOutbound();
        CHECK(!cancel.isEmpty() && cancel[0] == 24);
        r.receive(hex("0605")); r.takeOutbound();
        CHECK(!r.active()); CHECK(!r.succeeded());
        CHECK(!QFile::exists(dir.filePath("test.bin")));
    }
    for (const QByteArray& name : {QByteArray("../bad"), QByteArray("/tmp/bad"),
                                 QByteArray("CON.txt"), QByteArray("a:b"), QByteArray("x.")}) {
        QTemporaryDir dir;
        YappTransferSession r;
        QString error;
        CHECK(r.startReceive(dir.path(), "N0AAA", true, error));
        r.receive(hex("0501")); r.takeOutbound(); r.receive(header(name, 0));
        CHECK(r.takeOutbound().startsWith(hex("18")));
        CHECK(!r.succeeded());
    }
}
static void emptyFileAndLostFinalAck()
{
    QTemporaryDir dir;
    YappTransferSession r;
    arm(r, dir.path(), 0); CHECK(r.takeOutbound() == hex("0606"));
    r.receive(hex("0301")); CHECK(r.takeOutbound() == hex("0603"));
    CHECK(QFileInfo(dir.filePath("test.bin")).size() == 0);
    r.checkTimeout(0);
    CHECK(!r.succeeded());
    CHECK(r.snapshot().value("fileAccepted").toBool());
    CHECK(r.summary().contains("saved:"));
}
static void storageAndSourceFailures()
{
    QTemporaryDir dir;
    {
        YappTransferSession r;
        arm(r, dir.path(), 6); r.takeOutbound(); r.receive(block("ABC")); r.stop("lost");
    }
    const QStringList partials = QDir(dir.path()).entryList({"*.part"}, QDir::Files | QDir::Hidden);
    CHECK(partials.size() == 1);
    if (!partials.isEmpty()) { CHECK(writeFile(dir.filePath(partials.first()), "XYZ")); }
    YappTransferSession r;
    arm(r, dir.path(), 6);
    CHECK(r.takeOutbound().startsWith(hex("18"))); // modified checkpoint refused
    QTemporaryDir source;
    const QString path = source.filePath("out.bin");
    CHECK(writeFile(path, "ABC"));
    YappTransferSession s; QString error;
    CHECK(s.startSend(path, "N0BBB", error));
    s.takeOutbound(); s.receive(hex("0601")); s.takeOutbound(); s.receive(hex("0606"));
    CHECK(writeFile(path, "ABCD"));
    CHECK(s.takeOutbound().startsWith(hex("18")));

    QTemporaryDir failDir;
    YappTransferSession w;
    arm(w, failDir.path(), 3); w.takeOutbound();
    const QStringList metadata = QDir(failDir.path()).entryList({"*.json"}, QDir::Files | QDir::Hidden);
    CHECK(metadata.size() == 1);
    if (!metadata.isEmpty()) {
        const QString metadataPath = failDir.filePath(metadata.first());
        CHECK(QFile::remove(metadataPath)); CHECK(QDir().mkdir(metadataPath));
        w.receive(block("ABC"));
        CHECK(w.takeOutbound().startsWith(hex("18")));
        CHECK(!QFile::exists(failDir.filePath("test.bin")));
    }
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    receiverVectors(); allFragmentBoundaries(); senderVectors(); resumeAfterRestart();
    resumeSenderAndRefusals(); rejectionVectors(); emptyFileAndLostFinalAck(); storageAndSourceFailures();
    std::printf("YAPP-C: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
