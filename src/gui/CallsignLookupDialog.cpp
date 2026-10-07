#include "CallsignLookupDialog.h"

#include "CallsignCard.h"
#include "core/CallsignLookupService.h"
#include "core/CallsignUtils.h"
#include "core/ThemeManager.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace AetherSDR {

LiveCwContactsDialog::LiveCwContactsDialog(int textFontPx, QWidget* parent)
    : PersistentDialog(tr("Live CW contacts"), QString(), parent),
      m_textFontPx(std::clamp(textFontPx, 18, 32))
{
    setObjectName(QStringLiteral("liveCwContactsDialog"));
    setAccessibleName(tr("Live CW contacts"));
    setAccessibleDescription(tr("Station information heard in decoded CW. Closing turns off live contacts."));
    setMinimumWidth(480);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(12);
    setBodyLayoutMargins(QMargins(12, 12, 12, 12), QMargins(12, 12, 12, 12));

    m_waiting = new QLabel(tr("Waiting for a callsign in decoded CW…"), this);
    m_waiting->setObjectName(QStringLiteral("liveCwContactsWaiting"));
    m_waiting->setAccessibleName(tr("Live contact status"));
    m_waiting->setWordWrap(true);
    root->addWidget(m_waiting);

    m_card = new CallsignCard(CallsignCard::Variant::LiveCw, this);
    m_card->setTextFontPx(m_textFontPx);
    m_card->hide();
    root->addWidget(m_card);

    m_closeBtn = new QPushButton(tr("Close live contacts"), this);
    m_closeBtn->setObjectName(QStringLiteral("closeLiveCwContacts"));
    m_closeBtn->setAccessibleName(tr("Close live contacts"));
    m_closeBtn->setAccessibleDescription(tr("Close this window and turn off Show live CW contacts in the View menu."));
    m_closeBtn->setMinimumHeight(44);
    m_closeBtn->setAutoDefault(false);
    connect(m_closeBtn, &QPushButton::clicked, this, &QWidget::close);
    root->addWidget(m_closeBtn);

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &LiveCwContactsDialog::applyTheme);
}

void LiveCwContactsDialog::applyTheme()
{
    auto& tm = ThemeManager::instance();
    tm.applyStyleSheet(bodyWidget(),
        "QWidget { background: {{color.canon.ground}}; }");
    tm.applyStyleSheet(m_waiting,
        QStringLiteral("QLabel { color: {{color.canon.inkSoft}}; font-size: %1px; }").arg(m_textFontPx));
    tm.applyStyleSheet(m_closeBtn,
        QStringLiteral("QPushButton { background: {{color.canon.control}}; color: {{color.canon.cyan}};"
                       " border: 1px solid {{color.canon.lineHi}}; border-radius: 4px;"
                       " padding: 8px 14px; font-size: %1px; }"
                       "QPushButton:hover { background: {{color.canon.nested}}; color: {{color.canon.aqua}}; }")
            .arg(m_textFontPx));
}

void LiveCwContactsDialog::showCallsign(const QString& call)
{
    m_waiting->hide();
    m_card->showPending(call);
    m_card->show();
}

void LiveCwContactsDialog::clearContact()
{
    m_card->clearCard();
    m_card->hide();
    m_waiting->show();
}

