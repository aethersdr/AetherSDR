#include "WaveformEnvironmentDialog.h"
#include "CanonIndicators.h"

#include "core/ThemeManager.h"
#include "models/RadioModel.h"

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QStyle>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// The Waveforms family's canon sheet (TailnetShimDialog), trimmed to what an
// editor needs. State colours always travel with a word.
constexpr const char* kEnvironmentStyle =
    "QWidget#wfEnvBody { background: transparent; }"
    "QWidget#wfEnvRows { background: transparent; }"
    "QLabel { color: {{color.canon.inkSoft}}; background: transparent; border: none; font-size: 12px; }"
    "QLabel#wfEnvTitle { color: {{color.canon.ink}}; font-size: 20px; font-weight: bold; }"
    "QLabel#wfEnvSubtitle { color: {{color.canon.muted}}; font-size: 13px; }"
    "QLabel[wfEnvRole=\"key\"] { color: {{color.canon.muted}}; font-size: 10px; font-weight: bold; }"
    "QLabel[wfEnvRole=\"hint\"] { color: {{color.canon.muted}}; font-size: 11px; }"
    "QLabel[wfEnvRole=\"message\"][tone=\"error\"] { color: {{color.accent.danger}}; }"
    "QLabel[wfEnvRole=\"message\"][tone=\"ok\"] { color: {{color.accent.success}}; }"
    "QLabel[wfEnvRole=\"message\"][tone=\"pending\"] { color: {{color.accent.warning}}; }"
    "QFrame#wfEnvRule { background: {{color.canon.line}}; border: none; min-height: 1px; max-height: 1px; }"
    "QScrollArea { background: transparent; border: none; }"
    "QLineEdit { background: {{color.canon.control}}; color: {{color.canon.ink}};"
    " border: 1px solid {{color.canon.lineHi}}; border-radius: 4px; padding: 5px 8px; font-size: 12px;"
    " selection-background-color: {{color.canon.cyan}}; selection-color: {{color.canon.onAccent}}; }"
    "QLineEdit:focus { border-color: {{color.canon.aqua}}; }"
    "QLineEdit:disabled { color: {{color.canon.muted}}; border-color: {{color.canon.line}}; }"
    "QCheckBox { color: {{color.canon.inkSoft}}; background: transparent; font-size: 12px; spacing: 8px; }"
    "QCheckBox:disabled { color: {{color.canon.muted}}; }"
    "QPushButton { background: {{color.canon.control}}; color: {{color.canon.cyan}};"
    " border: 1px solid {{color.canon.lineHi}}; border-radius: 4px; padding: 6px 16px;"
    " font-size: 12px; font-weight: bold; }"
    "QPushButton:hover { background: {{color.canon.nested}}; color: {{color.canon.aqua}}; }"
    "QPushButton:focus { border-color: {{color.canon.aqua}}; }"
    "QPushButton:disabled { background: transparent; color: {{color.canon.muted}};"
    " border-color: {{color.canon.line}}; }"
    "QPushButton#wfEnvApply { background: {{color.brand.gradient}}; color: {{color.canon.onAccent}};"
    " border: 1px solid transparent; border-radius: 4px; padding: 6px 22px; }"
    "QPushButton#wfEnvApply:hover { border-color: {{color.canon.aqua}}; }"
    "QPushButton#wfEnvApply:focus { border: 2px solid {{color.canon.aqua}}; padding: 5px 21px; }"
    "QPushButton#wfEnvApply:disabled { background: transparent; color: {{color.canon.muted}};"
    " border-color: {{color.canon.line}}; }";

void repolish(QWidget* w)
{
    w->style()->unpolish(w);
    w->style()->polish(w);
}

} // namespace

QString waveformEnvironmentUnavailableReason(const RadioModel* model)
{
    if (!model || !model->isConnected()) {
        return QObject::tr("Connect to a radio to edit container environments.");
    }
    const RadioCapabilities caps = model->backendCapabilities();
    if (!caps.hasWaveforms || !caps.waveformEnvControl.has_value()) {
        return QObject::tr("Container environments need a radio with a Waveform Processor "
                           "running SmartSDR v4.3.16 or later.");
    }
    return {};
}

