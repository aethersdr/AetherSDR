#include "AetherPDInstallationModel.h"
#include "RadioModel.h"
#include "core/AetherPDProtocol.h"
#include "core/TailnetShimTokenStore.h"
#include <QThread>
#include <utility>

namespace AetherSDR {
AetherPDInstallationModel::AetherPDInstallationModel(RadioModel& radio, QObject* parent)
    : AetherPDInstallationModel(radio,
        [](const QString& serial, QObject* context, std::function<void(const QString&)> done) {
            TailnetShimTokenStore::load(serial, context, std::move(done));
        }, [](RadioModel& model) { return std::make_unique<Coordinator>(model); }, parent) {}
AetherPDInstallationModel::AetherPDInstallationModel(RadioModel& radio, TokenLoader loader, Factory factory, QObject* parent)
    : QObject(parent), m_radio(&radio), m_load(std::move(loader)), m_factory(std::move(factory))
{
    m_timeout.setParent(this);
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(10000);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (m_state == State::LoadingCredential) { unavailable(Failure::CredentialTimeout); }
    });
    connect(&radio, &RadioModel::connectionStateChanged, this, &AetherPDInstallationModel::reconcile);
    connect(&radio, &RadioModel::infoChanged, this, &AetherPDInstallationModel::reconcile);
    connect(&radio, &QObject::destroyed, this, [this] { m_radio = nullptr; reconcile(); });
}
AetherPDInstallationModel::~AetherPDInstallationModel()
{
    m_timeout.stop();
    if (m_job) { disconnect(m_job.get(), nullptr, this, nullptr); }
    m_job.reset();
}
bool AetherPDInstallationModel::active() const
{
    return m_state == State::LoadingCredential || m_state == State::Running || m_state == State::Canceling;
}
bool AetherPDInstallationModel::current() const
{
    return m_radio && m_radio->controlSessionCurrent(m_session) && m_radio->serial() == m_serial
        && m_radio->version() == m_firmware && m_radio->radioAddress() == m_address;
}
bool AetherPDInstallationModel::offer(const Coordinator::Prepared& package, const RadioControlSession& session)
{
    if (QThread::currentThread() != thread() || !m_radio || m_radio->thread() != thread() || active()
        || !m_radio->controlSessionCurrent(session)) { return false; }
    const auto plan = aetherpd::parseInstallationPlan(package.approvedPlan());
    if (!plan || package.used() || !package.approvedPlan().startsWith("PDUPGR02") || plan->serial != m_radio->serial()
        || plan->firmware != m_radio->version()) { return false; }
    ++m_generation;
    m_timeout.stop();
    if (m_job) { disconnect(m_job.get(), nullptr, this, nullptr); }
    m_job.reset();
    m_package = package;
    m_plan = package.approvedPlan();
    m_session = session;
    m_serial = plan->serial; m_firmware = plan->firmware; m_address = m_radio->radioAddress();
    m_carrier = package.carrierName(); m_archiveSha256 = package.carrierSha256();
    m_outcome.reset(); m_cleanup = false; m_failure = Failure::None;
    m_state = State::AwaitingApproval;
    emit changed();
    return true;
}
bool AetherPDInstallationModel::approve()
{
    if (QThread::currentThread() != thread() || m_state != State::AwaitingApproval || !m_package || !m_load || !m_factory) { return false; }
    if (!current()) { unavailable(Failure::SessionChanged); return false; }
    m_state = State::LoadingCredential;
    const quint64 generation = ++m_generation;
    const QPointer<AetherPDInstallationModel> self(this);
    const TokenLoader loader = m_load;
    const QString serial = m_serial;
    m_timeout.start();
    emit changed();
    if (!self || generation != m_generation || m_state != State::LoadingCredential) { return false; }
    if (!current()) { unavailable(Failure::SessionChanged); return false; }
    loader(serial, this, [self, generation](const QString& token) { if (self) { self->credential(token, generation); } });
    return true;
}
void AetherPDInstallationModel::credential(const QString& token, quint64 generation)
{
    if (generation != m_generation || m_state != State::LoadingCredential) { return; }
    if (!current()) { unavailable(Failure::SessionChanged); return; }
    if (token.isEmpty()) { unavailable(Failure::NoCredential); return; }
    ++m_generation;
    m_timeout.stop();
    m_job = m_factory(*m_radio);
    if (!m_job) { unavailable(Failure::StartRefused); return; }
    connect(m_job.get(), &Coordinator::stateChanged, this, [this] { emit changed(); });
    connect(m_job.get(), &Coordinator::finished, this, [this](Coordinator::Outcome result, bool cleanup) {
        m_state = State::Finished;
        m_outcome = result; m_cleanup = cleanup;
        m_package.reset();
        emit changed();
    });
    m_state = State::Running;
    const QPointer<AetherPDInstallationModel> self(this);
    const bool started = m_job->begin(*m_package, m_session, token);
    if (!self) { return; }
    m_package.reset();
    if (!started && m_job->state() == Coordinator::State::Idle) { unavailable(Failure::StartRefused); return; }
    emit changed();
}
void AetherPDInstallationModel::unavailable(Failure reason)
{
    ++m_generation;
    m_timeout.stop();
    m_state = State::Unavailable; m_failure = reason;
    m_package.reset();
    emit changed();
}
void AetherPDInstallationModel::cancel()
{
    if (m_state == State::AwaitingApproval || m_state == State::LoadingCredential) {
        ++m_generation; m_timeout.stop(); m_package.reset();
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
    if (m_state == State::Idle || m_state == State::Finished || m_state == State::Unavailable || current()) { return; }
    if (m_job && active()) { m_job->poll(); }
    else { unavailable(Failure::SessionChanged); }
}
} // namespace AetherSDR
