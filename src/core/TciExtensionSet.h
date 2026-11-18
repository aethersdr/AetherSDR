#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

namespace AetherSDR {

// The opt-in AetherSDR TCI extension set (docs/user/docs/tci-server.md).
// Command groups register here with the capability that offers them; the
// server sends none of it to a client that has not sent
// `aethersdr_ext_enable:true;`. Holds no model and no socket.
class TciExtensionSet {
public:
    static constexpr int kVersion = 1;

    struct Result {
        QString reply;      // to the requesting client only
        QString broadcast;  // to every client that enabled the set
    };

    struct Group {
        QString token;          // item in the `aethersdr_ext:` list
        QStringList commands;   // lower-case names, each `aethersdr_...`
        std::function<bool()> offered;            // the gating capability
        std::function<QStringList()> stateLines;  // sent on enable; may be empty
        std::function<Result(const QString& name, const QStringList& args)> handle;
        // Optional. Called after the list and every state line went to every
        // enabled client, so a group with its own change notice can take its
        // current state as sent. Not called when any enabled client already
        // held the list, nor when no client is enabled.
        std::function<void()> resent;
    };

    // Refuses an empty or reserved token or name, a missing gate or handler,
    // and a token or command another group already holds.
    bool registerGroup(Group group);

    QStringList offeredTokens() const;
    // `aethersdr_ext:<version>[,<token>...];`
    QString advertisement() const;
    QStringList stateLines() const;
    // The group answering `name`, while it is offered; nullptr otherwise.
    const Group* offeredGroupFor(const QString& name) const;
    // Runs every group's `resent` hook. A hook may register a group.
    void notifyResent() const;

    static bool isExtensionCommand(const QString& lowerName);
    // `true`/`1` and `false`/`0`; anything else is malformed.
    static std::optional<bool> parseBool(const QString& value);

private:
    QVector<Group> m_groups;
};

} // namespace AetherSDR