QString waveformEnvironmentInstallBlocker(const RadioModel* model)
{
    if (!model) {
        return {};
    }
    if (model->waveformEnvironmentApplyActive()) {
        return QObject::tr("Wait for the container environment change to finish.");
    }
    if (model->waveformEnvironmentStagingUncertain()
        && model->backendCapabilities().waveformEnvControl.has_value()) {
        return QObject::tr("The radio may still hold staged environment variables. Open "
                           "Environment… on a container and click Clear Staged Variables.");
    }
    return {};
}

QString WaveformEnvironmentDialog::emptyEnvironmentReason()
{
    return tr("Add at least one variable. When nothing is staged the radio keeps the "
              "container's current variables, so to empty one, give it an empty value instead.");
}

QString WaveformEnvironmentDialog::collectEnvironment(const QList<QPair<QString, QString>>& rows,
                                                      WaveformEnvironment* environment)
{
    WaveformEnvironment result;
    QSet<QString> keys;
    int number = 0;
    for (const auto& [rawKey, value] : rows) {
        ++number;
        const QString key = rawKey.trimmed();
        if (key.isEmpty() && value.isEmpty()) { continue; }
        if (key.isEmpty()) {
            return tr("Row %1 has a value but no variable name.").arg(number);
        }
        if (!WaveformEnvRequest::keyValid(key)) {
            return tr("\"%1\" is not a valid variable name. Use letters, digits and underscores, "
                      "starting with a letter or underscore, up to %2 characters.")
                .arg(key).arg(WaveformEnvRequest::kMaxKeyChars);
        }
        if (keys.contains(key)) {
            return tr("%1 is listed more than once.").arg(key);
        }
        // Refused, never altered: a value the radio reported is applied exactly
        // or not at all, so editing one row cannot change another.
        switch (WaveformEnvRequest::valueIssue(value)) {
        case WaveformEnvRequest::ValueIssue::None: break;
        case WaveformEnvRequest::ValueIssue::TooLong:
            return tr("The value of %1 is %2 characters; AetherSDR sends at most %3.")
                .arg(key).arg(value.size()).arg(WaveformEnvRequest::kMaxValueChars);
        case WaveformEnvRequest::ValueIssue::ControlCharacter:
            return tr("The value of %1 must be on one line, without tabs or control characters.").arg(key);
        case WaveformEnvRequest::ValueIssue::EdgeWhitespace:
            return tr("The value of %1 starts or ends with a space. The radio would not keep it; "
                      "remove the space.").arg(key);
        case WaveformEnvRequest::ValueIssue::RepeatedWhitespace:
            return tr("The value of %1 has two spaces in a row. The radio collapses them into "
                      "one; use single spaces.").arg(key);
        }
        keys.insert(key);
        result.append({key, value});
    }
    if (result.isEmpty()) {
        return emptyEnvironmentReason();
    }
    if (environment) { *environment = result; }
    return {};
}

