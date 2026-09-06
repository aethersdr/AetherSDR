#include "RadioCatalogue.h"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QJsonArray>

#include <utility>

namespace AetherSDR::control {
namespace {

const ResourceAddress kCatalogue{QStringLiteral("radioCatalogue"), {}, {}};

bool boundedText(const QString& text, qsizetype limit)
{
    if (text.size() > limit || !text.isValidUtf16()) {
        return false;
    }
    for (const QChar character : text) {
        if (character.isNull() || character.category() == QChar::Other_Control) {
            return false;
        }
    }
    return true;
}

bool validIdentity(const QString& family, const QString& serial)
{
    if (family.isEmpty() || family.size() > 16
        || family.front() < QLatin1Char('a') || family.front() > QLatin1Char('z')
        || serial.isEmpty()
        || !boundedText(serial, RadioCatalogue::kMaxTextChars)) {
        return false;
    }
    for (const QChar character : family) {
        if ((character < QLatin1Char('a') || character > QLatin1Char('z'))
            && (character < QLatin1Char('0') || character > QLatin1Char('9'))) {
            return false;
        }
    }
    return true;
}

QString identityKey(const QString& family, const QString& serial)
{
    return family + QChar(0x1f) + serial;
}

} // namespace

RadioCatalogue::RadioCatalogue(std::unique_ptr<RadioDiscoverySource> source,
                               ControlResourceStore* resources, QObject* parent)
    : QObject(parent), m_source(std::move(source)), m_resources(resources)
{
    Q_ASSERT(m_source && m_resources && m_source->thread() == thread());
    connect(m_source.get(), &RadioDiscoverySource::radioChanged, this, &RadioCatalogue::upsert);
    connect(m_source.get(), &RadioDiscoverySource::radioLost, this, &RadioCatalogue::remove);
    publish();
}

RadioCatalogue::~RadioCatalogue()
{
    stop();
    m_resources->remove(kCatalogue);
}

void RadioCatalogue::start()
{
    if (m_started) {
        return;
    }
    m_started = true;
    m_running = true;
    publish();
    m_source->start();
}

void RadioCatalogue::stop()
{
    m_started = true;
    m_running = false; // Gate callbacks before stopping a source that may emit.
    m_source->stop();
    m_entries.clear();
    publish();
}

void RadioCatalogue::upsert(const DiscoveredRadio& radio)
{
    if (!m_running || !validIdentity(radio.family, radio.serial)
        || !boundedText(radio.name, kMaxTextChars)
        || !boundedText(radio.model, kMaxTextChars)
        || !boundedText(radio.nickname, kMaxTextChars)
        || !boundedText(radio.version, kMaxTextChars)
        || !boundedText(radio.address, 64)) {
        return;
    }
    const bool lan = radio.transport == QStringLiteral("lan");
    if ((!lan && radio.transport != QStringLiteral("usb") && radio.transport != QStringLiteral("sim"))
        || (lan && (QHostAddress(radio.address).isNull() || radio.port == 0))
        || (!lan && (!radio.address.isEmpty() || radio.port != 0))) {
        return;
    }
    const QString key = identityKey(radio.family, radio.serial);
    if (!m_entries.contains(key) && m_entries.size() >= kMaxEntries) {
        m_limited = true;
        publish();
        return;
    }
    const QString id = QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex());
    m_entries.insert(key, {{QStringLiteral("id"), id},
                           {QStringLiteral("family"), radio.family},
                           {QStringLiteral("serial"), radio.serial},
                           {QStringLiteral("name"), radio.name},
                           {QStringLiteral("model"), radio.model},
                           {QStringLiteral("nickname"), radio.nickname},
                           {QStringLiteral("version"), radio.version},
                           {QStringLiteral("transport"), radio.transport},
                           {QStringLiteral("address"), radio.address},
                           {QStringLiteral("port"), radio.port},
                           {QStringLiteral("inUse"), radio.inUse}});
    publish();
}

void RadioCatalogue::remove(const QString& family, const QString& serial)
{
    if (m_running && validIdentity(family, serial)
        && m_entries.remove(identityKey(family, serial)) > 0) {
        publish();
    }
}

void RadioCatalogue::publish()
{
    QJsonArray entries;
    for (const QJsonObject& entry : std::as_const(m_entries)) {
        entries.append(entry);
    }
    m_resources->upsert(kCatalogue,
        {{QStringLiteral("running"), m_running},
         {QStringLiteral("sources"), QJsonArray::fromStringList(m_source->enabledSources())},
         {QStringLiteral("limited"), m_limited},
         {QStringLiteral("maxEntries"), static_cast<qint64>(kMaxEntries)},
         {QStringLiteral("entries"), entries}});
}

} // namespace AetherSDR::control
