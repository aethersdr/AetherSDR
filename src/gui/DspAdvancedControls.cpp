#include "DspAdvancedControls.h"

#include "ControlAvailabilityRegistry.h"
#include "GuardedSlider.h"
#include "core/ThemeManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QVariant>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <utility>

namespace AetherSDR {
DspAdvancedControls::DspAdvancedControls(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("advancedDspControls"));
    setAttribute(Qt::WA_StyledBackground, true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(3);
    auto* heading = new QHBoxLayout;
    auto* label = new QLabel(tr("Advanced:"), this);
    heading->addWidget(label);
    m_algorithm = new GuardedComboBox(this);
    m_algorithm->setObjectName(QStringLiteral("advancedDspAlgorithm"));
    m_algorithm->setAccessibleName(tr("Advanced DSP algorithm"));
    m_algorithm->setAccessibleDescription(tr("Choose an algorithm to inspect its radio-owned parameters. Selection sends no command."));
    label->setBuddy(m_algorithm);
    using Algorithm = DspParameterResetTarget;
    const QList<QPair<QString, Algorithm>> algorithms = {
        {QStringLiteral("NR"), Algorithm::Nr}, {QStringLiteral("ANF"), Algorithm::Anf},
        {QStringLiteral("NRL"), Algorithm::Nrl}, {QStringLiteral("ANFL"), Algorithm::Anfl},
        {QStringLiteral("ANFT"), Algorithm::Anft}, {QStringLiteral("NRF"), Algorithm::Nrf}};
    for (const auto& [name, algorithm] : algorithms) {
        m_algorithm->addItem(name, static_cast<int>(algorithm));
    }
    heading->addWidget(m_algorithm, 1);
    layout->addLayout(heading);
    m_rows.reserve(dspParameterSpecs().size());
    for (const DspParameterSpec& spec : dspParameterSpecs()) {
        const std::size_t index = m_rows.size();
        Row row;
        row.parameter = spec.parameter;
        row.container = new QWidget(this);
        auto* controls = new QHBoxLayout(row.container);
        controls->setContentsMargins(0, 0, 0, 0);
        controls->setSpacing(3);
        auto* name = new QLabel(tr(spec.label), row.container);
        name->setMinimumWidth(63);
        controls->addWidget(name);
        const QString id = QString::fromLatin1(spec.id);
        QWidget* editor = nullptr;
        if (!spec.choices.isEmpty() || spec.powerOfTwo) {
            row.choices = new GuardedComboBox(row.container);
            if (spec.powerOfTwo) {
                for (int value = spec.minimum; value <= spec.maximum; value *= 2) {
                    row.choices->addItem(QString::number(value), value);
                }
            } else {
                for (int value = 0; value < spec.choices.size(); ++value) {
                    row.choices->addItem(spec.choices[value], value);
                }
            }
            row.choices->setCurrentIndex(-1);
            row.choices->setMinimumWidth(66);
            connect(row.choices, &QComboBox::currentIndexChanged, this, [this, index](int choice) {
                if (m_refreshing) { return; }
                if (choice < 0) { refresh(true); return; }
                const QVariant data = m_rows[index].choices->itemData(choice);
                if (data.metaType().id() == QMetaType::Int) { edited(index, data.toInt()); }
                else { refresh(true); }
            });
            editor = row.choices;
        } else {
            row.spin = new QSpinBox(row.container);
            row.spin->setRange(spec.minimum, spec.maximum);
            row.spin->setKeyboardTracking(false);
            row.spin->setSpecialValueText(tr("Unknown"));
            row.spin->setMinimumWidth(66);
            connect(row.spin, &QSpinBox::valueChanged, this, [this, index](int value) { edited(index, value); });
            editor = row.spin;
        }
        editor->setObjectName(id + QStringLiteral("Editor"));
        editor->setAccessibleName(tr("%1 %2").arg(QString::fromLatin1(
            spec.algorithm == Algorithm::Nr ? "NR" : spec.algorithm == Algorithm::Anf ? "ANF"
            : spec.algorithm == Algorithm::Nrl ? "NRL" : spec.algorithm == Algorithm::Anfl ? "ANFL"
            : spec.algorithm == Algorithm::Anft ? "ANFT" : "NRF"), tr(spec.label)));
        editor->setAccessibleDescription(spec.choices.isEmpty()
            ? tr("%1 to %2%3. Edit, then press Apply for this parameter.").arg(spec.minimum).arg(spec.maximum)
                .arg(spec.powerOfTwo ? tr(", powers of two") : QString())
            : tr("Choose %1, then press Apply for this parameter.").arg(spec.choices.join(QStringLiteral(", "))));
        editor->setFixedHeight(22);
        name->setBuddy(editor);
        controls->addWidget(editor, 1);
        row.apply = new QPushButton(tr("Apply"), row.container);
        row.apply->setObjectName(id + QStringLiteral("Apply"));
        row.apply->setAccessibleName(tr("Apply %1").arg(editor->accessibleName()));
        row.apply->setAutoDefault(false);
        row.apply->setFixedSize(44, 22);
        controls->addWidget(row.apply);
        connect(row.apply, &QPushButton::pressed, this, [this, index] {
            Row& selected = m_rows[index];
            selected.pressed = selected.edit && current(*selected.edit) ? selected.edit : std::nullopt;
        });
        connect(row.apply, &QPushButton::clicked, this, [this, index] { apply(index); });
        m_rows.push_back(std::move(row));
        layout->addWidget(m_rows.back().container);
    }
    m_reason = new QLabel(this);
    m_reason->setObjectName(QStringLiteral("advancedDspReason"));
    m_reason->setWordWrap(true);
    layout->addWidget(m_reason);
    m_notice = new QLabel(tr("Values follow radio status. Apply sends one parameter."), this);
    m_notice->setObjectName(QStringLiteral("advancedDspNotice"));
    m_notice->setWordWrap(true);
    layout->addWidget(m_notice);
    ThemeManager::instance().applyStyleSheet(this, QStringLiteral(
        "QWidget#advancedDspControls { background: {{color.canon.nested}}; }"
        "QLabel { color: {{color.canon.muted}}; font-size: 10px; }"
        "QPushButton, QComboBox, QSpinBox { background: {{color.canon.control}};"
        "color: {{color.canon.cyan}}; border: 1px solid {{color.canon.lineHi}};"
        "border-radius: 3px; padding: 1px 3px; font-size: 11px; }"
        "QPushButton:hover, QComboBox:hover, QSpinBox:hover { background: {{color.canon.nested}};"
        "color: {{color.canon.aqua}}; }"
        "QPushButton:disabled, QComboBox:disabled, QSpinBox:disabled { color: {{color.control.unavailable}}; }"
        "QPushButton:focus, QComboBox:focus, QSpinBox:focus { border-color: {{color.canon.cyan}}; }"));
    connect(m_algorithm, &QComboBox::currentIndexChanged, this, [this] {
        refresh(true, {}, true);
    });
    refresh(true);
}

void DspAdvancedControls::setContext(RadioModel* radio, SliceModel* slice)
{
    for (const QMetaObject::Connection& connection : m_connections) { disconnect(connection); }
    m_connections.clear();
    delete m_availability;
    m_availability = nullptr;
    m_radio = radio;
    m_slice = slice;
    const quint64 binding = ++m_binding;
    const QPointer<RadioModel> boundRadio = radio;
    const QPointer<SliceModel> boundSlice = slice;
    const auto update = [this, boundRadio, boundSlice] {
        if (m_radio == boundRadio && m_slice == boundSlice) { refresh(); }
    };
    if (radio) {
        m_connections.push_back(connect(radio, &RadioModel::connectionStateChanged, this, update));
        m_connections.push_back(connect(radio, &RadioModel::capabilitiesChanged, this, update));
        m_connections.push_back(connect(radio, &RadioModel::infoChanged, this, update));
        m_connections.push_back(connect(radio, &RadioModel::licenseFeaturesChanged, this, update));
        m_connections.push_back(connect(radio, &RadioModel::slotOccupancyChanged, this, update));
        m_connections.push_back(connect(radio, &QObject::destroyed, this, [this, binding] {
            if (m_binding != binding) { return; }
            delete std::exchange(m_availability, nullptr);
            refresh(true);
        }));
        m_availability = new ControlAvailabilityRegistry(*radio, this);
        for (std::size_t index = 0; index < m_rows.size(); ++index) {
            Row& row = m_rows[index];
            QWidget* editor = row.spin ? static_cast<QWidget*>(row.spin) : row.choices;
            const QString reason = tr("Advanced DSP requires supported firmware, hardware, entitlement and current radio status");
            m_availability->registerWidget(editor, reason,
                [this, index](bool, const RadioCapabilities&) { return m_rows[index].available; },
                [] { return true; }, false);
            m_availability->registerWidget(row.apply, tr("Edit a confirmed parameter before pressing Apply"),
                [this, index](bool, const RadioCapabilities&) { return m_rows[index].canApply; },
                [] { return true; }, false);
        }
    }
    if (slice) {
        m_connections.push_back(connect(slice, &SliceModel::dspParametersChanged, this, [this, boundRadio, boundSlice] {
            if (m_radio == boundRadio && m_slice == boundSlice) { refresh(true); }
        }));
        m_connections.push_back(connect(slice, &SliceModel::modeChanged, this, update));
        m_connections.push_back(connect(slice, &SliceModel::receiveObservationChanged, this, update));
        m_connections.push_back(connect(slice, &SliceModel::externalReceiveReplacementChanged, this, update));
        m_connections.push_back(connect(slice, &QObject::destroyed, this, [this, binding] {
            if (m_binding == binding) { refresh(true); }
        }));
    }
    refresh(true);
}

bool DspAdvancedControls::current(const Edit& edit) const
{
    return edit.radio && edit.slice && edit.radio == m_radio && edit.slice == m_slice
        && edit.session == m_radio->receiveControlSession()
        && edit.observation == m_slice->dspParametersRevision();
}

void DspAdvancedControls::edited(std::size_t index, int value)
{
    if (m_refreshing || !m_radio || !m_slice) { return; }
    Row& row = m_rows[index];
    if (!validDspParameterValue(row.parameter, value)
        || !m_radio->dspParameterUnavailableReason(m_slice, m_radio->receiveControlSession(), row.parameter).isEmpty()) {
        refresh(true);
        return;
    }
    row.edit = Edit{m_radio, m_slice, m_radio->receiveControlSession(), m_slice->dspParametersRevision(), value};
    row.pressed.reset();
    refresh();
}

void DspAdvancedControls::apply(std::size_t index)
{
    Row& row = m_rows[index];
    const std::optional<Edit> intent = std::exchange(row.pressed, std::nullopt);
    if (!intent || !row.edit || !current(*intent) || intent->value != row.edit->value) {
        refresh(true);
        return;
    }
    const QPointer<DspAdvancedControls> alive(this);
    const quint64 binding = m_binding;
    const bool sent = m_radio->requestDspParameter(m_slice, intent->session, {row.parameter, intent->value});
    if (!alive || m_binding != binding) { return; }
    refresh(true, sent ? tr("Requested. Waiting for radio status.")
        : tr("Request unavailable. Edit again after radio status updates."));
}

void DspAdvancedControls::refresh(bool discardEdits, const QString& notice, bool forceLayout)
{
    m_refreshing = true;
    const QString previousReason = m_reason->text();
    const QString previousNotice = m_notice->text();
    QStringList reasons;
    const auto algorithm = static_cast<DspParameterResetTarget>(m_algorithm->currentData().toInt());
    for (Row& row : m_rows) {
        const DspParameterSpec& spec = *dspParameterSpec(row.parameter);
        const bool selected = spec.algorithm == algorithm;
        row.container->setVisible(selected);
        const std::optional<int> observed = m_slice ? m_slice->dspParameter(row.parameter) : std::nullopt;
        const QString reason = m_radio
            ? m_radio->dspParameterUnavailableReason(m_slice, m_radio->receiveControlSession(), row.parameter)
            : tr("Connect to a radio to inspect advanced DSP parameters");
        if (discardEdits || !reason.isEmpty() || (row.edit && !current(*row.edit))) {
            row.edit.reset();
            row.pressed.reset();
        }
        row.available = reason.isEmpty();
        row.canApply = row.available && row.edit && observed && row.edit->value != *observed;
        const std::optional<int> displayed = row.edit ? std::optional(row.edit->value) : observed;
        QWidget* editor = row.spin ? static_cast<QWidget*>(row.spin) : row.choices;
        const QSignalBlocker blocker(editor);
        if (row.spin) {
            row.spin->setSpecialValueText(displayed ? QString() : tr("Unknown"));
            row.spin->setValue(displayed.value_or(spec.minimum));
        } else {
            row.choices->setCurrentIndex(displayed ? row.choices->findData(*displayed) : -1);
        }
        if (!m_availability) { editor->setEnabled(false); row.apply->setEnabled(false); }
        if (selected && !reason.isEmpty() && !reasons.contains(reason)) { reasons.append(reason); }
    }
    if (m_availability) { m_availability->refreshEngaged(); }
    for (const Row& row : m_rows) {
        QWidget* editor = row.spin ? static_cast<QWidget*>(row.spin) : row.choices;
        const DspParameterSpec& spec = *dspParameterSpec(row.parameter);
        const QString reason = m_radio
            ? m_radio->dspParameterUnavailableReason(m_slice, m_radio->receiveControlSession(), row.parameter)
            : tr("Connect to a radio to inspect advanced DSP parameters");
        const QString description = reason.isEmpty()
            ? tr("Radio-owned value. Edit, then press Apply for this parameter. Range %1 to %2.")
                .arg(spec.minimum).arg(spec.maximum) : reason;
        editor->setToolTip(description);
        editor->setAccessibleDescription(description);
        row.apply->setToolTip(!reason.isEmpty() ? reason : row.canApply
            ? tr("Request only this parameter from the radio") : tr("Edit this parameter before pressing Apply"));
        row.apply->setAccessibleDescription(row.apply->toolTip());
    }
    m_reason->setText(reasons.join(QStringLiteral("\n")));
    m_reason->setVisible(!reasons.isEmpty());
    if (discardEdits) { m_notice->setText(tr("Values follow radio status. Apply sends one parameter.")); }
    if (!notice.isNull()) { m_notice->setText(notice); }
    m_refreshing = false;
    if (forceLayout || m_reason->text() != previousReason || m_notice->text() != previousNotice) {
        if (layout()) { layout()->invalidate(); layout()->activate(); }
        updateGeometry();
        emit contentChanged();
    }
}
} // namespace AetherSDR