WaveformEnvironmentDialog::WaveformEnvironmentDialog(RadioModel* model, const QString& containerName,
                                                     QWidget* parent)
    : CanonWindow(tr("Waveform Environment"), parent)
    , m_model(model)
    , m_name(containerName)
{
    constexpr int kWidth = 620;
    constexpr int kSide = 28;
    setFixedWidth(kWidth);
    bodyWidget()->setObjectName(QStringLiteral("wfEnvBody"));
    applyCanonSheet(bodyWidget(), [] {
        return QString::fromLatin1(kEnvironmentStyle) + canonIndicatorRules();
    });

    auto* layout = new QVBoxLayout(bodyWidget());
    layout->setContentsMargins(kSide, 26, kSide, 22);
    layout->setSpacing(12);
    QWidget* body = bodyWidget();

    auto* title = new QLabel(tr("Environment"), body);
    title->setObjectName(QStringLiteral("wfEnvTitle"));
    title->setAlignment(Qt::AlignCenter);
    title->setAccessibleName(tr("Environment of waveform %1").arg(m_name));
    layout->addWidget(title);
    auto* subtitle = new QLabel(m_name, body);
    subtitle->setObjectName(QStringLiteral("wfEnvSubtitle"));
    subtitle->setAlignment(Qt::AlignCenter);
    subtitle->setTextFormat(Qt::PlainText);
    subtitle->setAccessibleName(tr("Container %1").arg(m_name));
    layout->addWidget(subtitle);
    auto* rule = new QFrame(body);
    rule->setObjectName(QStringLiteral("wfEnvRule"));
    layout->addWidget(rule);

    auto* intro = new QLabel(
        tr("Variables the radio passes to this container. Apply & Recreate replaces the whole "
           "environment and rebuilds the container from its installed image, which restarts "
           "the waveform."), body);
    intro->setWordWrap(true);
    intro->setProperty("wfEnvRole", QStringLiteral("hint"));
    intro->setAccessibleName(tr("About container environments"));
    layout->addWidget(intro);

    auto* header = new QHBoxLayout;
    header->setSpacing(8);
    auto* keyHeader = new QLabel(tr("VARIABLE"), body);
    keyHeader->setProperty("wfEnvRole", QStringLiteral("key"));
    auto* valueHeader = new QLabel(tr("VALUE"), body);
    valueHeader->setProperty("wfEnvRole", QStringLiteral("key"));
    header->addWidget(keyHeader, 2);
    header->addWidget(valueHeader, 3);
    header->addSpacing(90);
    layout->addLayout(header);

    auto* scroll = new QScrollArea(body);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumHeight(180);
    scroll->setAccessibleName(tr("Environment variables"));
    m_rowsContainer = new QWidget(scroll);
    m_rowsContainer->setObjectName(QStringLiteral("wfEnvRows"));
    m_rowsLayout = new QVBoxLayout(m_rowsContainer);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(6);
    m_emptyLabel = new QLabel(tr("The radio reports no variables for this container."), m_rowsContainer);
    m_emptyLabel->setProperty("wfEnvRole", QStringLiteral("hint"));
    m_emptyLabel->setAccessibleName(m_emptyLabel->text());
    m_rowsLayout->addWidget(m_emptyLabel);
    m_rowsLayout->addStretch(1);
    scroll->setWidget(m_rowsContainer);
    layout->addWidget(scroll, 1);

    auto* tools = new QHBoxLayout;
    tools->setSpacing(10);
    m_addButton = new QPushButton(tr("Add Variable"), body);
    m_addButton->setAccessibleName(tr("Add an environment variable"));
    connect(m_addButton, &QPushButton::clicked, this, [this] {
        addRow({}, {});
        if (!m_rows.isEmpty()) { m_rows.last().key->setFocus(); }
    });
    m_showValues = new QCheckBox(tr("Show values"), body);
    m_showValues->setAccessibleName(tr("Show environment values"));
    m_showValues->setAccessibleDescription(
        tr("Values are hidden because they may hold passwords or keys."));
    connect(m_showValues, &QCheckBox::toggled, this, [this](bool show) {
        for (const Row& row : m_rows) {
            row.value->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
        }
    });
    tools->addWidget(m_addButton);
    tools->addWidget(m_showValues);
    tools->addStretch(1);
    layout->addLayout(tools);

    // Visible whenever Apply is dimmed for an empty list, so the reason is not
    // only in a tooltip.
    m_applyHint = new QLabel(emptyEnvironmentReason(), body);
    m_applyHint->setProperty("wfEnvRole", QStringLiteral("hint"));
    m_applyHint->setWordWrap(true);
    m_applyHint->setAccessibleName(tr("Why Apply is unavailable"));
    m_applyHint->setAccessibleDescription(m_applyHint->text());
    layout->addWidget(m_applyHint);

    m_message = new QLabel(body);
    m_message->setProperty("wfEnvRole", QStringLiteral("message"));
    m_message->setWordWrap(true);
    m_message->setTextFormat(Qt::PlainText);
    m_message->setAccessibleName(tr("Environment status"));
    layout->addWidget(m_message);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(10);
    m_revertButton = new QPushButton(tr("Revert"), body);
    m_revertButton->setAccessibleName(tr("Revert to the environment the radio reports"));
    connect(m_revertButton, &QPushButton::clicked, this, [this] {
        loadFromRadio();
        setMessage({}, {});
    });
    auto* closeButton = new QPushButton(tr("Close"), body);
    closeButton->setAccessibleName(tr("Close the environment editor"));
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    m_applyButton = new QPushButton(tr("Apply && Recreate"), body);
    m_applyButton->setObjectName(QStringLiteral("wfEnvApply"));
    m_applyButton->setAccessibleName(tr("Apply the environment and recreate waveform %1").arg(m_name));
    connect(m_applyButton, &QPushButton::clicked, this, &WaveformEnvironmentDialog::onApply);
    // Shown only while this client cannot tell whether the radio still holds
    // variables it staged; installs wait until a clear is acknowledged.
    m_clearStagingButton = new QPushButton(tr("Clear Staged Variables"), body);
    m_clearStagingButton->setAccessibleName(tr("Clear the radio's staged environment variables"));
    m_clearStagingButton->setAccessibleDescription(
        tr("The radio may still hold variables staged for the next install."));
    connect(m_clearStagingButton, &QPushButton::clicked, this, &WaveformEnvironmentDialog::onClearStaging);
    buttons->addWidget(m_revertButton);
    buttons->addWidget(m_clearStagingButton);
    buttons->addStretch(1);
    buttons->addWidget(closeButton);
    buttons->addWidget(m_applyButton);
    layout->addLayout(buttons);

    if (m_model) {
        connect(m_model, &RadioModel::waveformContainersChanged, this, [this](const QString& name) {
            if (name.isEmpty() || name == m_name) { onRadioEnvironmentChanged(); }
        });
        connect(m_model, &RadioModel::capabilitiesChanged,
                this, &WaveformEnvironmentDialog::updateAvailability);
        connect(m_model, &RadioModel::connectionStateChanged,
                this, &WaveformEnvironmentDialog::updateAvailability);
        connect(m_model, &RadioModel::waveformEnvironmentStagingChanged,
                this, &WaveformEnvironmentDialog::updateAvailability);
    }
    loadFromRadio();
    updateAvailability();
}

