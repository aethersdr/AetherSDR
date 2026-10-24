#pragma once

#include <QString>

#include <functional>

namespace AetherSDR {

// Which CTR2 window action a delayed retry belongs to. A retry is armed when
// an action fails for want of Linux device access, and runs a second or a
// password prompt later; by then the operator may have started another
// action or cancelled. Every start and cancel moves the serial on, and a
// ticket from before that no longer runs.
class Ctr2ActionRetry {
public:
    // retarget: this run follows an access grant, so the job acts on the unit
    // as it is now (a restart may have moved it into download mode), if it
    // is still the same one.
    using Job = std::function<bool(bool retarget, QString*)>;

    struct Ticket {
        quint64 serial{0};
        Job job;
        QString doneText;
        QString device;   // mark(): the device the press was for
    };

    // An action starting; retry: it is the one retry of the previous action.
    void started(Job job, const QString& doneText, bool retry = false)
    {
        ++m_serial;
        m_job = std::move(job);
        m_doneText = doneText;
        m_retried = retry;
    }
    // The operator cancelled: pending retries are stale.
    void invalidate() { ++m_serial; }
    // The operator pressed something that is not a job yet (a question is
    // about to be asked, or the device read again) for `device`: pending
    // retries are stale, and the returned ticket stands for this press until
    // the next start or cancel, and only while `device` is still the one
    // selected (see current()).
    Ticket mark(const QString& device = {})
    {
        ++m_serial;
        return {m_serial, {}, {}, device};
    }

    // The action running now, to retry later.
    Ticket ticket() const { return {m_serial, m_job, m_doneText}; }
    // No action has started, and nothing was cancelled, since the ticket;
    // and a press's device is still the selected one.
    bool current(const Ticket& ticket, const QString& selected = {}) const
    {
        return ticket.serial == m_serial && (ticket.device.isEmpty() || ticket.device == selected);
    }
    bool hasJob() const { return static_cast<bool>(m_job); }
    // The running action is already the one retry.
    bool retried() const { return m_retried; }

private:
    quint64 m_serial{0};
    Job m_job;
    QString m_doneText;
    bool m_retried{false};
};

} // namespace AetherSDR