CallsignLookupDialog::CallsignLookupDialog(QWidget* parent)
    : PersistentDialog(QStringLiteral("Callsign Lookup"),
                       QStringLiteral("CallsignLookupDialogGeometry"), parent)
{
    setObjectName(QStringLiteral("CallsignLookupDialog"));
    setMinimumSize(420, 260);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* inputRow = new QHBoxLayout;
    inputRow->setSpacing(8);

    m_input = new QLineEdit(this);
    m_input->setObjectName(QStringLiteral("callsignLookupInput"));
    m_input->setAccessibleName(QStringLiteral("Callsign to look up"));
    m_input->setPlaceholderText(QStringLiteral("Enter callsign — e.g. KI6BCJ"));
    m_input->setMaxLength(14);
    ThemeManager::instance().applyStyleSheet(m_input,
        "QLineEdit { background: {{color.background.0}}; color: {{color.text.primary}};"
        " border: 1px solid {{color.border.strong}}; border-radius: 3px;"
        " padding: 5px 8px; font-size: 14px; font-weight: bold; }");
    // Callsigns are uppercase; retype as the operator enters them.
    connect(m_input, &QLineEdit::textEdited, this, [this](const QString& t) {
        const QString up = t.toUpper();
        if (up != t)
            m_input->setText(up);
    });
    connect(m_input, &QLineEdit::returnPressed, this, [this] { startLookup(false); });
    inputRow->addWidget(m_input, 1);

    const QString btnStyle =
        "QPushButton { background: {{color.background.2}}; color: {{color.text.primary}};"
        " border: 1px solid {{color.border.strong}}; border-radius: 3px;"
        " padding: 5px 14px; font-size: 12px; }"
        "QPushButton:hover { border-color: {{color.accent}}; color: {{color.accent.bright}}; }"
        "QPushButton:disabled { color: {{color.text.disabled}}; }";

    m_lookupBtn = new QPushButton(QStringLiteral("Lookup"), this);
    m_lookupBtn->setObjectName(QStringLiteral("callsignLookupGo"));
    m_lookupBtn->setAccessibleName(QStringLiteral("Look up callsign"));
    m_lookupBtn->setDefault(true);
    ThemeManager::instance().applyStyleSheet(m_lookupBtn, btnStyle);
    connect(m_lookupBtn, &QPushButton::clicked, this, [this] { startLookup(false); });
    inputRow->addWidget(m_lookupBtn);

    m_refreshBtn = new QPushButton(QStringLiteral("Refresh"), this);
    m_refreshBtn->setObjectName(QStringLiteral("callsignLookupRefresh"));
    m_refreshBtn->setAccessibleName(QStringLiteral("Refresh from QRZ, bypassing the cache"));
    m_refreshBtn->setToolTip(QStringLiteral("Fetch fresh data from QRZ.com even if cached"));
    ThemeManager::instance().applyStyleSheet(m_refreshBtn, btnStyle);
    connect(m_refreshBtn, &QPushButton::clicked, this, [this] { startLookup(true); });
    inputRow->addWidget(m_refreshBtn);

    root->addLayout(inputRow);

    m_card = new CallsignCard(CallsignCard::Variant::Large, this);
    m_card->setVisible(false);
    root->addWidget(m_card);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("callsignLookupStatus"));
    m_status->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(m_status,
        "QLabel { color: {{color.text.label}}; font-size: 11px; background: transparent; }");
    root->addWidget(m_status);

    root->addStretch();

    auto& svc = CallsignLookupService::instance();
    connect(&svc, &CallsignLookupService::infoReady, this,
            [this](const CallsignInfo& info, bool fromCache) {
        if (info.call != m_pendingCall)
            return;
        m_card->setVisible(true);
        m_card->showInfo(info, fromCache);
        if (info.prefixOnly) {
            setStatus(QStringLiteral(
                "QRZ.com unavailable — showing country-level prefix data "
                "(cty.dat). Full details will replace this if QRZ answers."));
        } else if (fromCache) {
            const qint64 ageDays =
                (QDateTime::currentSecsSinceEpoch() - info.fetchedUtc) / 86400;
            setStatus(ageDays > 0
                ? QStringLiteral("From cache — fetched %1 day(s) ago").arg(ageDays)
                : QStringLiteral("From cache — fetched today"));
        } else {
            setStatus(QStringLiteral("Fetched from QRZ.com"));
        }
        const QString photo =
            CallsignLookupService::instance().photoPathFor(info.call);
        if (!photo.isEmpty())
            m_card->setPhotoPath(photo);
    });
    connect(&svc, &CallsignLookupService::photoReady, this,
            [this](const QString& call, const QString& imagePath) {
        if (call == m_pendingCall)
            m_card->setPhotoPath(imagePath);
    });
    connect(&svc, &CallsignLookupService::lookupFailed, this,
            [this](const QString& call, const QString& message) {
        if (call != m_pendingCall)
            return;
        m_card->setVisible(true);
        m_card->showError(call, message);
        setStatus(message);
    });

    if (!svc.hasCredentials()) {
        setStatus(QStringLiteral(
            "QRZ.com account not configured — lookups fall back to "
            "country-level prefix data. Add your username and password in "
            "Radio Setup → QRZ for full details."));
    }
}

void CallsignLookupDialog::setStatus(const QString& text)
{
    m_status->setText(text);
}

void CallsignLookupDialog::lookupCallsign(const QString& call)
{
    m_input->setText(Callsigns::normalized(call));
    startLookup(false);
}

void CallsignLookupDialog::startLookup(bool forceRefresh)
{
    const QString call = Callsigns::normalized(m_input->text());
    if (call.isEmpty())
        return;
    if (!Callsigns::isLikelyCallsign(call)) {
        m_pendingCall = call;
        m_card->setVisible(true);
        m_card->showError(call, QStringLiteral("Doesn't look like a callsign"));
        setStatus({});
        return;
    }
    m_pendingCall = call;
    m_card->setVisible(true);
    m_card->showPending(call);
    setStatus({});
    CallsignLookupService::instance().lookup(call, forceRefresh);
}

} // namespace AetherSDR