void WaveformEnvironmentDialog::loadFromRadio()
{
    for (const Row& row : std::as_const(m_rows)) {
        row.widget->deleteLater();
    }
    m_rows.clear();
    const auto reported = m_model ? m_model->waveformContainerEnvironment(m_name) : std::nullopt;
    m_containerPresent = reported.has_value();
    m_loaded = reported.value_or(QMap<QString, QString>{});
    for (auto it = m_loaded.cbegin(); it != m_loaded.cend(); ++it) {
        addRow(it.key(), it.value());
    }
    m_emptyLabel->setVisible(m_rows.isEmpty());
}

void WaveformEnvironmentDialog::addRow(const QString& key, const QString& value)
{
    auto* rowWidget = new QWidget(m_rowsContainer);
    auto* rowLayout = new QHBoxLayout(rowWidget);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);
    Row row;
    row.widget = rowWidget;
    // No setMaxLength: Qt would cut a longer value the radio reported the moment
    // it loads. Lengths are checked, and refused visibly, only on Apply.
    row.key = new QLineEdit(key, rowWidget);
    row.key->setPlaceholderText(tr("NAME"));
    row.value = new QLineEdit(value, rowWidget);
    // Permanent: Show values must never expose the text to automation dumps or
    // grabs (AutomationSensitiveLineEdit.h).
    row.value->setProperty("aetherSensitiveValue", true);
    row.value->setEchoMode(m_showValues && m_showValues->isChecked()
                               ? QLineEdit::Normal : QLineEdit::Password);
    auto* remove = new QPushButton(tr("Remove"), rowWidget);
    // Names fixed at creation, so a script or screen reader can keep addressing
    // the row while its variable name is typed; the description follows the name.
    const QString handle = key.isEmpty() ? tr("new variable %1").arg(++m_newRowCount) : key;
    row.key->setAccessibleName(tr("Name of %1").arg(handle));
    row.value->setAccessibleName(tr("Value of %1").arg(handle));
    remove->setAccessibleName(tr("Remove %1").arg(handle));
    const auto describe = [row, remove] {
        const QString name = row.key->text().trimmed();
        const QString text = name.isEmpty() ? tr("No name yet") : tr("Variable %1").arg(name);
        row.key->setAccessibleDescription(text);
        row.value->setAccessibleDescription(text);
        remove->setAccessibleDescription(text);
    };
    describe();
    connect(row.key, &QLineEdit::textChanged, rowWidget, describe);
    connect(row.key, &QLineEdit::textChanged, this, &WaveformEnvironmentDialog::updateAvailability);
    connect(row.value, &QLineEdit::textChanged, this, &WaveformEnvironmentDialog::updateAvailability);
    connect(remove, &QPushButton::clicked, this, [this, rowWidget] { removeRow(rowWidget); });
    rowLayout->addWidget(row.key, 2);
    rowLayout->addWidget(row.value, 3);
    rowLayout->addWidget(remove);
    m_rowsLayout->insertWidget(m_rowsLayout->count() - 1, rowWidget);
    m_rows.append(row);
    m_emptyLabel->setVisible(false);
    const bool editable = !m_applying && m_containerPresent
        && waveformEnvironmentUnavailableReason(m_model).isEmpty();
    rowWidget->setEnabled(editable);
    if (m_applyButton) { updateAvailability(); }
}

