#include <qt6keychain/keychain.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <QRegularExpression>
#include <QTimer>
#include <cstdio>
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) { return 2; }
    const QString action = QString::fromLatin1(argv[1]);
    if (action != "read" && action != "create") { return 2; }
    const QString service = QStringLiteral("AetherPD development publisher");
    const QString key = QStringLiteral("flex8600-bench-20261116");
    QKeychain::ReadPasswordJob read(service);
    read.setAutoDelete(false); read.setInsecureFallback(false); read.setKey(key);
    QEventLoop loop;
    QObject::connect(&read, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    QTimer timer; timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &app, [] { std::_Exit(3); });
    timer.start(15000); read.start(); loop.exec();
    if (action == "read") {
        if (read.error() == QKeychain::EntryNotFound) { return 10; }
        if (read.error() != QKeychain::NoError) { return 4; }
        const QByteArray seed = read.textData().toLatin1();
        if (!QRegularExpression("^[0-9a-f]{64}$").match(QString::fromLatin1(seed)).hasMatch()) { return 5; }
        return std::fwrite(seed.constData(), 1, seed.size(), stdout) == 64 ? 0 : 6;
    }
    if (read.error() != QKeychain::EntryNotFound) { return 7; }
    char input[66] = {};
    if (!std::fgets(input, sizeof(input), stdin)) { return 8; }
    const QString seed = QString::fromLatin1(input).trimmed();
    if (!QRegularExpression("^[0-9a-f]{64}$").match(seed).hasMatch()) { return 9; }
    QKeychain::WritePasswordJob write(service);
    write.setAutoDelete(false); write.setInsecureFallback(false); write.setKey(key); write.setTextData(seed);
    QObject::connect(&write, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    write.start(); loop.exec();
    return write.error() == QKeychain::NoError ? 0 : 11;
}
