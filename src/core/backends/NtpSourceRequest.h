#pragma once

#include <QMetaType>
#include <QRegularExpression>
#include <QString>

#include <optional>

namespace AetherSDR {

// The radio's own NTP time sources (SmartSDR 4.3.16, FlexLib NtpSource.cs). The
// radio keeps at most ten; each has an immutable id. The allowlists mirror the
// firmware's validation (NtpSource.cs:29-77): anything outside them is refused by
// the radio, and a quote or space would break the command line. \z, not $: $
// also matches before a final newline, which would end the command early.
struct NtpSourceRules {
    static constexpr int kMaxSources = 10;
    static constexpr int kMaxIdChars = 64;
    static constexpr int kMaxAddressChars = 253;
    // The firmware stores 64, but its parse buffer counts the two quotes.
    static constexpr int kMaxDescriptionChars = 62;

    static bool idValid(const QString& id)
    {
        static const QRegularExpression re(
            QStringLiteral("^[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?\\z"));
        return id.size() <= kMaxIdChars && re.match(id).hasMatch();
    }
    static bool addressValid(const QString& address)
    {
        static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9._:%-]+\\z"));
        return address.size() <= kMaxAddressChars && re.match(address).hasMatch();
    }
    static bool descriptionValid(const QString& description)
    {
        static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9._\\- ]*\\z"));
        return description.size() <= kMaxDescriptionChars && re.match(description).hasMatch();
    }
    static bool directiveValid(const QString& directive)
    {
        return directive == QLatin1String("server") || directive == QLatin1String("pool")
            || directive == QLatin1String("peer");
    }
};

// One definition as the operator enters it: everything `ntp_sources create`
// carries. Enabled is not part of it; a created source starts enabled.
struct NtpSourceDefinition {
    QString id;
    QString address;
    QString directive{QStringLiteral("server")};
    QString description;
    bool prefer{false};

    bool valid() const
    {
        return NtpSourceRules::idValid(id) && NtpSourceRules::addressValid(address)
            && NtpSourceRules::directiveValid(directive)
            && NtpSourceRules::descriptionValid(description);
    }
};

// One command toward the radio's NTP source list. There is no update verb: an
// edit is Remove followed by Create (RadioModel::replaceNtpSource).
struct NtpSourceRequest {
    enum class Action { Subscribe, Create, Remove, Enable, Disable };
    Action action{Action::Subscribe};
    NtpSourceDefinition definition;   // Create; only definition.id for the others

    bool valid() const
    {
        switch (action) {
        case Action::Subscribe: return definition.id.isEmpty();
        case Action::Create: return definition.valid();
        case Action::Remove:
        case Action::Enable:
        case Action::Disable: return NtpSourceRules::idValid(definition.id);
        }
        return false;
    }
};

// One source as the radio last reported it. Stratum and reach are -1 while
// unknown; state is the raw chrony state (selected, candidate, falseticker,
// jittery, outlier, non-selectable, unknown) or empty.
struct NtpSourceState {
    QString id;
    QString address;
    QString directive;
    QString description;
    bool enabled{false};
    bool selected{false};
    bool prefer{false};
    QString state;
    int stratum{-1};
    int reach{-1};

    // FlexLib treats a chrony "selected" state as in use even before selected=1.
    bool inUse() const { return selected || state == QLatin1String("selected"); }
    bool operator==(const NtpSourceState&) const = default;
};

// Present-only status for one source: `removed`, or the fields the line carried.
struct NtpSourceDelta {
    QString id;
    bool removed{false};
    std::optional<QString> address;
    std::optional<QString> directive;
    std::optional<QString> description;
    std::optional<bool> enabled;
    std::optional<bool> selected;
    std::optional<bool> prefer;
    std::optional<QString> state;
    std::optional<int> stratum;
    std::optional<int> reach;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::NtpSourceDelta)
