#include "ProfileSwitcherApplet.h"
#include "core/ThemeManager.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QSignalBlocker>

namespace AetherSDR {

static const char* kRowLabelStyle =
    "QLabel { color: #8090a0; font-size: 10px; font-weight: bold; }";

namespace {

// Rebuild a combo from a fresh profile list, restoring the active selection.
// Signals are blocked so the repopulation never looks like a user choice — the
// applet drives loads off currentTextChanged(), so without the blocker this
// repopulation would spuriously re-issue a profile load.
void rebuildCombo(QComboBox* combo, const QStringList& profiles, const QString& active)
{
    const QSignalBlocker block(combo);
    combo->clear();
    combo->addItems(profiles);
    const int idx = combo->findText(active);
    // Highlight the radio's authoritative active profile.  When the radio
    // reports an EMPTY active (findText returns -1) leave the combo with no
    // selection (index -1) so the placeholder shows — do NOT fall back to
    // index 0.  Global profiles legitimately report an empty "current" until
    // one is explicitly loaded (unlike TX/Mic, which always name a current), so
    // falling back to index 0 would falsely display the first profile as the
    // active one.  The real active is restored the moment the radio
    // names it via the next *Changed/state signal.  The set is signal-blocked,
    // so it never issues a load either way.
    combo->setCurrentIndex(idx);
    combo->setEnabled(!profiles.isEmpty());
}

// Update only the highlighted selection (no list rebuild) — used for the
// high-frequency TX/Mic state signals.  Guarded so it is a no-op unless the
// active profile actually moved.
void syncCombo(QComboBox* combo, const QString& active)
{
    if (active.isEmpty() || combo->currentText() == active)
        return;
    const int idx = combo->findText(active);
    if (idx >= 0) {
        const QSignalBlocker block(combo);
        combo->setCurrentIndex(idx);
    }
}

} // namespace

ProfileSwitcherApplet::ProfileSwitcherApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/prof"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(4, 3, 4, 3);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(3);
    grid->setColumnStretch(1, 1);

    auto makeRow = [&](int row, const QString& text, const QString& accessible,
                       const char* objName) -> QComboBox* {
        auto* lbl = new QLabel(text, this);
        lbl->setStyleSheet(QString::fromLatin1(kRowLabelStyle));
        auto* combo = new QComboBox(this);
        combo->setObjectName(QString::fromLatin1(objName));
        combo->setAccessibleName(accessible);
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(8);
        // Shown when the radio reports no active profile (currentIndex == -1);
        // e.g. a global profile that has never been loaded this session.
        combo->setPlaceholderText(tr("— none —"));
        combo->setEnabled(false);  // until a model + profiles arrive
        grid->addWidget(lbl, row, 0);
        grid->addWidget(combo, row, 1);
        return combo;
    };

    m_globalCombo = makeRow(0, tr("Global"), tr("Global profile"), "profGlobalCombo");
    m_txCombo     = makeRow(1, tr("TX"),     tr("TX profile"),     "profTxCombo");
    m_micCombo    = makeRow(2, tr("Mic"),    tr("Mic profile"),    "profMicCombo");

    setAccessibleName(tr("Profile switcher"));
}

void ProfileSwitcherApplet::setRadioModel(RadioModel* model)
{
    m_model = model;
    if (!m_model)
        return;

    auto& tx = m_model->transmitModel();

    // List + active for Global both arrive via globalProfilesChanged().
    connect(m_model, &RadioModel::globalProfilesChanged,
            this, &ProfileSwitcherApplet::refreshGlobal);

    // TX: full list on profileListChanged(); active selection on stateChanged().
    connect(&tx, &TransmitModel::profileListChanged,
            this, &ProfileSwitcherApplet::refreshTx);
    connect(&tx, &TransmitModel::stateChanged,
            this, &ProfileSwitcherApplet::syncTxCurrent);

    // Mic: full list on micProfileListChanged(); active on micStateChanged().
    connect(&tx, &TransmitModel::micProfileListChanged,
            this, &ProfileSwitcherApplet::refreshMic);
    connect(&tx, &TransmitModel::micStateChanged,
            this, &ProfileSwitcherApplet::syncMicCurrent);

    // Apply live whenever the selection changes.  currentTextChanged() fires
    // for both user picks and programmatic setCurrentText() (e.g. the agent
    // automation bridge's invoke verb), which is exactly what we want — an
    // automated "select" should load too.  Every *internal* mutation
    // (rebuildCombo / syncCombo) is wrapped in a QSignalBlocker, so the only
    // changes that reach these slots are genuine selections — there is no
    // select→load→refresh→load feedback loop.
    //
    // All three rows delegate to the model load helpers (loadGlobalProfile /
    // loadProfile / loadMicProfile) rather than hand-rolling command strings,
    // so the radio command verbs live in exactly one place (the models) — same
    // source of truth ProfileManagerDialog now relies on.
    connect(m_globalCombo, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        if (!name.isEmpty())
            m_model->loadGlobalProfile(name);
    });
    connect(m_txCombo, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        if (!name.isEmpty())
            m_model->transmitModel().loadProfile(name);
    });
    connect(m_micCombo, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        if (!name.isEmpty())
            m_model->transmitModel().loadMicProfile(name);
    });

    // Seed from whatever the model already holds (handles late wiring after a
    // connection is already up).
    refreshGlobal();
    refreshTx();
    refreshMic();
}

void ProfileSwitcherApplet::refreshGlobal()
{
    if (!m_model)
        return;
    rebuildCombo(m_globalCombo, m_model->globalProfiles(), m_model->activeGlobalProfile());
}

void ProfileSwitcherApplet::refreshTx()
{
    if (!m_model)
        return;
    const auto& tx = m_model->transmitModel();
    rebuildCombo(m_txCombo, tx.profileList(), tx.activeProfile());
}

void ProfileSwitcherApplet::refreshMic()
{
    if (!m_model)
        return;
    const auto& tx = m_model->transmitModel();
    rebuildCombo(m_micCombo, tx.micProfileList(), tx.activeMicProfile());
}

void ProfileSwitcherApplet::syncTxCurrent()
{
    if (!m_model)
        return;
    syncCombo(m_txCombo, m_model->transmitModel().activeProfile());
}

void ProfileSwitcherApplet::syncMicCurrent()
{
    if (!m_model)
        return;
    syncCombo(m_micCombo, m_model->transmitModel().activeMicProfile());
}

} // namespace AetherSDR