void WaveformEnvironmentDialog::removeRow(QWidget* rowWidget)
{
    for (qsizetype i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).widget == rowWidget) {
            m_rows.removeAt(i);
            break;
        }
    }
    rowWidget->deleteLater();
    m_emptyLabel->setVisible(m_rows.isEmpty());
    updateAvailability();
}

bool WaveformEnvironmentDialog::hasRows() const
{
    for (const Row& row : m_rows) {
        if (!row.key->text().trimmed().isEmpty() || !row.value->text().isEmpty()) { return true; }
    }
    return false;
}

QList<QPair<QString, QString>> WaveformEnvironmentDialog::rowValues() const
{
    QList<QPair<QString, QString>> values;
    for (const Row& row : m_rows) {
        values.append({row.key->text(), row.value->text()});
    }
    return values;
}

void WaveformEnvironmentDialog::updateAvailability()
{
    QString reason = waveformEnvironmentUnavailableReason(m_model);
    if (reason.isEmpty() && !m_containerPresent) {
        reason = tr("%1 is no longer installed on the radio.").arg(m_name);
    }
    const bool editable = reason.isEmpty() && !m_applying;
    const bool empty = !hasRows();
    for (const Row& row : std::as_const(m_rows)) {
        row.widget->setEnabled(editable);
    }
    m_addButton->setEnabled(editable);
    m_revertButton->setEnabled(!m_applying && m_containerPresent);
    m_applyButton->setEnabled(editable && !empty);
    const QString tip = m_applying ? tr("Waiting for the radio to recreate the container.")
        : !reason.isEmpty() ? reason
        : empty ? emptyEnvironmentReason() : QString();
    for (QWidget* control : {static_cast<QWidget*>(m_addButton), static_cast<QWidget*>(m_applyButton)}) {
        const QString text = control == m_addButton && reason.isEmpty() && !m_applying ? QString() : tip;
        control->setToolTip(text);
        control->setAccessibleDescription(text);
    }
    m_applyHint->setVisible(editable && empty);
    const bool uncertain = m_model && m_model->waveformEnvironmentStagingUncertain()
        && !m_model->waveformEnvironmentApplyActive();
    m_clearStagingButton->setVisible(uncertain);
    m_clearStagingButton->setEnabled(uncertain && waveformEnvironmentUnavailableReason(m_model).isEmpty());
    if (!reason.isEmpty() && !m_applying) {
        setMessage(reason, QStringLiteral("error"));
    }
}

void WaveformEnvironmentDialog::setMessage(const QString& text, const QString& tone)
{
    m_message->setText(text);
    m_message->setProperty("tone", tone);
    m_message->setVisible(!text.isEmpty());
    m_message->setAccessibleDescription(text);
    repolish(m_message);
}

