#include "NativeFilterSettingsWidget.h"

#include <QAccessible>
#include <QAccessibleValueChangeEvent>

#include "ControlAvailabilityRegistry.h"
#include "GuardedSlider.h"
#include "models/RadioModel.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMetaType>
#include <QPushButton>
#include <QStandardItemModel>
#include <QSignalBlocker>
#include <QStringList>
#include <QSet>
#include <QVBoxLayout>


namespace AetherSDR {
namespace {
bool revisionValid(const QVariant& value, quint64& revision)
{
    if (value.typeId() != QMetaType::QString || value.toString().size() > 20) { return false; }
    bool valid = false;
    revision = value.toString().toULongLong(&valid);
    return valid && QString::number(revision) == value.toString();
}
QString filterNamespace(const RadioCapabilities& caps)
{
    for (const QString& ns : caps.extensionNamespaces) {
        if (caps.extensions.value(ns).toMap().value(QStringLiteral("nativeFilterControlVersion")).toInt() == 1) {
            return ns;
        }
    }
    return {};
}
QString bankNamespace(const RadioCapabilities& caps)
{
    for (const QString& ns : caps.extensionNamespaces) {
        const QVariant version = caps.extensions.value(ns).toMap().value(QStringLiteral("nativeFilterBankControlVersion"));
        if (version.typeId() == QMetaType::Int && version.toInt() == 1) { return ns; }
    }
    return {};
}
QString fmNamespace(const RadioCapabilities& caps)
{
    for (const QString& ns : caps.extensionNamespaces) {
        const QVariant version = caps.extensions.value(ns).toMap().value("nativeFmWidthControlVersion");
        if (version.typeId() == QMetaType::Int && version.toInt() == 1) { return ns; }
    }
    return {};
}
QString bankRowId(int row)
{
    if (row == 0) { return QStringLiteral("active"); }
    const QStringList fields{QStringLiteral("roofing"), QStringLiteral("ifShape"), QStringLiteral("afType")};
    return QStringLiteral("bank%1.%2").arg((row - 1) / 3).arg(fields[(row - 1) % 3]);
}
QVariantMap bankRow(const QVariantMap& status, int row)
{
    for (const QVariant& item : status.value(QStringLiteral("controls")).toList()) {
        if (item.toMap().value(QStringLiteral("id")).toString() == bankRowId(row)) { return item.toMap(); }
    }
    return {};
}
quint64 nextFilterRequest()
{
    static quint64 sequence = 0x46494c0000000000ULL;
    return ++sequence;
}
QVariantMap axisFor(const QVariantMap& status, int axis)
{
    const QString id = axis == 0 ? QStringLiteral("low") : QStringLiteral("high");
    for (const QVariant& entry : status.value(QStringLiteral("axes")).toList()) {
        const QVariantMap value = entry.toMap();
        if (value.value(QStringLiteral("id")).toString() == id) { return value; }
    }
    return {};
}
}

NativeFilterSettingsWidget::NativeFilterSettingsWidget(RadioModel& model, QWidget* parent)
    : QWidget(parent), m_model(model), m_availability(new ControlAvailabilityRegistry(model, this)),
      m_axes{new QComboBox(this), new QComboBox(this)},
      m_labels{new QLabel(this), new QLabel(this)},
      m_mode(new QLabel(this)), m_status(new QLabel(this))
{
    setObjectName(QStringLiteral("nativeFilterSettings"));
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(tr("These are the radio's native receive-filter settings for the current mode. "
        "Audio low/high cuts are not RF channel widths. Each selection changes one setting and waits for radio readback."), this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    m_mode->setObjectName(QStringLiteral("nativeFilterMode"));
    layout->addWidget(m_mode);
    auto* group = new QGroupBox(tr("Native receive filters"), this);
    auto* form = new QFormLayout(group);
    layout->addWidget(group);
    for (int axis = 0; axis < 2; ++axis) {
        QComboBox* combo = m_axes[axis];
        combo->setObjectName(axis == 0 ? QStringLiteral("nativeFilterLow") : QStringLiteral("nativeFilterHigh"));
        m_labels[axis]->setBuddy(combo);
        form->addRow(m_labels[axis], combo);
        m_availability->registerWidget(combo,
            tr("Requires a connected radio with native filter control, confirmed settings for this mode, and receive operation. Some modes have only one filter setting."),
            [this, axis](bool connected, const RadioCapabilities& caps) {
                return connected && !filterNamespace(caps).isEmpty()
                    && m_confirmed.value(QStringLiteral("editable")).toBool()
                    && axisFor(m_confirmed, axis).contains(QStringLiteral("index"));
            }, {}, false);
        connect(combo, &QComboBox::activated, this, [this, axis](int index) { submit(axis, index); });
    }
    m_status->setWordWrap(true);
    m_status->setObjectName(QStringLiteral("nativeFilterStatus"));
    layout->addWidget(m_status);
    auto* bankGroup = new QGroupBox(tr("Radio filter banks"), this);
    auto* bankForm = new QFormLayout(bankGroup);
    layout->addWidget(bankGroup);
    m_bankInfo = new QLabel(bankGroup);
    m_bankInfo->setObjectName(QStringLiteral("nativeFilterBankInfo"));
    m_bankInfo->setWordWrap(true);
    m_bankInfo->setTextFormat(Qt::PlainText);
    bankForm->addRow(m_bankInfo);
    const QStringList fieldLabels{tr("roofing"), tr("IF shape"), tr("AF type")};
    for (int row = 0; row < 10; ++row) {
        QComboBox* combo = new QComboBox(bankGroup);
        m_banks[row] = combo;
        combo->setObjectName(QStringLiteral("nativeFilterBank_") + bankRowId(row));
        const QString label = row == 0 ? tr("Active receive filter")
            : tr("Filter %1 %2").arg(QChar('A' + (row - 1) / 3), fieldLabels[(row - 1) % 3]);
        combo->setAccessibleName(label);
        bankForm->addRow(label + QLatin1Char(':'), combo);
        const auto available = [this, row](bool connected, const RadioCapabilities& caps) {
            return connected && !bankNamespace(caps).isEmpty()
                && bankRow(m_bankConfirmed, row).value(QStringLiteral("editable")).toBool();
        };
        m_availability->registerWidget(combo, tr("Requires current receive filter bank readback."), available, {}, false);
        connect(combo, &QComboBox::activated, this, [this, row](int index) { submitBank(row, index, false); });
        if (row > 0) {
            auto* reset = new QPushButton(tr("Reset"), bankGroup);
            reset->setAutoDefault(false);
            reset->setObjectName(QStringLiteral("nativeFilterBankReset_") + bankRowId(row));
            reset->setAccessibleName(tr("Reset %1 to radio initial setting").arg(label));
            m_bankResets[row - 1] = reset;
            bankForm->addRow(reset);
            m_availability->registerWidget(reset, tr("Requires current editable filter bank readback."),
                [this, row, available](bool connected, const RadioCapabilities& caps) {
                    return available(connected, caps) && bankRow(m_bankConfirmed, row).value(QStringLiteral("resettable")).toBool();
                }, {}, false);
            connect(reset, &QPushButton::clicked, this, [this, row] { submitBank(row, -1, true); });
        }
    }
    for (int bank = 0; bank < 3; ++bank) {
        m_bandwidths[bank] = new QLabel(bankGroup);
        m_bandwidths[bank]->setObjectName(QStringLiteral("nativeFilterBankBandwidth%1").arg(bank));
        m_bandwidths[bank]->setTextFormat(Qt::PlainText);
        bankForm->addRow(m_bandwidths[bank]);
    }
    m_bankStatus = new QLabel(bankGroup);
    m_bankStatus->setObjectName(QStringLiteral("nativeFilterBankStatus"));
    m_bankStatus->setTextFormat(Qt::PlainText);
    m_bankStatus->setWordWrap(true);
    bankForm->addRow(m_bankStatus);
    m_bankRefresh = new QPushButton(tr("Read settings"), bankGroup);
    m_bankRefresh->setObjectName(QStringLiteral("nativeFilterBankRefresh"));
    m_bankRefresh->setAutoDefault(false);
    bankForm->addRow(m_bankRefresh);
    m_availability->registerWidget(m_bankRefresh, tr("Requires connected native filter bank control."),
        [](bool connected, const RadioCapabilities& caps) { return connected && !bankNamespace(caps).isEmpty(); }, {}, false);
    connect(m_bankRefresh, &QPushButton::clicked, this, &NativeFilterSettingsWidget::queryBanks);
    auto* fmGroup = new QGroupBox(tr("FM Normal/Narrow"), this);
    auto* fmForm = new QFormLayout(fmGroup);
    auto* fmNotice = new QLabel(tr("Radio FM/FM-D Normal/Narrow, separate from audio cuts and filter banks. Apply changes the active receive setting."), fmGroup);
    fmNotice->setWordWrap(true); fmForm->addRow(fmNotice);
    for (int index = 0; index < 2; ++index) {
        m_fmObserved[index] = new QLabel(fmGroup);
        m_fmObserved[index]->setObjectName(index ? "nativeFmWidthRight" : "nativeFmWidthLeft");
        m_fmObserved[index]->setTextFormat(Qt::PlainText); fmForm->addRow(m_fmObserved[index]);
    }
    m_fmChoice = new GuardedComboBox(fmGroup); m_fmChoice->setObjectName("nativeFmWidthChoice");
    m_fmChoice->addItems({tr("Normal"), tr("Narrow")}); m_fmChoice->setAccessibleName(tr("Active receive FM width"));
    fmForm->addRow(tr("Active receive setting:"), m_fmChoice);
    m_fmRead = new QPushButton(tr("Read FM width"), fmGroup); m_fmRead->setObjectName("nativeFmWidthRead");
    m_fmApply = new QPushButton(tr("Apply FM width"), fmGroup); m_fmApply->setObjectName("nativeFmWidthApply");
    m_fmCancel = new QPushButton(tr("Cancel read"), fmGroup); m_fmCancel->setObjectName("nativeFmWidthCancel");
    for (QPushButton* button : {m_fmRead, m_fmApply, m_fmCancel}) { button->setAutoDefault(false); fmForm->addRow(button); }
    m_fmStatus = new QLabel(fmGroup); m_fmStatus->setObjectName("nativeFmWidthStatus"); m_fmStatus->setWordWrap(true); m_fmStatus->setTextFormat(Qt::PlainText); fmForm->addRow(m_fmStatus);
    connect(m_fmChoice, &QComboBox::activated, this, [this](int) { m_fmDirty = true; m_fmDraftRevision = m_fmConfirmed.value("revision").toString(); renderFm(); });
    connect(m_fmRead, &QPushButton::clicked, this, [this] { invokeFm("filter.fm.read"); });
    connect(m_fmApply, &QPushButton::clicked, this, [this] { invokeFm("filter.fm.set"); });
    connect(m_fmCancel, &QPushButton::clicked, this, [this] { invokeFm("filter.fm.cancel"); });
    layout->addWidget(fmGroup);
    renderFm();
    layout->addStretch();
    connect(&model, &RadioModel::connectionStateChanged, this, &NativeFilterSettingsWidget::bindBackend);
    connect(&model, &RadioModel::capabilitiesChanged, this, &NativeFilterSettingsWidget::bindBackend);
    bindBackend();
}

void NativeFilterSettingsWidget::bindBackend()
{
    const QString ns = m_model.isConnected() ? filterNamespace(m_model.backendCapabilities()) : QString{};
    const QString fmNs = m_model.isConnected() ? fmNamespace(m_model.backendCapabilities()) : QString{};
    const QString bankNs = m_model.isConnected() ? bankNamespace(m_model.backendCapabilities()) : QString{};
    IRadioBackend* backend = m_model.isConnected() ? m_model.backend() : nullptr;
    if (backend == m_backend && ns == m_namespace && bankNs == m_bankNamespace && fmNs == m_fmNamespace
        && m_receiveSession == m_model.receiveControlSession()) { const QPointer<NativeFilterSettingsWidget> alive(this); render(); if (alive) { renderFm(); } return; }
    const quint64 generation = ++m_bindingGeneration;
    m_receiveSession = m_model.receiveControlSession();
    const RadioControlSession session = m_model.controlSession();
    for (const QMetaObject::Connection& connection : m_connections) { disconnect(connection); }
    m_backend = backend;
    m_namespace = ns;
    m_bankNamespace = bankNs;
    m_fmNamespace = fmNs; m_fmConfirmed.clear(); m_fmRevision = m_fmRequest = 0; m_fmDirty = false; m_fmDraftRevision.clear();
    m_bankConfirmed.clear();
    m_bankQuery = m_bankEdit = 0;
    m_query = m_edit = 0;
    m_confirmed.clear();
    m_filterRevision = m_bankRevision = 0;
    m_status->clear();
    const QPointer<NativeFilterSettingsWidget> bindingGuard(this);
    render();
    if (!bindingGuard || generation != m_bindingGeneration) { return; }
    renderFm();
    if (!bindingGuard || generation != m_bindingGeneration) { return; }
    if (!backend || (ns.isEmpty() && bankNs.isEmpty())) { return; }
    const QPointer<IRadioBackend> source = backend;
    m_connections[0] = connect(backend, &IRadioBackend::extensionStatus, this,
        [this, source, ns, bankNs, fmNs, generation, session](const QString& space, const QString& kind, const QVariantMap& status) {
            if (generation != m_bindingGeneration || !m_model.controlSessionCurrent(session)
                || !source || source != m_backend || source != m_model.backend() || !m_model.isConnected()) { return; }
            if (!fmNs.isEmpty() && space == fmNs && kind == QLatin1String("fmWidth")) { acceptFmStatus(status); }
            else if (space == ns && kind == QLatin1String("filter")) { acceptStatus(status); }
            else if (!bankNs.isEmpty() && space == bankNs && kind == QLatin1String("filterBanks")) { acceptBankStatus(status); }
        });
    m_connections[1] = connect(backend, &IRadioBackend::extensionResult, this,
        [this, source, generation, session](quint64 id, const QVariant& result) {
            if (generation != m_bindingGeneration || !m_model.controlSessionCurrent(session)
                || !source || source != m_backend || source != m_model.backend() || !m_model.isConnected()) { return; }
            if (id && id == m_fmRequest) {
                m_fmRequest = 0;
                const QPointer<NativeFilterSettingsWidget> alive(this);
                if (result.typeId() == QMetaType::QVariantMap && result.toMap().contains("schemaVersion")) { acceptFmStatus(result.toMap()); }
                if (alive && generation == m_bindingGeneration) { renderFm(); } return;
            }
            if (id && id == m_bankQuery) {
                m_bankQuery = 0;
                if (m_bankGetSerial == m_bankSerial) { acceptBankStatus(result.toMap()); } else { renderBanks(); }
            }
            else if (id && id == m_bankEdit) { m_bankEdit = 0; renderBanks(); }
            else if (id && id == m_query) {
                m_query = 0;
                if (m_filterGetSerial == m_filterSerial) { acceptStatus(result.toMap()); } else { render(); }
            }
            else if (id && id == m_edit) { m_edit = 0; m_status->setText(tr("Request sent. Values shown are confirmed radio settings.")); }
        });
    m_connections[2] = connect(backend, &IRadioBackend::extensionError, this,
        [this, source, generation, session](quint64 id, const QString& error) {
            if (generation != m_bindingGeneration || !m_model.controlSessionCurrent(session)
                || !source || source != m_backend || source != m_model.backend() || !m_model.isConnected()) { return; }
            if (id && id == m_fmRequest) { m_fmRequest = 0; const QPointer<NativeFilterSettingsWidget> alive(this); renderFm(); if (alive && generation == m_bindingGeneration) { m_fmStatus->setText(error); } return; }
            if (id && (id == m_query || id == m_edit)) { m_status->setText(error); }
            else if (id && (id == m_bankQuery || id == m_bankEdit)) {
                if (id == m_bankQuery) { m_bankQuery = 0; }
                if (id == m_bankEdit) { m_bankEdit = 0; }
                m_bankStatus->setText(error);
            }
        });
    const QPointer<NativeFilterSettingsWidget> guard(this);
    if (!ns.isEmpty()) {
        m_query = nextFilterRequest();
        m_filterGetSerial = m_filterSerial;
        m_model.invokeBackendExtension(ns, QStringLiteral("filter.get"), m_query);
    }
    if (guard && generation == guard->m_bindingGeneration) { guard->queryBanks(); }
    if (guard && generation == guard->m_bindingGeneration && !fmNs.isEmpty()) { guard->invokeFm("filter.fm.get"); }
}

void NativeFilterSettingsWidget::acceptStatus(const QVariantMap& status)
{
    quint64 revision = 0;
    const bool ordered = revisionValid(status.value("statusRevision"), revision);
    if (ordered && revision < m_filterRevision) { return; }
    ++m_filterSerial;
    const QVariantList axes = status.value("axes").toList();
    bool valid = ordered && status.value("axes").typeId() == QMetaType::QVariantList && axes.size() <= 2
        && status.value("context").typeId() == QMetaType::QString && !status.value("context").toString().isEmpty()
        && status.value("context").toString().size() <= 128 && status.value("mode").typeId() == QMetaType::QString
        && status.value("mode").toString().size() <= 32 && status.value("editable").typeId() == QMetaType::Bool;
    QStringList ids;
    for (const QVariant& entry : axes) {
        const QVariantMap axis = entry.toMap();
        const QString id = axis.value(QStringLiteral("id")).toString();
        const QVariantList values = axis.value(QStringLiteral("valuesHz")).toList();
        valid = valid && entry.typeId() == QMetaType::QVariantMap
            && axis.value("id").typeId() == QMetaType::QString && axis.value("label").typeId() == QMetaType::QString
            && axis.value("valuesHz").typeId() == QMetaType::QVariantList
            && (id == QLatin1String("low") || id == QLatin1String("high"))
            && !ids.contains(id) && !values.isEmpty() && values.size() <= 256
            && !axis.value(QStringLiteral("label")).toString().isEmpty()
            && axis.value(QStringLiteral("label")).toString().size() <= 128;
        ids.append(id);
        for (const QVariant& value : values) {
            valid = valid && value.typeId() == QMetaType::Int && value.toInt() >= -1000000 && value.toInt() <= 1000000;
        }
        if (axis.contains("index")) {
            valid = valid && axis.value("index").typeId() == QMetaType::Int
                && axis.value("index").toInt() >= 0 && axis.value("index").toInt() < values.size();
        }
    }
    if (valid) { m_filterRevision = revision; }
    m_confirmed = valid ? status : QVariantMap{};
    m_status->setText(valid ? QString{} : tr("The radio supplied an invalid native filter layout."));
    render();
}

void NativeFilterSettingsWidget::render()
{
    const QPointer<NativeFilterSettingsWidget> alive(this);
    const quint64 generation = m_bindingGeneration;
    const quint64 serial = m_filterSerial;
    const auto current = [alive, generation, serial] {
        return alive && generation == alive->m_bindingGeneration && serial == alive->m_filterSerial;
    };
    const QString mode = m_confirmed.value(QStringLiteral("mode")).toString();
    m_mode->setText(tr("Current mode: %1").arg(mode.isEmpty() ? tr("Unknown") : mode));
    if (!current()) { return; }
    for (int axis = 0; axis < 2; ++axis) {
        const QPointer<QComboBox> editor(m_axes[axis]);
        const int previousIndex = editor->currentIndex();
        {
            QSignalBlocker blocker(editor);
            const auto usable = [&blocker, editor, current] {
                if (!editor) { blocker.dismiss(); return false; }
                return current();
            };
            const QVariantMap entry = axisFor(m_confirmed, axis);
            const QString label = entry.value(QStringLiteral("label"), tr("Filter control %1").arg(axis + 1)).toString();
            m_labels[axis]->setText(label + QLatin1Char(':'));
            if (!usable()) { return; }
            editor->setAccessibleName(label);
            if (!usable()) { return; }
            editor->clear();
            if (!usable()) { return; }
            const QVariantList values = entry.value(QStringLiteral("valuesHz")).toList();
            for (const QVariant& hz : values) {
                editor->addItem(tr("%1 Hz").arg(hz.toInt()));
                if (!usable()) { return; }
            }
            editor->setCurrentIndex(entry.contains(QStringLiteral("index"))
                ? entry.value(QStringLiteral("index")).toInt() : -1);
            if (!usable()) { return; }
        }
        if (previousIndex != editor->currentIndex()) {
            QAccessibleValueChangeEvent event(editor, editor->currentText());
            QAccessible::updateAccessibility(&event);
            if (!current() || !editor) { return; }
        }
    }
    m_availability->refreshEngaged();
    if (current()) { renderBanks(); }
}

void NativeFilterSettingsWidget::submit(int axis, int index)
{
    if (index < 0 || !m_axes[axis]->isEnabled() || !m_backend
        || m_backend != m_model.backend() || !m_model.isConnected() || m_namespace.isEmpty()) { return; }
    const QVariantMap argument{{QStringLiteral("axis"), axis == 0 ? QStringLiteral("low") : QStringLiteral("high")},
        {QStringLiteral("index"), index}, {QStringLiteral("context"), m_confirmed.value(QStringLiteral("context"))}};
    const QPointer<NativeFilterSettingsWidget> alive(this);
    const quint64 generation = m_bindingGeneration;
    const quint64 serial = m_filterSerial;
    render();
    if (!alive || generation != m_bindingGeneration || serial != m_filterSerial) { return; }
    m_edit = nextFilterRequest();
    m_model.invokeBackendExtension(m_namespace, QStringLiteral("filter.set"), m_edit, argument);
}
void NativeFilterSettingsWidget::queryBanks()
{
    if (!m_backend || m_backend != m_model.backend() || !m_model.isConnected()
        || m_bankNamespace.isEmpty() || m_bankQuery) { return; }
    m_bankQuery = nextFilterRequest();
    m_bankGetSerial = m_bankSerial;
    m_model.invokeBackendExtension(m_bankNamespace, QStringLiteral("filter.banks.get"), m_bankQuery);
}

void NativeFilterSettingsWidget::acceptBankStatus(const QVariantMap& status)
{
    quint64 revision = 0;
    const bool ordered = revisionValid(status.value("statusRevision"), revision);
    if (ordered && revision < m_bankRevision) { return; }
    ++m_bankSerial;
    bool valid = ordered && status.value("context").typeId() == QMetaType::QString
        && !status.value("context").toString().isEmpty() && status.value("context").toString().size() <= 128
        && status.value("mode").typeId() == QMetaType::QString && status.value("mode").toString().size() <= 32
        && status.value("controls").typeId() == QMetaType::QVariantList;
    const QVariantList controls = status.value("controls").toList();
    valid = valid && controls.size() == 10;
    QStringList ids;
    for (const QVariant& item : controls) {
        const QVariantMap row = item.toMap();
        const QString id = row.value("id").toString();
        int rowIndex = -1;
        for (int candidate = 0; candidate < 10; ++candidate) { if (bankRowId(candidate) == id) { rowIndex = candidate; break; } }
        valid = valid && item.typeId() == QMetaType::QVariantMap && rowIndex >= 0 && !ids.contains(id)
            && row.value("id").typeId() == QMetaType::QString
            && row.value("label").typeId() == QMetaType::QString && !row.value("label").toString().isEmpty()
            && row.value("label").toString().size() <= 128 && row.value("reason").typeId() == QMetaType::QString
            && row.value("reason").toString().size() <= 512 && row.value("editable").typeId() == QMetaType::Bool
            && row.value("resettable").typeId() == QMetaType::Bool && row.value("pending").typeId() == QMetaType::Bool
            && row.value("choices").typeId() == QMetaType::QVariantList;
        ids.append(id);
        const QVariantList choices = row.value("choices").toList();
        const int expectedChoices = rowIndex == 0 ? 3 : rowIndex > 0 && (rowIndex - 1) % 3 == 0 ? 6
            : rowIndex > 0 && (rowIndex - 1) % 3 == 1 ? 4 : 3;
        valid = valid && choices.size() == expectedChoices;
        QList<int> values;
        for (const QVariant& choice : choices) {
            const QVariantMap c = choice.toMap();
            const int value = c.value("value").toInt();
            valid = valid && choice.typeId() == QMetaType::QVariantMap && c.value("value").typeId() == QMetaType::Int
                && value >= 0 && value < expectedChoices && !values.contains(value)
                && c.value("label").typeId() == QMetaType::QString && !c.value("label").toString().isEmpty()
                && c.value("label").toString().size() <= 128 && c.value("editable").typeId() == QMetaType::Bool
                && c.value("reason").typeId() == QMetaType::QString && c.value("reason").toString().size() <= 512;
            values.append(value);
        }
        if (row.contains("value")) { valid = valid && row.value("value").typeId() == QMetaType::Int && values.contains(row.value("value").toInt()); }
        valid = valid && (!row.value("editable").toBool() || (row.contains("value") && !row.value("pending").toBool()));
        if (row.contains("actualBandwidthHz")) {
            valid = valid && id.endsWith(".roofing") && row.value("actualBandwidthHz").typeId() == QMetaType::Int
                && row.value("actualBandwidthHz").toInt() >= 270 && row.value("actualBandwidthHz").toInt() <= 15000;
        }
    }
    if (status.contains("bankCount")) { valid = valid && status.value("bankCount").typeId() == QMetaType::Int && (status.value("bankCount").toInt() == 2 || status.value("bankCount").toInt() == 3); }
    if (status.contains("option270Installed")) { valid = valid && status.value("option270Installed").typeId() == QMetaType::Bool; }
    if (valid) { m_bankRevision = revision; }
    m_bankConfirmed = valid ? status : QVariantMap{};
    m_bankStatus->setText(valid ? QString{} : tr("The radio supplied an invalid filter bank layout."));
    renderBanks();
}

void NativeFilterSettingsWidget::renderBanks()
{
    const QPointer<NativeFilterSettingsWidget> alive(this);
    const quint64 generation = m_bindingGeneration;
    const quint64 serial = m_bankSerial;
    const auto current = [alive, generation, serial] {
        return alive && generation == alive->m_bindingGeneration && serial == alive->m_bankSerial;
    };
    for (int row = 0; row < 10; ++row) {
        const QPointer<QComboBox> editor(m_banks[row]);
        const int previousIndex = editor->currentIndex();
        {
            QSignalBlocker blocker(editor);
            const auto usable = [&blocker, editor, current] {
                if (!editor) { blocker.dismiss(); return false; }
                return current();
            };
            const QVariantMap control = bankRow(m_bankConfirmed, row);
            editor->clear();
            if (!usable()) { return; }
            QVariantList choices = control.value("choices").toList();
            if (choices.isEmpty()) {
                const int field = row == 0 ? -1 : (row - 1) % 3;
                const QStringList labels = row == 0 ? QStringList{tr("A"), tr("B"), tr("C")}
                    : field == 0 ? QStringList{tr("Auto"), tr("270 Hz"), tr("500 Hz"), tr("2.7 kHz"), tr("6 kHz"), tr("15 kHz")}
                    : field == 1 ? QStringList{tr("Sharp"), tr("Medium"), tr("Soft"), tr("None (FM)")}
                                 : QStringList{tr("Narrow"), tr("Medium"), tr("Wide")};
                for (int value = 0; value < labels.size(); ++value) {
                    choices.append(QVariantMap{{"value", value}, {"label", labels[value]}, {"editable", false},
                        {"reason", tr("Waiting for current native filter bank readback.")}});
                }
            }
            for (const QVariant& item : choices) {
                const QVariantMap choice = item.toMap();
                editor->addItem(choice.value("label").toString(), choice.value("value"));
                if (!usable()) { return; }
                const int index = editor->count() - 1;
                editor->setItemData(index, choice.value("reason"), Qt::ToolTipRole);
                if (!usable()) { return; }
                if (auto* model = qobject_cast<QStandardItemModel*>(editor->model())) {
                    if (QStandardItem* entry = model->item(index)) {
                        entry->setEnabled(choice.value("editable").toBool());
                        if (!usable()) { return; }
                    }
                }
            }
            editor->setCurrentIndex(control.contains("value") ? editor->findData(control.value("value")) : -1);
            if (!usable()) { return; }
        }
        if (previousIndex != editor->currentIndex()) {
            QAccessibleValueChangeEvent event(editor, editor->currentText());
            QAccessible::updateAccessibility(&event);
            if (!current() || !editor) { return; }
        }
    }
    m_availability->refreshEngaged();
    if (!current()) { return; }
    const QString unavailable = tr("Requires a connected radio with native filter bank control and current readback.");
    for (int row = 0; row < 10; ++row) {
        const QVariantMap control = bankRow(m_bankConfirmed, row);
        const QString reason = control.isEmpty() ? unavailable : control.value("reason").toString();
        m_banks[row]->setToolTip(reason);
        if (!current()) { return; }
        m_banks[row]->setAccessibleDescription(reason);
        if (!current()) { return; }
        if (row > 0) {
            m_bankResets[row - 1]->setToolTip(reason);
            if (!current()) { return; }
            m_bankResets[row - 1]->setAccessibleDescription(reason);
            if (!current()) { return; }
        }
    }
    for (int bank = 0; bank < 3; ++bank) {
        const QVariantMap roofing = bankRow(m_bankConfirmed, bank * 3 + 1);
        m_bandwidths[bank]->setText(tr("Filter %1 actual roofing bandwidth: %2")
            .arg(QChar('A' + bank)).arg(roofing.contains("actualBandwidthHz")
                ? tr("%1 Hz").arg(roofing.value("actualBandwidthHz").toInt()) : tr("Unknown")));
        if (!current()) { return; }
    }
    m_bankInfo->setText(tr("Filter banks: %1 · Optional 270 Hz filter: %2")
        .arg(m_bankConfirmed.contains("bankCount") ? QString::number(m_bankConfirmed.value("bankCount").toInt()) : tr("Unknown"))
        .arg(m_bankConfirmed.contains("option270Installed")
            ? m_bankConfirmed.value("option270Installed").toBool() ? tr("Installed") : tr("Not installed") : tr("Unknown")));
}

void NativeFilterSettingsWidget::submitBank(int row, int choice, bool reset)
{
    if (row < 0 || row >= 10 || (!reset && (choice < 0 || choice >= m_banks[row]->count()))) { return; }
    const QVariantMap control = bankRow(m_bankConfirmed, row);
    const int value = choice >= 0 ? m_banks[row]->itemData(choice).toInt() : -1;
    const QPointer<NativeFilterSettingsWidget> alive(this);
    const quint64 generation = m_bindingGeneration;
    const quint64 serial = m_bankSerial;
    renderBanks();
    if (!alive || generation != m_bindingGeneration || serial != m_bankSerial) { return; }
    if (!m_backend || m_backend != m_model.backend() || !m_model.isConnected() || m_bankNamespace.isEmpty()
        || !m_banks[row]->isEnabled() || !control.value("editable").toBool()) { return; }
    if (reset) {
        if (row == 0 || !control.value("resettable").toBool()) { return; }
    } else {
        bool allowed = false;
        for (const QVariant& item : control.value("choices").toList()) {
            const QVariantMap c = item.toMap();
            if (c.value("value").toInt() == value && c.value("editable").toBool()) { allowed = true; }
        }
        if (!allowed) { return; }
    }
    QVariantMap argument{{"context", m_bankConfirmed.value("context")}, {"id", bankRowId(row)}};
    if (!reset) { argument.insert("value", value); }
    m_bankEdit = nextFilterRequest();
    m_model.invokeBackendExtension(m_bankNamespace,
        reset ? QStringLiteral("filter.banks.reset") : QStringLiteral("filter.banks.set"), m_bankEdit, argument);
}

void NativeFilterSettingsWidget::acceptFmStatus(const QVariantMap& status)
{
    const QSet<QString> fields{"schemaVersion", "statusRevision", "context", "revision", "connected", "editable", "reason", "displays", "refresh", "lastWrite"};
    QSet<QString> actual;
    for (auto it = status.cbegin(); it != status.cend(); ++it) { actual.insert(it.key()); }
    quint64 revision = 0; quint64 semantic = 0;
    bool valid = actual == fields && status.value("schemaVersion").typeId() == QMetaType::Int && status.value("schemaVersion").toInt() == 1
        && revisionValid(status.value("statusRevision"), revision) && revisionValid(status.value("revision"), semantic)
        && status.value("context").typeId() == QMetaType::QString && !status.value("context").toString().isEmpty() && status.value("context").toString().size() <= 128
        && status.value("connected").typeId() == QMetaType::Bool && status.value("editable").typeId() == QMetaType::Bool
        && status.value("reason").typeId() == QMetaType::QString && status.value("reason").toString().size() <= 2048
        && status.value("displays").typeId() == QMetaType::QVariantList && status.value("displays").toList().size() == 2
        && status.value("refresh").typeId() == QMetaType::QVariantMap && status.value("lastWrite").typeId() == QMetaType::QVariantMap;
    int index = 0; bool leftKnown = false;
    for (const QVariant& item : status.value("displays").toList()) {
        const QVariantMap row = item.toMap();
        QSet<QString> rowKeys;
        for (auto it = row.cbegin(); it != row.cend(); ++it) { rowKeys.insert(it.key()); }
        QSet<QString> expected{"display", "eligible", "known", "reason"};
        const bool known = row.value("known").toBool(); if (known) { expected.insert("value"); }
        valid = valid && item.typeId() == QMetaType::QVariantMap && rowKeys == expected
            && row.value("display").typeId() == QMetaType::Int && row.value("display").toInt() == index
            && row.value("known").typeId() == QMetaType::Bool && row.value("eligible").typeId() == QMetaType::Bool
            && row.value("reason").typeId() == QMetaType::QString && row.value("reason").toString().size() <= 2048
            && (!known || (status.value("connected").toBool() && row.value("eligible").toBool()
                && row.value("value").typeId() == QMetaType::Int && row.value("value").toInt() >= 0 && row.value("value").toInt() <= 1));
        if (index == 0) { leftKnown = known; }
        ++index;
    }
    const QVariantMap refresh = status.value("refresh").toMap();
    QSet<QString> refreshKeys;
    for (auto it = refresh.cbegin(); it != refresh.cend(); ++it) { refreshKeys.insert(it.key()); }
    quint64 token = 0;
    valid = valid && refreshKeys == QSet<QString>{"token", "busy", "pendingKey", "reason"}
        && refresh.value("token").typeId() == QMetaType::QString
        && (refresh.value("token").toString().isEmpty() || revisionValid(refresh.value("token"), token))
        && refresh.value("busy").typeId() == QMetaType::Bool && refresh.value("pendingKey").typeId() == QMetaType::QString
        && QStringList{"", "FR", "FT", "TS", "MV", "QR", "SF0", "SF1", "OM0", "OM1", "FW0", "FW1"}.contains(refresh.value("pendingKey").toString())
        && refresh.value("reason").typeId() == QMetaType::QString && refresh.value("reason").toString().size() <= 2048;
    const QVariantMap write = status.value("lastWrite").toMap();
    if (!write.isEmpty()) {
        valid = valid && write.size() == 2 && write.value("state").typeId() == QMetaType::QString
            && QStringList{"pending", "rejected", "indeterminate", "sent_unverified", "readback_consistent"}.contains(write.value("state").toString())
            && write.value("reason").typeId() == QMetaType::QString && write.value("reason").toString().size() <= 2048;
    }
    valid = valid && (!status.value("editable").toBool() || (status.value("connected").toBool() && leftKnown && !refresh.value("busy").toBool()));
    if (!valid) {
        m_fmConfirmed.clear();
        const QPointer<NativeFilterSettingsWidget> alive(this);
        m_fmStatus->setText(tr("Invalid native FM width status; Read again."));
        if (alive) { renderFm(); } return;
    }
    if (revision < m_fmRevision) { return; }
    m_fmRevision = revision;
    m_fmConfirmed = status;
    renderFm();
}

void NativeFilterSettingsWidget::renderFm()
{
    const QPointer<NativeFilterSettingsWidget> alive(this);
    const quint64 generation = m_bindingGeneration;
    const quint64 serial = ++m_fmRenderSerial;
    const auto current = [alive, generation, serial] { return alive && generation == alive->m_bindingGeneration && serial == alive->m_fmRenderSerial; };
    const QVariantList displays = m_fmConfirmed.value("displays").toList();
    for (int index = 0; index < 2; ++index) {
        const QVariantMap row = index < displays.size() ? displays[index].toMap() : QVariantMap{};
        const QString text = tr("%1 display: %2").arg(index ? tr("Right") : tr("Left"),
            row.value("known").toBool() ? (row.value("value").toInt() ? tr("Narrow") : tr("Normal")) : tr("Unknown"));
        if (m_fmObserved[index]->text() != text) {
            m_fmObserved[index]->setText(text);
            if (!current()) { return; }
            QAccessibleEvent event(m_fmObserved[index], QAccessible::NameChanged);
            QAccessible::updateAccessibility(&event);
            if (!current()) { return; }
        }
    }
    const bool busy = m_fmConfirmed.value("refresh").toMap().value("busy").toBool();
    const bool conflict = m_fmDirty && m_fmDraftRevision != m_fmConfirmed.value("revision").toString();
    const bool available = !m_fmNamespace.isEmpty() && m_backend && m_model.isConnected();
    m_fmRead->setEnabled(available && !busy && !m_fmRequest);
    if (!current()) { return; }
    m_fmCancel->setEnabled(available && busy && !m_fmRequest);
    if (!current()) { return; }
    m_fmChoice->setEnabled(available && m_fmConfirmed.value("editable").toBool() && !busy && !m_fmRequest);
    if (!current()) { return; }
    m_fmApply->setEnabled(m_fmChoice->isEnabled() && m_fmDirty && !conflict);
    if (!current()) { return; }
    if (!m_fmDirty) {
        QSignalBlocker block(m_fmChoice);
        m_fmChoice->setCurrentIndex(!displays.isEmpty() && displays[0].toMap().value("known").toBool() ? displays[0].toMap().value("value").toInt() : -1);
        if (!current()) { block.dismiss(); return; }
    }
    if (!current()) { return; }
    QString reason = m_fmConfirmed.isEmpty() ? tr("FM Normal/Narrow is unavailable or not yet read.") : m_fmConfirmed.value("reason").toString();
    if (conflict) { reason = tr("The native context changed. Read, then choose the intended value again."); }
    const QVariantMap write = m_fmConfirmed.value("lastWrite").toMap();
    if (!write.isEmpty()) { reason += tr(" Last Apply: %1. %2").arg(write.value("state").toString(), write.value("reason").toString()); }
    if (busy) { reason = m_fmConfirmed.value("refresh").toMap().value("reason").toString(); }
    if (reason.isEmpty()) { reason = tr("Choose Normal or Narrow, then Apply FM width."); }
    if (m_fmStatus->text() != reason) {
        m_fmStatus->setText(reason);
        if (!current()) { return; }
        QAccessibleEvent event(m_fmStatus, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
        if (!current()) { return; }
    }
    for (QWidget* control : QList<QWidget*>{m_fmChoice, m_fmApply, m_fmRead, m_fmCancel}) {
        control->setToolTip(reason);
        if (!current()) { return; }
        control->setAccessibleDescription(reason);
        if (!current()) { return; }
    }
}

void NativeFilterSettingsWidget::invokeFm(const QString& verb)
{
    if (!m_backend || m_backend != m_model.backend() || !m_model.isConnected() || m_fmNamespace.isEmpty() || m_fmRequest) { return; }
    QVariant argument;
    if (verb == "filter.fm.set") {
        if (!m_fmApply->isEnabled() || m_fmChoice->currentIndex() < 0) { return; }
        argument = QVariantMap{{"revision", m_fmDraftRevision}, {"value", m_fmChoice->currentIndex()}};
    } else if (verb == "filter.fm.cancel") {
        argument = QVariantMap{{"token", m_fmConfirmed.value("refresh").toMap().value("token")}};
    }
    m_fmRequest = nextFilterRequest();
    const quint64 request = m_fmRequest;
    const QString space = m_fmNamespace;
    const QPointer<NativeFilterSettingsWidget> alive(this);
    renderFm();
    if (alive && m_fmRequest == request && space == m_fmNamespace && m_backend == m_model.backend() && m_model.isConnected()) {
        m_model.invokeBackendExtension(space, verb, request, argument);
    }
}

} // namespace AetherSDR
