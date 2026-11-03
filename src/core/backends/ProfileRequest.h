#pragma once

#include "core/backends/RadioControlTypes.h"

namespace AetherSDR {

struct ProfileRequest {
    enum class Kind { Global, Transmit, Mic, Cw };
    enum class Action { Refresh, Load, Save, Create, Remove, AutoSave, Reset };
    Kind kind{Kind::Global};
    Action action{Action::Refresh};
    QString name;
    bool enabled{false};

    bool valid() const
    {
        if (kind < Kind::Global || kind > Kind::Cw
            || action < Action::Refresh || action > Action::Reset) { return false; }
        if (action == Action::Refresh || action == Action::AutoSave) {
            return name.isEmpty() && (action != Action::AutoSave || kind == Kind::Global);
        }
        if ((action == Action::Save && kind != Kind::Global)
            || (action == Action::Reset && kind != Kind::Cw)
            || (action == Action::Create && kind == Kind::Global)
            || name.trimmed().isEmpty() || name.size() > 256) { return false; }
        for (const QChar ch : name) {
            if (!ch.isPrint() || ch == QLatin1Char('"') || ch == QLatin1Char('|')
                || ch == QLatin1Char('\\')) { return false; }
        }
        if (kind != Kind::Global
            && (action == Action::Create || action == Action::Remove || action == Action::Reset)
            && QString(name).remove(QLatin1Char('*')).trimmed().isEmpty()) { return false; }
        return true;
    }
};

} // namespace AetherSDR
