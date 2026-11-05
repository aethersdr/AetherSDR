#include "core/ProfileTransfer.h"
#include "core/ZipArchive.h"
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest>

namespace AetherSDR {
class ProfileTransferTestAccess {
public:
    static bool commit(ProfileTransfer& transfer, const QString& path, const QByteArray& data, QString* error)
    {
        transfer.m_path = path;
        transfer.m_saveFile = new QTemporaryFile(&transfer);
        if (!transfer.m_saveFile->open() || transfer.m_saveFile->write(data) != data.size()) { return false; }
        return transfer.commitExportFile(error);
    }
};
}
using namespace AetherSDR;
class ProfileExportValidationTest final : public QObject {
    Q_OBJECT
private slots:
    void preserveDestinationOnInvalidOrTruncatedZip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("profile.ssdr_cfg"));
        const QByteArray valid = writeStoredZip({{QStringLiteral("meta_data"),"settings"},
                                                {QStringLiteral("flex_payload"),"data"}});
        for (const QByteArray& invalid : {QByteArray("invalid"), valid.left(valid.size()-12), QByteArray()}) {
            QFile original(path); QVERIFY(original.open(QIODevice::WriteOnly));
            QCOMPARE(original.write("previous"),8); original.close();
            ProfileTransfer transfer(nullptr);
            QString error;
            QVERIFY(!ProfileTransferTestAccess::commit(transfer,path,invalid,&error));
            QVERIFY(!error.isEmpty());
            QVERIFY(original.open(QIODevice::ReadOnly));
            QCOMPARE(original.readAll(),QByteArray("previous"));
        }
        ProfileTransfer transfer(nullptr);
        QString error;
        QVERIFY(ProfileTransferTestAccess::commit(transfer,path,valid,&error));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(),valid);
    }
};
QTEST_GUILESS_MAIN(ProfileExportValidationTest)
#include "profile_export_validation_test.moc"
