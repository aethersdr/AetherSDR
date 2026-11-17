#include "AetherPDInstallationModel.h"
#include "RadioModel.h"
#include "AetherPDDevelopmentSelection.h"
#include "AetherPDFirstDevelopmentSelection.h"
#include "core/AetherPDProtocol.h"
#include "core/AetherPDTransportPolicy.h"
#include "core/AetherPDInstallationMonitor.h"
#include "core/TailnetShimTokenStore.h"
#include "core/TailnetShimClient.h"
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>
#include <utility>

namespace AetherSDR {
namespace {
AetherPDInstallationModel::RecoveryServices recoveryServices(RadioModel& radio)
{
    const QPointer<RadioModel> guarded(&radio);
    return {
        [guarded](const QByteArray& plan, const QByteArray& key, bool completed) {
            if (!guarded) { return false; }
            return completed ? AetherPDInstallationJournal::complete(guarded->settingsScope(), plan, key)
                             : AetherPDInstallationJournal::record(guarded->settingsScope(), plan, key);
        },
        [guarded](const QByteArray& key) { return guarded ? AetherPDInstallationJournal::read(guarded->settingsScope(), key) : AetherPDInstallationJournal::Read{}; },
        [](const QString& serial, const QString& attempt, QObject* context, std::function<void(const QString&)> done) {
            TailnetShimTokenStore::load(serial + u'/' + attempt, context, std::move(done), TailnetShimTokenStore::Entry::AetherPDFirst);
        },
        [] { return std::make_unique<AetherPDInstallationMonitor>(); },
        [guarded](const QByteArray& plan, const QByteArray& key) {
            return guarded && AetherPDInstallationJournal::retainPending(guarded->settingsScope(), plan, key);
        }
    };
}
} // namespace
AetherPDInstallationModel::AetherPDInstallationModel(RadioModel& radio, QObject* parent)
    : AetherPDInstallationModel(radio,
        [](const QString& serial, QObject* context, std::function<void(const QString&)> done) {
            TailnetShimTokenStore::load(serial, context, std::move(done));
        }, [](RadioModel& model) { return std::make_unique<Coordinator>(model); }, parent, {},
        [](const QString& serial, const QString& attempt, const QString& token, QObject* context, std::function<void(bool)> done) {
            TailnetShimTokenStore::save(attempt.isEmpty() ? serial : serial + u'/' + attempt, token, context,
                [done = std::move(done)](bool persisted, const QString&) { done(persisted); },
                attempt.isEmpty() ? TailnetShimTokenStore::Entry::TailnetShim : TailnetShimTokenStore::Entry::AetherPDFirst);
        }, recoveryServices(radio)) {}
AetherPDInstallationModel::AetherPDInstallationModel(RadioModel& radio, TokenLoader loader, Factory factory, QObject* parent, DownloadFactory downloader, TokenWriter writer, RecoveryServices recovery)
    : QObject(parent), m_radio(&radio), m_load(std::move(loader)), m_write(std::move(writer)), m_recovery(std::move(recovery)), m_factory(std::move(factory)),
      m_downloadFactory(downloader ? std::move(downloader) : [] { return std::make_unique<AetherPDPackageDownload>(); })
{
    m_timeout.setParent(this);
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(10000);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (m_state == State::LoadingCredential) { unavailable(Failure::CredentialTimeout); }
        else if (m_state == State::SavingCredential) { unavailable(Failure::CredentialTimeout); }
        else if (m_state == State::PromotingCredential) { promoted(false, m_generation); }
        else if (m_state == State::LoadingRecoveryCredential) { unavailable(Failure::CredentialTimeout); }
    });
    connect(&radio, &RadioModel::connectionStateChanged, this, &AetherPDInstallationModel::reconcile);
    connect(&radio, &RadioModel::infoChanged, this, &AetherPDInstallationModel::reconcile);
    connect(&radio, &QObject::destroyed, this, [this] { m_radio = nullptr; reconcile(); });
}
AetherPDInstallationModel::~AetherPDInstallationModel()
{
    retirePreparation();
    m_timeout.stop();
    if (m_job) { disconnect(m_job.get(), nullptr, this, nullptr); }
    if (m_recoveryMonitor) { disconnect(m_recoveryMonitor.get(), nullptr, this, nullptr); }
    m_job.reset();
}
bool AetherPDInstallationModel::active() const
{
    return m_state == State::Downloading || m_state == State::Preparing || m_state == State::LoadingCredential
        || m_state == State::Running || m_state == State::Canceling || m_state == State::SavingCredential
        || m_state == State::PromotingCredential || m_state == State::LoadingRecoveryCredential || m_state == State::CheckingInstallation;
}
bool AetherPDInstallationModel::current() const
{
    return m_radio && m_radio->controlSessionCurrent(m_session) && m_radio->serial() == m_serial
        && m_radio->version() == m_firmware && m_radio->radioAddress() == m_address;
}
bool AetherPDInstallationModel::prepareRelease(const AetherPDRelease& release, const RadioControlSession& session)
{
    if (!release.valid() || release.profile != 1 || !prepareDownload(release.firmware, session)) { return false; }
    if (!m_download->start(release)) { unavailable(Failure::DownloadFailed); return false; }
    return true;
}
bool AetherPDInstallationModel::prepareFirstRelease(const AetherPDFirstRelease& release, const QString& hostname,
                                                  const RadioControlSession& session)
{
    if (!release.valid() || !m_write || !m_recovery.journal || hostname.isEmpty() || hostname.size() >= 64
        || hostname != QString::fromLatin1(hostname.toLatin1()) || !prepareDownload(release.firmware, session, hostname)) { return false; }
    if (!m_download->start(release)) { unavailable(Failure::DownloadFailed); return false; }
    return true;
}
bool AetherPDInstallationModel::prepareDownload(const QString& firmware, const RadioControlSession& session, const QString& hostname)
{
    if (QThread::currentThread() != thread() || !m_radio || m_radio->thread() != thread() || active() || !m_radio->controlSessionCurrent(session)
        || firmware != m_radio->version() || m_radio->serial().isEmpty()) { return false; }
    retirePreparation();
    if (m_job) { disconnect(m_job.get(), nullptr, this, nullptr); }
    m_job.reset(); m_package.reset(); m_plan.clear(); m_carrier.clear(); m_archiveSha256.clear();
    m_firstToken.clear(); m_publisherKey.clear(); m_nativeName.clear();
    m_outcome.reset(); m_cleanup = false; m_failure = Failure::None; m_received = 0; m_total = 0;
    m_session = session; m_serial = m_radio->serial(); m_firmware = m_radio->version(); m_address = m_radio->radioAddress();
    const quint64 generation = ++m_generation;
    m_downloadHostname = hostname;
    m_download = m_downloadFactory();
    if (!m_download || m_download->thread() != thread()) { unavailable(Failure::DownloadFailed); return false; }
    m_download->setParent(this);
    connect(m_download.get(), &AetherPDPackageDownload::progress, this, [this, generation](qint64 received, qint64 total) {
        if (generation != m_generation || m_state != State::Downloading) { return; }
        if (!current()) { unavailable(Failure::SessionChanged); return; }
        m_received = received; m_total = total; emit changed();
    });
    connect(m_download.get(), &AetherPDPackageDownload::failed, this, [this, generation] {
        if (generation == m_generation && m_state == State::Downloading) { unavailable(Failure::DownloadFailed); }
    });
    connect(m_download.get(), &AetherPDPackageDownload::ready, this, [this, generation] { downloaded(generation); });
    m_state = State::Downloading;
    const QPointer<AetherPDInstallationModel> self(this);
    emit changed();
    if (!self || generation != m_generation || m_state != State::Downloading) { return false; }
    if (!current()) { unavailable(Failure::SessionChanged); return false; }
    return true;
}
QString AetherPDInstallationModel::selectedDevelopmentVersion() const
{
    return m_radio && m_radio->version() == aetherpd::development::firmware()
        ? aetherpd::development::version() : QString{};
}
bool AetherPDInstallationModel::prepareSelectedDevelopment(const RadioControlSession& session)
{
    if (QThread::currentThread() != thread() || !m_radio || m_radio->thread() != thread() || active()
        || !m_radio->controlSessionCurrent(session) || selectedDevelopmentVersion().isEmpty()
        || m_radio->serial().isEmpty()) { return false; }
    retirePreparation();
    if (m_job) { disconnect(m_job.get(), nullptr, this, nullptr); }
    m_job.reset(); m_package.reset(); m_plan.clear(); m_carrier.clear(); m_archiveSha256.clear();
    m_firstToken.clear(); m_publisherKey.clear(); m_nativeName.clear();
    m_outcome.reset(); m_cleanup = false; m_failure = Failure::None; m_received = 0; m_total = 0;
    m_session = session; m_serial = m_radio->serial(); m_firmware = m_radio->version(); m_address = m_radio->radioAddress();
    const quint64 generation = ++m_generation;
    preparePackage([] { return aetherpd::development::load(); }, generation);
    return true;
}
bool AetherPDInstallationModel::prepareFirstPackage(const AetherPDFirstPackage& package, const QString& hostname,
                                                   const RadioControlSession& session)
{
    return prepareFirst([package] { return std::optional<AetherPDFirstPackage>(package); }, hostname, session);
}
QString AetherPDInstallationModel::selectedFirstDevelopmentVersion() const
{
    return m_radio && m_radio->version() == aetherpd::firstDevelopment::firmware()
        ? aetherpd::firstDevelopment::version() : QString{};
}
bool AetherPDInstallationModel::prepareSelectedFirstDevelopment(const RadioControlSession& session)
{
    if (selectedFirstDevelopmentVersion().isEmpty()) { return false; }
    return prepareFirst([] { return aetherpd::firstDevelopment::load(); },
        tailnetshim::suggestedHostname(m_radio->nickname()), session);
}
bool AetherPDInstallationModel::savedFirstInstallationAvailable() const
{
    if (!m_radio || selectedFirstDevelopmentVersion().isEmpty() || !m_recovery.read) { return false; }
    const QByteArray key = aetherpd::firstDevelopment::publisherKey();
    const auto saved = m_recovery.read(key);
    if (saved.state != AetherPDInstallationJournal::State::Pending && saved.state != AetherPDInstallationJournal::State::CredentialSaved) { return false; }
    const auto plan = aetherpd::parseFirstInstallationPlan(saved.plan, key);
    return plan && plan->serial == m_radio->serial() && plan->firmware == m_radio->version();
}
bool AetherPDInstallationModel::recoverSelectedFirstInstallation(const RadioControlSession& session)
{
    return !selectedFirstDevelopmentVersion().isEmpty()
        && recoverFirstInstallation(aetherpd::firstDevelopment::publisherKey(), session);
}
bool AetherPDInstallationModel::prepareFirst(std::function<std::optional<AetherPDFirstPackage>()> load, const QString& hostname,
                                           const RadioControlSession& session)
{
    if (QThread::currentThread() != thread() || !m_radio || m_radio->thread() != thread() || active()
        || !m_radio->controlSessionCurrent(session) || m_radio->serial().isEmpty() || !m_write || !m_recovery.journal) { return false; }
    retirePreparation();
    if (m_job) { disconnect(m_job.get(), nullptr, this, nullptr); }
    m_job.reset(); m_package.reset(); m_plan.clear(); m_carrier.clear(); m_archiveSha256.clear();
    m_publisherKey.clear(); m_nativeName.clear();
    m_outcome.reset(); m_cleanup = false; m_failure = Failure::None; m_received = 0; m_total = 0;
    m_session = session; m_serial = m_radio->serial(); m_firmware = m_radio->version(); m_address = m_radio->radioAddress();
    prepareFirstSnapshot(std::move(load), hostname, ++m_generation);
    return true;
}
void AetherPDInstallationModel::prepareFirstSnapshot(std::function<std::optional<AetherPDFirstPackage>()> load,
                                                    const QString& hostname, quint64 generation)
{
    QByteArray bytes(32, Qt::Uninitialized);
    for (char& byte : bytes) { byte = static_cast<char>(QRandomGenerator::system()->generate() & 255U); }
    m_firstToken = QString::fromLatin1(bytes.toHex());
    const QByteArray tokenHash = QCryptographicHash::hash(m_firstToken.toLatin1(), QCryptographicHash::Sha256);
    const QString serial = m_serial;
    prepareSnapshot([load = std::move(load), serial, tokenHash, hostname] {
        const auto package = load();
        return package ? Coordinator::prepareFirst(*package, serial, tokenHash, hostname) : std::nullopt;
    }, generation);
}
bool AetherPDInstallationModel::savedFirstPreparationAvailable() const
{
    if (!m_radio || active() || m_state == State::AwaitingApproval || m_radio->isRadioTransmitting()
        || selectedFirstDevelopmentVersion().isEmpty() || !m_recovery.read) { return false; }
    const QByteArray key = aetherpd::firstDevelopment::publisherKey();
    const auto saved = m_recovery.read(key);
    const auto plan = aetherpd::parseFirstInstallationPlan(saved.plan, key);
    return saved.state == AetherPDInstallationJournal::State::Pending && plan
        && plan->serial == m_radio->serial() && plan->firmware == m_radio->version();
}
bool AetherPDInstallationModel::prepareSelectedSavedFirstInstallation(const RadioControlSession& session)
{
    return savedFirstPreparationAvailable() && prepareSavedFirst([] { return aetherpd::firstDevelopment::load(); },
        aetherpd::firstDevelopment::publisherKey(), session);
}
bool AetherPDInstallationModel::prepareSavedFirstPackage(const AetherPDFirstPackage& package, const QByteArray& publisherKey,
                                                        const RadioControlSession& session)
{
    return prepareSavedFirst([package] { return std::optional<AetherPDFirstPackage>(package); }, publisherKey, session);
}
bool AetherPDInstallationModel::prepareSavedFirst(std::function<std::optional<AetherPDFirstPackage>()> load,
    const QByteArray& publisherKey, const RadioControlSession& session)
{
    if (QThread::currentThread() != thread() || !m_radio || m_radio->thread() != thread() || active()
        || m_state == State::AwaitingApproval || !m_radio->controlSessionCurrent(session) || m_radio->isRadioTransmitting()
        || !load || !m_write || !m_recovery.read || !m_recovery.load || !m_recovery.journal || !m_recovery.retainPending) { return false; }
    const auto saved = m_recovery.read(publisherKey);
    const auto plan = aetherpd::parseFirstInstallationPlan(saved.plan, publisherKey);
    if (saved.state != AetherPDInstallationJournal::State::Pending || !plan
        || plan->serial != m_radio->serial() || plan->firmware != m_radio->version()) { return false; }
    retirePreparation();
    if (m_job) { disconnect(m_job.get(), nullptr, this, nullptr); m_job.reset(); }
    m_package.reset(); m_firstToken.clear(); m_archiveSha256.clear();
    m_plan = saved.plan; m_publisherKey = publisherKey;
    const QString suffix = QString::fromLatin1(m_plan.mid(244, 8).toHex());
    m_nativeName = QStringLiteral("AetherPDFirst") + suffix; m_carrier = QStringLiteral("aetherpd-first-") + suffix;
    m_session = session; m_serial = plan->serial; m_firmware = plan->firmware; m_address = m_radio->radioAddress();
    m_outcome.reset(); m_cleanup = false; m_failure = Failure::None;
    m_savedPreparation = true; m_savedPackageLoader = std::move(load);
    m_state = State::LoadingRecoveryCredential;
    const quint64 generation = ++m_generation;
    const QPointer<AetherPDInstallationModel> self(this);
    const auto loader = m_recovery.load;
    const QString serial = m_serial, attempt = plan->planDigest;
    m_timeout.start(); emit changed();
    if (!self || generation != m_generation || m_state != State::LoadingRecoveryCredential) { return false; }
    if (!current()) { unavailable(Failure::SessionChanged); return false; }
    loader(serial, attempt, this, [self, generation](const QString& token) { if (self) { self->recoveryCredential(token, generation); } });
    return true;
}
void AetherPDInstallationModel::retirePreparation()
{
    m_downloadHostname.clear();
    m_recovering = false;
    m_recoveryCleanup = false;
    m_savedPreparation = false; m_savedPackageLoader = {};
    if (m_recoveryMonitor) { disconnect(m_recoveryMonitor.get(), nullptr, this, nullptr); m_recoveryMonitor.reset(); }
    if (m_download) {
        disconnect(m_download.get(), nullptr, this, nullptr);
        m_download->cancel();
        // May be called from a downloader signal; retire after its stack unwinds.
        m_download.release()->deleteLater();
    }
    if (m_preparation) {
        disconnect(m_preparation, nullptr, this, nullptr);
        m_preparation->deleteLater(); m_preparation = nullptr;
    }
}
void AetherPDInstallationModel::downloaded(quint64 generation)
{
    if (generation != m_generation || m_state != State::Downloading || !m_download) { return; }
    if (!current()) { unavailable(Failure::SessionChanged); return; }
    const auto package = m_download->package();
    const auto firstPackage = m_download->firstPackage();
    const QString hostname = m_downloadHostname;
    retirePreparation();
    if (firstPackage) {
        prepareFirstSnapshot([package = *firstPackage] { return std::optional<AetherPDFirstPackage>(package); }, hostname, generation);
        return;
    }
    if (!package) { unavailable(Failure::PackageRejected); return; }
    preparePackage([package = *package] { return std::optional<AetherPDPackage>(package); }, generation);
}
void AetherPDInstallationModel::preparePackage(std::function<std::optional<AetherPDPackage>()> load, quint64 generation)
{
    const QString serial = m_serial;
    prepareSnapshot([load = std::move(load), serial] {
        const auto package = load();
        return package ? Coordinator::prepare(*package, serial) : std::nullopt;
    }, generation);
}
void AetherPDInstallationModel::prepareSnapshot(std::function<std::optional<Coordinator::Prepared>()> prepare, quint64 generation)
{
    m_state = State::Preparing;
    auto* watcher = new QFutureWatcher<std::optional<Coordinator::Prepared>>(this);
    m_preparation = watcher;
    connect(watcher, &QFutureWatcher<std::optional<Coordinator::Prepared>>::finished, this, [this, watcher, generation] {
        if (generation != m_generation || m_state != State::Preparing || m_preparation != watcher) { return; }
        const auto prepared = watcher->result();
        m_preparation = nullptr; watcher->deleteLater();
        if (!current()) { unavailable(Failure::SessionChanged); return; }
        if (!prepared) { unavailable(Failure::PackageRejected); return; }
        m_state = State::Idle;
        if (!offer(*prepared, m_session)) { unavailable(Failure::PackageRejected); }
    });
    // Only value snapshots cross threads; no radio, model or credential access.
    watcher->setFuture(QtConcurrent::run(std::move(prepare)));
    emit changed();
}
bool AetherPDInstallationModel::offer(const Coordinator::Prepared& package, const RadioControlSession& session)
{
    if (QThread::currentThread() != thread() || !m_radio || m_radio->thread() != thread() || active()
        || !m_radio->controlSessionCurrent(session)) { return false; }
    const bool first = !package.nativeName().isEmpty();
    const auto plan = first ? aetherpd::parseFirstInstallationPlan(package.approvedPlan(), package.publisherKey())
                            : aetherpd::parseInstallationPlan(package.approvedPlan());
    if (first && (m_firstToken.isEmpty() || QCryptographicHash::hash(m_firstToken.toLatin1(), QCryptographicHash::Sha256)
        != package.approvedPlan().mid(148, 32))) { return false; }
    if (!plan || package.used() || (!first && !package.approvedPlan().startsWith("PDUPGR02")) || plan->serial != m_radio->serial()
        || plan->firmware != m_radio->version()) { return false; }
    ++m_generation;
    m_timeout.stop();
    if (m_job) { disconnect(m_job.get(), nullptr, this, nullptr); }
    m_job.reset();
    m_package = package;
    m_plan = package.approvedPlan();
    m_publisherKey = package.publisherKey(); m_nativeName = package.nativeName();
    if (!first) { m_firstToken.clear(); }
    m_session = session;
    m_serial = plan->serial; m_firmware = plan->firmware; m_address = m_radio->radioAddress();
    m_carrier = package.carrierName(); m_archiveSha256 = package.carrierSha256();
    m_outcome.reset(); m_cleanup = false; m_failure = Failure::None;
    m_state = State::AwaitingApproval;
    emit changed();
    return true;
}
std::optional<AetherPDInstallationModel::Preview> AetherPDInstallationModel::preview() const
{
    const bool first = !m_nativeName.isEmpty();
    const auto plan = first ? aetherpd::parseFirstInstallationPlan(m_plan, m_publisherKey) : aetherpd::parseInstallationPlan(m_plan);
    if (!plan) { return std::nullopt; }
    return Preview{m_plan, plan->serial, plan->firmware, plan->previousDigest, plan->candidateDigest,
        plan->candidateKey, plan->planDigest, m_carrier, m_archiveSha256, first, m_nativeName,
        first ? QString::fromLatin1(m_plan.mid(180, 64).split('\0').first()) : QString{}};
}
bool AetherPDInstallationModel::approve(const QByteArray& displayedPlan)
{
    if (QThread::currentThread() != thread() || m_state != State::AwaitingApproval || !m_package || !m_factory
        || (m_nativeName.isEmpty() ? !m_load : !m_write)) { return false; }
    if (displayedPlan.isEmpty() || displayedPlan != m_plan) { return false; }
    if (!current()) { unavailable(Failure::SessionChanged); return false; }
    if (m_radio->isRadioTransmitting()) { return false; }
    const bool first = !m_nativeName.isEmpty();
    const auto shown = preview();
    if (first && (!shown || m_firstToken.isEmpty())) { return false; }
    if (m_savedPreparation) {
        const QPointer<AetherPDInstallationModel> self(this);
        const bool retained = m_recovery.retainPending(m_plan, m_publisherKey);
        if (!self) { return false; }
        if (!retained) { unavailable(Failure::JournalFailed); return false; }
        m_state = State::LoadingCredential;
        const QString token = m_firstToken;
        credential(token, ++m_generation);
        return true;
    }
    m_state = first ? State::SavingCredential : State::LoadingCredential;
    const quint64 generation = ++m_generation;
    const QPointer<AetherPDInstallationModel> self(this);
    const TokenLoader loader = m_load;
    const QString serial = m_serial;
    m_timeout.start();
    emit changed();
    if (!self || generation != m_generation || (m_state != State::LoadingCredential && m_state != State::SavingCredential)) { return false; }
    if (!current()) { unavailable(Failure::SessionChanged); return false; }
    if (first) {
        const TokenWriter writer = m_write;
        writer(serial, shown->planDigest, m_firstToken, this, [self, generation](bool persisted) {
            if (!self || generation != self->m_generation || self->m_state != State::SavingCredential) { return; }
            if (!persisted) { self->unavailable(Failure::CredentialSaveFailed); return; }
            if (!self->current()) { self->unavailable(Failure::SessionChanged); return; }
            const auto journal = self->m_recovery.journal;
            const bool recorded = journal && journal(self->m_plan, self->m_publisherKey, false);
            if (!self || generation != self->m_generation || self->m_state != State::SavingCredential) { return; }
            if (!recorded) {
                self->unavailable(Failure::JournalFailed); return;
            }
            self->m_state = State::LoadingCredential;
            self->credential(self->m_firstToken, generation);
        });
        return true;
    }
    loader(serial, this, [self, generation](const QString& token) { if (self) { self->credential(token, generation); } });
    return true;
}
void AetherPDInstallationModel::credential(const QString& token, quint64 generation)
{
    if (generation != m_generation || m_state != State::LoadingCredential) { return; }
    if (!current()) { unavailable(Failure::SessionChanged); return; }
    if (m_radio->isRadioTransmitting()) { unavailable(Failure::StartRefused); return; }
    if (token.isEmpty()) { unavailable(Failure::NoCredential); return; }
    ++m_generation;
    m_timeout.stop();
    m_job = m_factory(*m_radio);
    if (!m_job) { unavailable(Failure::StartRefused); return; }
    connect(m_job.get(), &Coordinator::stateChanged, this, [this] { emit changed(); });
    connect(m_job.get(), &Coordinator::finished, this, &AetherPDInstallationModel::completed);
    m_state = State::Running;
    const QPointer<AetherPDInstallationModel> self(this);
    const bool started = m_job->begin(*m_package, m_session, token);
    if (!self) { return; }
    m_package.reset();
    if (!started && m_job->state() == Coordinator::State::Idle) { unavailable(Failure::StartRefused); return; }
    emit changed();
}
void AetherPDInstallationModel::completed(Coordinator::Outcome result, bool cleanup)
{
    m_outcome = result; m_cleanup = cleanup; m_package.reset();
    if (!m_firstToken.isEmpty() && result == Coordinator::Outcome::Committed) {
        m_state = State::PromotingCredential;
        const quint64 generation = ++m_generation;
        const QPointer<AetherPDInstallationModel> self(this);
        const TokenWriter writer = m_write;
        const QString serial = m_serial, token = m_firstToken;
        m_timeout.start();
        emit changed();
        if (!self || generation != m_generation || m_state != State::PromotingCredential) { return; }
        writer(serial, {}, token, this, [self, generation](bool persisted) {
            if (self) { self->promoted(persisted, generation); }
        });
        return;
    }
    m_firstToken.clear(); m_state = State::Finished;
    emit changed();
}
void AetherPDInstallationModel::promoted(bool persisted, quint64 generation)
{
    if (generation != m_generation || m_state != State::PromotingCredential) { return; }
    ++m_generation; m_timeout.stop(); m_firstToken.clear(); m_state = State::Finished;
    if (!persisted) { m_failure = Failure::CredentialSaveFailed; }
    else {
        const QPointer<AetherPDInstallationModel> self(this);
        const auto journal = m_recovery.journal;
        const bool recorded = journal && journal(m_plan, m_publisherKey, true);
        if (!self) { return; }
        if (!recorded) { m_failure = Failure::JournalFailed; }
    }
    emit changed();
}
void AetherPDInstallationModel::unavailable(Failure reason)
{
    retirePreparation();
    ++m_generation;
    m_timeout.stop();
    m_state = State::Unavailable; m_failure = reason;
    m_package.reset();
    m_firstToken.clear();
    emit changed();
}
void AetherPDInstallationModel::cancel()
{
    if (m_state == State::LoadingRecoveryCredential || m_state == State::CheckingInstallation) {
        ++m_generation; m_timeout.stop();
        if (m_recoveryMonitor) { disconnect(m_recoveryMonitor.get(), nullptr, this, nullptr); m_recoveryMonitor->cancel(); }
        completed(Coordinator::Outcome::Canceled, false);
        return;
    }
    if (m_state == State::Downloading || m_state == State::Preparing || m_state == State::AwaitingApproval || m_state == State::LoadingCredential
        || m_state == State::SavingCredential) {
        retirePreparation();
        ++m_generation; m_timeout.stop(); m_package.reset(); m_firstToken.clear();
        m_state = State::Finished; m_outcome = Coordinator::Outcome::Canceled; m_cleanup = true;
        emit changed();
    } else if (m_state == State::Running && m_job) {
        m_state = State::Canceling;
        m_job->cancel();
        emit changed();
    }
}
void AetherPDInstallationModel::reconcile()
{
    if (m_state == State::Idle || m_state == State::Finished || m_state == State::Unavailable || m_state == State::PromotingCredential || current()) { return; }
    if (m_job && active()) { m_job->poll(); }
    else { unavailable(Failure::SessionChanged); }
}
bool AetherPDInstallationModel::recoveredCleanupAvailable() const
{
    return current() && m_recovering && m_state == State::Finished
        && m_outcome == Coordinator::Outcome::Committed && !m_cleanup && !m_radio->isRadioTransmitting();
}
bool AetherPDInstallationModel::cleanupRecoveredInstallation(const QByteArray& displayedPlan)
{
    if (QThread::currentThread() != thread() || !recoveredCleanupAvailable() || displayedPlan != m_plan
        || !m_recovery.load || !m_factory) { return false; }
    const auto plan = aetherpd::parseFirstInstallationPlan(m_plan, m_publisherKey);
    if (!plan) { return false; }
    if (m_recoveryMonitor) { disconnect(m_recoveryMonitor.get(), nullptr, this, nullptr); m_recoveryMonitor.reset(); }
    if (m_job) { disconnect(m_job.get(), nullptr, this, nullptr); m_job.reset(); }
    m_recoveryCleanup = true; m_failure = Failure::None; m_state = State::LoadingRecoveryCredential;
    const quint64 generation = ++m_generation;
    const QPointer<AetherPDInstallationModel> self(this);
    const auto loader = m_recovery.load;
    const QString serial = m_serial, attempt = plan->planDigest;
    m_timeout.start(); emit changed();
    if (!self || generation != m_generation || m_state != State::LoadingRecoveryCredential) { return false; }
    if (!current()) { unavailable(Failure::SessionChanged); return false; }
    loader(serial, attempt, this, [self, generation](const QString& token) { if (self) { self->recoveryCredential(token, generation); } });
    return true;
}
bool AetherPDInstallationModel::recoverFirstInstallation(const QByteArray& publisherKey, const RadioControlSession& session)
{
    if (QThread::currentThread() != thread() || !m_radio || m_radio->thread() != thread() || active()
        || !m_radio->controlSessionCurrent(session) || !m_recovery.read || !m_recovery.load || !m_recovery.monitor || !m_write) { return false; }
    const auto saved = m_recovery.read(publisherKey);
    if (saved.state != AetherPDInstallationJournal::State::Pending && saved.state != AetherPDInstallationJournal::State::CredentialSaved) { return false; }
    const auto plan = aetherpd::parseFirstInstallationPlan(saved.plan, publisherKey);
    if (!plan || plan->serial != m_radio->serial() || plan->firmware != m_radio->version()
        || !aetherpd::lanAddress(m_radio->radioAddress())) { return false; }
    retirePreparation();
    if (m_job) { disconnect(m_job.get(), nullptr, this, nullptr); m_job.reset(); }
    m_package.reset(); m_firstToken.clear(); m_recovering = true;
    m_session = session; m_serial = plan->serial; m_firmware = plan->firmware; m_address = m_radio->radioAddress();
    m_plan = saved.plan; m_publisherKey = publisherKey;
    const QString suffix = QString::fromLatin1(m_plan.mid(244, 8).toHex());
    m_nativeName = QStringLiteral("AetherPDFirst") + suffix; m_carrier = QStringLiteral("aetherpd-first-") + suffix;
    m_archiveSha256.clear(); m_outcome.reset(); m_cleanup = false; m_failure = Failure::None;
    m_state = State::LoadingRecoveryCredential;
    const quint64 generation = ++m_generation;
    const QPointer<AetherPDInstallationModel> self(this);
    const auto loader = m_recovery.load;
    const QString serial = m_serial, attempt = plan->planDigest;
    m_timeout.start(); emit changed();
    if (!self || generation != m_generation || m_state != State::LoadingRecoveryCredential) { return false; }
    if (!current()) { unavailable(Failure::SessionChanged); return false; }
    loader(serial, attempt, this, [self, generation](const QString& token) { if (self) { self->recoveryCredential(token, generation); } });
    return true;
}
void AetherPDInstallationModel::recoveryCredential(const QString& token, quint64 generation)
{
    if (generation != m_generation || m_state != State::LoadingRecoveryCredential) { return; }
    if (!current()) { unavailable(Failure::SessionChanged); return; }
    if (!aetherpd::validAdminToken(token) || QCryptographicHash::hash(token.toLatin1(), QCryptographicHash::Sha256) != m_plan.mid(148, 32)) {
        unavailable(Failure::NoCredential); return;
    }
    ++m_generation; m_timeout.stop(); m_firstToken = token;
    if (m_savedPreparation) {
        const auto saved = m_recovery.read(m_publisherKey);
        if (saved.state != AetherPDInstallationJournal::State::Pending || saved.plan != m_plan) {
            unavailable(Failure::JournalFailed); return;
        }
        const auto load = std::move(m_savedPackageLoader);
        const QByteArray approved = m_plan, key = m_publisherKey;
        prepareSnapshot([load, approved, key] {
            const auto package = load ? load() : std::nullopt;
            return package ? Coordinator::prepareSavedFirst(*package, approved, key) : std::nullopt;
        }, m_generation);
        return;
    }
    if (m_recoveryCleanup) {
        m_job = m_factory(*m_radio);
        if (!m_job || m_job->thread() != thread()) { unavailable(Failure::StartRefused); return; }
        connect(m_job.get(), &Coordinator::stateChanged, this, [this] { emit changed(); });
        connect(m_job.get(), &Coordinator::finished, this, &AetherPDInstallationModel::completed);
        m_state = State::Running;
        const QPointer<AetherPDInstallationModel> self(this);
        const bool started = m_job->beginFirstCleanup(m_plan, m_publisherKey, m_session, token);
        if (!self) { return; }
        if (!started && m_job->state() == Coordinator::State::Idle) { unavailable(Failure::StartRefused); return; }
        emit changed();
        return;
    }
    m_recoveryMonitor = m_recovery.monitor();
    if (!m_recoveryMonitor || m_recoveryMonitor->thread() != thread()) { unavailable(Failure::StartRefused); return; }
    connect(m_recoveryMonitor.get(), &AetherPDInstallationMonitor::finished, this,
        [this](AetherPDInstallationMonitor::State state, AetherPDInstallationMonitor::Reason) {
            if (!current()) { unavailable(Failure::SessionChanged); return; }
            completed(state == AetherPDInstallationMonitor::State::Committed ? Coordinator::Outcome::Committed : Coordinator::Outcome::Unconfirmed, false);
        });
    m_state = State::CheckingInstallation;
    const QPointer<AetherPDInstallationModel> self(this);
    if (!m_recoveryMonitor->beginFirstInstallation(m_address, token, m_plan, m_publisherKey)) {
        unavailable(Failure::StartRefused); return;
    }
    if (self) { emit changed(); }
}
} // namespace AetherSDR
