#include "core/backends/flex/FlexBackend.h"

#include <QStringList>

namespace AetherSDR {

namespace {

// FlexLib NtpSource.cs SplitStatusTokens: space-separated, a double-quoted
// value (description="two words") is one token, and the quotes are dropped.
QStringList splitStatusTokens(const QString& body)
{
    QStringList tokens;
    QString current;
    bool quoted = false;
    for (const QChar ch : body) {
        if (ch == QLatin1Char('"')) {
            quoted = !quoted;
        } else if (ch == QLatin1Char(' ') && !quoted) {
            if (!current.isEmpty()) { tokens.append(current); current.clear(); }
        } else {
            current.append(ch);
        }
    }
    if (!current.isEmpty()) { tokens.append(current); }
    return tokens;
}

std::optional<bool> parseFlag(const QString& value)
{
    bool ok = false;
    const int v = value.toInt(&ok);
    if (!ok || v < 0 || v > 255) { return std::nullopt; }
    return v == 1;
}

std::optional<int> parseInt(const QString& value)
{
    bool ok = false;
    const int v = value.toInt(&ok);
    return ok ? std::optional<int>(v) : std::nullopt;
}

} // namespace

// FlexLib v4.3.16: Radio.cs:14546 RequestNtpSource, NtpSource.cs:104 Remove and
// :301 Enabled, Radio.cs:2348 for the subscription.
ReceiveDispatch FlexBackend::requestNtpSource(const NtpSourceRequest& request,
                                             const RadioControlCompletion& completion,
                                             const RadioControlFence& current)
{
    using Action = NtpSourceRequest::Action;
    if (!request.valid() || !m_setupSink || (current && !current())
        || !capabilities().ntpSourceControl.has_value()) {
        return ReceiveDispatch::Unsupported;
    }
    const NtpSourceDefinition& d = request.definition;
    QString command;
    switch (request.action) {
    case Action::Subscribe: command = QStringLiteral("sub ntp_sources all"); break;
    case Action::Create:
        command = QStringLiteral("ntp_sources create id=%1 address=%2 directive=%3")
                      .arg(d.id, d.address, d.directive);
        if (!d.description.isEmpty()) {
            command += QStringLiteral(" description=\"%1\"").arg(d.description);
        }
        if (d.prefer) { command += QStringLiteral(" prefer=1"); }
        break;
    case Action::Remove: command = QStringLiteral("ntp_sources remove id=%1").arg(d.id); break;
    case Action::Enable: command = QStringLiteral("ntp_sources enable id=%1").arg(d.id); break;
    case Action::Disable: command = QStringLiteral("ntp_sources disable id=%1").arg(d.id); break;
    }
    const auto sink = m_setupSink;
    return sink(command, completion) ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

// FlexLib v4.3.16 Radio.cs:3610 routes `ntp_sources` before its kv split.
bool FlexBackend::decodeRawStatus(const QString& body)
{
    static const QString kNtpSources = QStringLiteral("ntp_sources ");
    if (!body.startsWith(kNtpSources)) { return false; }
    decodeNtpSourceStatus(body.mid(kNtpSources.size()));
    return true;
}

// FlexLib v4.3.16 Radio.cs:14427 ParseNtpSourcesStatus and NtpSource.cs
// StatusUpdate. The first word is always id=<id>; only the word right after it
// can mean removal, since a description could contain "removed".
void FlexBackend::decodeNtpSourceStatus(const QString& rawBody)
{
    const QStringList words = rawBody.split(QLatin1Char(' '));
    if (words.size() < 2 || !words.at(0).startsWith(QLatin1String("id="))) { return; }
    NtpSourceDelta d;
    d.id = words.at(0).mid(3);
    if (d.id.isEmpty()) { return; }
    if (words.at(1) == QLatin1String("removed")) {
        d.removed = true;
        emit ntpSourceChanged(d);
        return;
    }
    for (const QString& token : splitStatusTokens(rawBody.mid(words.at(0).size() + 1))) {
        const qsizetype eq = token.indexOf(QLatin1Char('='));
        if (eq <= 0) { continue; }
        const QString key = token.left(eq).toLower();
        const QString value = token.mid(eq + 1);
        if (key == QLatin1String("address")) { d.address = value; }
        else if (key == QLatin1String("directive")) { d.directive = value; }
        else if (key == QLatin1String("description")) { d.description = value; }
        else if (key == QLatin1String("enabled")) { d.enabled = parseFlag(value); }
        else if (key == QLatin1String("selected")) { d.selected = parseFlag(value); }
        else if (key == QLatin1String("prefer")) { d.prefer = parseFlag(value); }
        else if (key == QLatin1String("state")) { d.state = value; }
        else if (key == QLatin1String("stratum")) { d.stratum = parseInt(value); }
        else if (key == QLatin1String("reach")) { d.reach = parseInt(value); }
    }
    emit ntpSourceChanged(d);
}

} // namespace AetherSDR