void WaveformEnvironmentDialog::onRadioEnvironmentChanged()
{
    if (m_applying) { return; }
    const auto current = m_model ? m_model->waveformContainerEnvironment(m_name) : std::nullopt;
    const bool present = current.has_value();
    const QMap<QString, QString> reported = current.value_or(QMap<QString, QString>{});
    if (present == m_containerPresent && reported == m_loaded) { return; }
    // Unedited rows simply follow the radio; edited ones are the operator's until
    // they choose Revert.
    QMap<QString, QString> shown;
    for (const auto& [key, value] : rowValues()) {
        if (!key.trimmed().isEmpty() || !value.isEmpty()) { shown.insert(key.trimmed(), value); }
    }
    if (shown == m_loaded || !present) {
        loadFromRadio();
    } else if (shown == reported) {
        // The radio caught up with what is on screen (after a recreate whose
        // new environment arrived late): nothing to tell the operator.
        m_loaded = reported;
        m_containerPresent = present;
    } else {
        m_containerPresent = present;
        setMessage(tr("The radio reported a different environment. Revert shows it."),
                   QStringLiteral("pending"));
    }
    updateAvailability();
}

void WaveformEnvironmentDialog::onApply()
{
    WaveformEnvironment environment;
    const QString problem = collectEnvironment(rowValues(), &environment);
    if (!problem.isEmpty()) {
        setMessage(problem, QStringLiteral("error"));
        return;
    }
    m_applying = true;
    updateAvailability();
    setMessage(tr("Recreating %1…").arg(m_name), QStringLiteral("pending"));
    if (!m_applyHandler || !m_applyHandler(m_name, environment)) {
        // A result may already have arrived synchronously; only an apply that
        // never started still needs its reason.
        if (m_applying) {
            m_applying = false;
            const QString reason = waveformEnvironmentUnavailableReason(m_model);
            setMessage(reason.isEmpty() ? tr("Another environment change is still in progress.")
                                        : reason, QStringLiteral("error"));
            updateAvailability();
        }
    }
}

void WaveformEnvironmentDialog::showApplyResult(const WaveformEnvApplyResult& result)
{
    m_applying = false;
    using Outcome = WaveformEnvApplyResult::Outcome;
    QString text;
    bool ok = false;
    switch (result.outcome) {
    case Outcome::Recreated:
        if (result.environmentConfirmed) {
            loadFromRadio();
            text = tr("Recreated. These are the variables the radio now reports.");
        } else {
            // The rows stay as applied and follow the radio's next report; they
            // are not presented as confirmed.
            text = tr("Recreated, but the radio has not yet reported the new variables.");
        }
        ok = result.environmentConfirmed;
        break;
    case Outcome::RecreateFailed:
        text = tr("The radio could not recreate %1.").arg(m_name);
        break;
    case Outcome::NoConfirmation:
        text = tr("The radio accepted the recreate but has not reported the result. "
                  "Check the waveform before relying on it.");
        break;
    case Outcome::Refused:
    case Outcome::Cancelled:
        text = tr("Nothing was recreated: %1.").arg(result.message);
        break;
    }
    if (!result.stagingCleared) {
        ok = false;
        text += QLatin1Char(' ') + tr("Clearing the radio's staged variables failed (%1), so Docker "
                                      "installs stay blocked until Clear Staged Variables succeeds.")
                                       .arg(result.stagingMessage);
    }
    setMessage(text, ok ? QStringLiteral("ok") : QStringLiteral("error"));
    updateAvailability();
}

void WaveformEnvironmentDialog::onClearStaging()
{
    if (!m_model) { return; }
    const QPointer<WaveformEnvironmentDialog> self(this);
    const bool sent = m_model->clearWaveformEnvironmentStaging(this, [self](const RadioControlReply& reply) {
        if (!self) { return; }
        if (reply.kind == RadioControlReply::Kind::Accepted) {
            self->setMessage(tr("The radio's staged variables are cleared."), QStringLiteral("ok"));
        } else {
            self->setMessage(tr("Clearing staged variables failed: %1.")
                                 .arg(reply.message.trimmed().isEmpty() ? tr("no reason given")
                                                                        : reply.message.trimmed()),
                             QStringLiteral("error"));
        }
        self->updateAvailability();
    });
    if (!sent) {
        setMessage(tr("Clearing staged variables could not be sent."), QStringLiteral("error"));
    }
}

} // namespace AetherSDR
