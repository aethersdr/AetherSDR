#pragma once

#include "RadioMessageTypes.h"

#include <QString>

namespace AetherSDR {

class GuiClientRegistrationState
{
public:
    enum class Phase {
        Idle,
        AwaitingReply,
        Registered,
        Rejected
    };

    enum class Action {
        ContinueHandshake,
        DisconnectAndWaitForUser
    };

    struct Result {
        Action action{Action::DisconnectAndWaitForUser};
        int resultCode{0};
        QString detail;

        bool accepted() const { return action == Action::ContinueHandshake; }
    };

    void begin()
    {
        m_phase = Phase::AwaitingReply;
        m_radioErrorDetail.clear();
    }

    void noteRadioMessage(const QString& text, MessageSeverity severity)
    {
        if (m_phase != Phase::AwaitingReply
            || (severity != MessageSeverity::Error
                && severity != MessageSeverity::Fatal)) {
            return;
        }

        const QString detail = text.trimmed();
        if (!detail.isEmpty()) {
            m_radioErrorDetail = detail;
        }
    }

    Result complete(int resultCode, const QString& responseBody)
    {
        if (resultCode == 0) {
            m_phase = Phase::Registered;
            m_radioErrorDetail.clear();
            return {Action::ContinueHandshake, resultCode, {}};
        }

        m_phase = Phase::Rejected;
        QString detail = responseBody.trimmed();
        if (detail.isEmpty()) {
            detail = m_radioErrorDetail;
        }
        m_radioErrorDetail.clear();
        return {Action::DisconnectAndWaitForUser, resultCode, detail};
    }

    void reset()
    {
        m_phase = Phase::Idle;
        m_radioErrorDetail.clear();
    }

    // Observed only by gui_client_registration_state_test since #4563 removed
    // the AwaitingReply guard that was production's last reader. Kept
    // deliberately: Registered and Rejected are what make this a state machine
    // rather than a pair of booleans, and the tests pin the transitions that
    // #4560 turned out to depend on. Do not delete it as unused without
    // replacing what it documents.
    Phase phase() const { return m_phase; }

private:
    Phase m_phase{Phase::Idle};
    QString m_radioErrorDetail;
};

} // namespace AetherSDR
