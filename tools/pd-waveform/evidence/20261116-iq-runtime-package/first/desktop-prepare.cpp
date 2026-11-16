#include "first-selection.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
using namespace AetherSDR;
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) { return 2; }
    const auto package = aetherpd::firstDevelopment::load();
    if (!package) { return 3; }
    const auto upload = package->prepare("fixture-8600", QByteArray(32, 't'), "fixture-radio", QByteArray(32, 'n'));
    if (!upload) { return 4; }
    const auto repeat = package->prepare("fixture-8600", QByteArray(32, 't'), "fixture-radio", QByteArray(32, 'n'));
    if (!repeat || upload->nativeArchive != repeat->nativeArchive || upload->carrier.archive != repeat->carrier.archive) { return 5; }
    const auto other = package->prepare("fixture-8600", QByteArray(32, 't'), "fixture-radio", QByteArray(32, 'm'));
    if (!other || other->plan == upload->plan || other->carrier.archive == upload->carrier.archive || other->nativeArchive == upload->nativeArchive) { return 6; }
    const QDir out(QString::fromLocal8Bit(argv[1]));
    if (!out.exists()) { return 7; }
    const auto write = [&](const QString& name, const QByteArray& data) {
        QFile file(out.filePath(name));
        return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(data) == data.size();
    };
    if (!write("plan", upload->plan) || !write("native.ssdr_waveform", upload->nativeArchive)
        || !write("carrier.tar.gz", upload->carrier.archive) || !write("other-plan", other->plan)) { return 8; }
    const auto hash = [](const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    const QJsonObject report{{"loaded_with_compiled_pins", true}, {"prepare_repeat_identical", true},
        {"different_attempt_changes_both_uploads", true}, {"native_bytes", upload->nativeArchive.size()},
        {"carrier_bytes", upload->carrier.archive.size()}, {"native_sha256", hash(upload->nativeArchive)},
        {"carrier_sha256", hash(upload->carrier.archive)}, {"plan_sha256", hash(upload->plan)},
        {"synthetic_identity", true}, {"uploaded", false}};
    if (!write("report.json", QJsonDocument(report).toJson())) { return 9; }
    std::puts("Actual first-install package authenticated and prepared by desktop code; no upload.");
    return 0;
}
