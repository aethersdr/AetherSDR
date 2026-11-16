#include "TestSettingsProfile.h"
#include "AetherPDPackageFixture.h"
#include "core/AetherPDInstallationCoordinator.h"
#include "core/AetherPDInstallationJournal.h"
#include "core/RadioSettingsScope.h"
#include "core/SettingsDatabase.h"
#include "core/SettingsPaths.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QtTest>

using namespace AetherSDR;
using Journal = AetherPDInstallationJournal;
class InstallationJournalTest : public QObject {
    Q_OBJECT
    const RadioSettingsScope m_scope{"flex", "fixture-8600"};
private slots:
    void init() { m_scope.removeFeature("AetherPDInstallation"); }
    void pendingSurvivesReloadAndCannotBeReplaced()
    {
        AetherPDTest::PackageFixture fixture; const auto package = fixture.loadFirst(); QVERIFY(package);
        const auto first = AetherPDInstallationCoordinator::prepareFirst(*package, "fixture-8600", QByteArray(32, 'a'), "fixture-radio");
        const auto second = AetherPDInstallationCoordinator::prepareFirst(*package, "fixture-8600", QByteArray(32, 'b'), "fixture-radio");
        QVERIFY(first); QVERIFY(second);
        const QByteArray key = fixture.trust.candidateKey;
        QCOMPARE(Journal::read(m_scope, key).state, Journal::State::Missing);
        QVERIFY(Journal::record(m_scope, first->approvedPlan(), key));
        QVERIFY(Journal::record(m_scope, first->approvedPlan(), key));
        QVERIFY(!Journal::record(m_scope, second->approvedPlan(), key));
        QVERIFY(!Journal::complete(m_scope, second->approvedPlan(), key));
        AppSettings::instance().load();
        QCOMPARE(Journal::read(m_scope, key).state, Journal::State::Pending);
        QCOMPARE(Journal::read(m_scope, key).plan, first->approvedPlan());
        QVERIFY(Journal::complete(m_scope, first->approvedPlan(), key));
        AppSettings::instance().load();
        QCOMPARE(Journal::read(m_scope, key).state, Journal::State::CredentialSaved);
        QVERIFY(Journal::record(m_scope, second->approvedPlan(), key));
        QCOMPARE(Journal::read(m_scope, key).state, Journal::State::Pending);
    }
    void changedTrustTargetOrSchemaFailsClosed()
    {
        AetherPDTest::PackageFixture fixture; const auto package = fixture.loadFirst(); QVERIFY(package);
        const auto prepared = AetherPDInstallationCoordinator::prepareFirst(*package, "fixture-8600", QByteArray(32, 'a'), "fixture-radio"); QVERIFY(prepared);
        const QByteArray plan = prepared->approvedPlan(), key = fixture.trust.candidateKey;
        QVERIFY(!Journal::record({"flex", "another-radio"}, plan, key));
        QVERIFY(!Journal::record({"flex", ""}, plan, key));
        QVERIFY(Journal::record(m_scope, plan, key));
        const QJsonObject stored = m_scope.featureExact("AetherPDInstallation");
        QCOMPARE(Journal::read(m_scope, QByteArray(32, 'z')).state, Journal::State::Invalid);
        QVERIFY(!Journal::complete(m_scope, plan, QByteArray(32, 'z')));
        QVERIFY(m_scope.setFeature("AetherPDInstallation", 2, stored));
        QVERIFY(!Journal::record(m_scope, plan, key));
        int schema = 0; QCOMPARE(m_scope.featureExact("AetherPDInstallation", &schema), stored); QCOMPARE(schema, 2);
        QJsonObject malformed = stored; malformed.insert("plan", QString(616, u'Z'));
        QVERIFY(m_scope.setFeature("AetherPDInstallation", 1, malformed));
        QCOMPARE(Journal::read(m_scope, key).state, Journal::State::Invalid);
        QVERIFY(!Journal::record(m_scope, plan, key));
        QCOMPARE(m_scope.featureExact("AetherPDInstallation"), malformed);
    }
    void staleWriterCannotReplaceAnotherConnectionsRecord()
    {
        const QJsonObject first{{"attempt", "first"}}, second{{"attempt", "second"}}, stale{{"attempt", "stale"}};
        QVERIFY(m_scope.compareAndSetFeature("AetherPDInstallation", 0, std::nullopt, 1, first));
        QVERIFY(!m_scope.compareAndSetFeature("AetherPDInstallation", 0, std::nullopt, 1, stale));
        SettingsDatabase other;
        QVERIFY(other.open(SettingsPaths::databasePath()));
        QVERIFY(other.upsertRadioFeature("flex", "fixture-8600", "AetherPDInstallation", 1,
            QString::fromUtf8(QJsonDocument(second).toJson(QJsonDocument::Compact))));
        QVERIFY(!m_scope.compareAndSetFeature("AetherPDInstallation", 1, first, 1, stale));
        QCOMPARE(m_scope.featureExact("AetherPDInstallation"), second);
        QVERIFY(!m_scope.compareAndSetFeature("AetherPDInstallation", 2, second, 1, stale));
        QVERIFY(m_scope.compareAndSetFeature("AetherPDInstallation", 1, second, 1, first));
        QCOMPARE(m_scope.featureExact("AetherPDInstallation"), first);
    }
};
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aetherpd-journal-test"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    InstallationJournalTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "aetherpd_installation_journal_test.moc"
