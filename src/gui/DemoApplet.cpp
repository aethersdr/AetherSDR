#include "DemoApplet.h"

#include "core/backends/sim/NoiseMixer.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {
// The channels shown, in display order, with an optional knob {name, label,
// min, max, default}. Kept in sync with NoiseMixer's channels. defaultOn +
// defaultLevel define the STARTUP scene — the applet is the single source of
// truth for it and pushes this state to the engine on show, so controls and
// audio can never drift. (Pink floor + a 1200 Hz birdie: shows off NR + notch.)
struct RowSpec {
    const char* channel;
    const char* label;
    const char* knob;     // "" = no knob
    const char* knobLabel;
    int knobMin, knobMax, knobDefault;
    bool   defaultOn;
    int    defaultLevel;  // dB
};
const RowSpec kRows[] = {
    {"cw",         "CW tone",        "hz",   "Hz",  300, 1200, 700,  false, -16},
    {"voice",      "Voice (speech)", "",     "",    0, 0, 0,          false, -10},
    {"white",      "White / AWGN",   "",     "",    0, 0, 0,         false, -26},
    {"pink",       "Pink / hiss",    "",     "",    0, 0, 0,         true,  -22},
    {"qrn",        "QRN crackle",    "rate", "i/s", 1, 60, 12,       false, -18},
    {"powerline",  "Power-line",     "freq", "Hz",  50, 60, 60,      false, -22},
    {"crashes",    "Static crash",   "rate", "c/s", 1, 30, 4,       false, -16},  // rate stored val/10
    {"birdie",     "Birdie carrier", "hz",   "Hz",  300, 3000, 1200, true, -18},
    {"hash",       "SMPS hash",      "prf",  "Hz",  30, 400, 120,    false, -24},
    {"woodpecker", "Woodpecker",     "prf",  "pps", 2, 50, 10,       false, -22},
};
}  // namespace

DemoApplet::DemoApplet(QWidget* parent) : QWidget(parent)
{
    buildUI();
}

void DemoApplet::buildUI()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto* title = new QLabel(QStringLiteral("Demo Noise"), this);
    title->setStyleSheet(QStringLiteral("color:#5cf;font-weight:bold;"));
    root->addWidget(title);

    // ── scene presets ──────────────────────────────────────────────────────
    auto* presetRow = new QHBoxLayout();
    presetRow->setSpacing(4);
    for (const QString& p : NoiseMixer::allPresetNames()) {
        auto* b = new QPushButton(p, this);
        b->setStyleSheet(QStringLiteral(
            "QPushButton{background:#234;color:#bdf;border:1px solid #456;"
            "border-radius:4px;padding:3px 7px;font-size:11px;}"
            "QPushButton:hover{background:#2a4a6a;}"));
        connect(b, &QPushButton::clicked, this, [this, p]() {
            emit demoPresetRequested(p);
            applyPresetToControls(p);
        });
        presetRow->addWidget(b);
    }
    presetRow->addStretch();
    root->addLayout(presetRow);

    // ── per-channel rows: toggle | level slider | optional knob ────────────
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);
    int r = 0;
    for (const RowSpec& spec : kRows) {
        ChannelRow row;
        row.name = QString::fromLatin1(spec.channel);
        row.knob = QString::fromLatin1(spec.knob);

        row.toggle = new QPushButton(QString::fromLatin1(spec.label), this);
        row.toggle->setCheckable(true);
        row.toggle->setChecked(spec.defaultOn);
        row.toggle->setStyleSheet(QStringLiteral(
            "QPushButton{text-align:left;padding:2px 6px;border:1px solid #345;"
            "border-radius:3px;background:#1a2332;color:#bcd;font-size:12px;}"
            "QPushButton:checked{background:#1a5;color:#fff;}"));
        connect(row.toggle, &QPushButton::toggled, this,
                [this, name = row.name](bool on) { emit demoNoiseToggled(name, on); });
        grid->addWidget(row.toggle, r, 0);

        row.level = new QSlider(Qt::Horizontal, this);
        row.level->setRange(-60, 0);
        row.level->setValue(spec.defaultLevel);
        row.levelLabel = new QLabel(QString::number(spec.defaultLevel), this);
        row.levelLabel->setStyleSheet(QStringLiteral("color:#5cf;font-family:monospace;"));
        row.levelLabel->setMinimumWidth(28);
        connect(row.level, &QSlider::valueChanged, this,
                [this, name = row.name, lbl = row.levelLabel](int v) {
                    lbl->setText(QString::number(v));
                    emit demoNoiseLevelChanged(name, v);
                });
        grid->addWidget(row.level, r, 1);
        grid->addWidget(row.levelLabel, r, 2);

        if (!row.knob.isEmpty()) {
            row.knobSlider = new QSlider(Qt::Horizontal, this);
            row.knobSlider->setRange(spec.knobMin, spec.knobMax);
            row.knobSlider->setValue(spec.knobDefault);
            row.knobSlider->setMaximumWidth(64);
            row.knobLabel = new QLabel(QString::number(spec.knobDefault), this);
            row.knobLabel->setStyleSheet(QStringLiteral("color:#8cf;font-family:monospace;"));
            row.knobLabel->setMinimumWidth(30);
            connect(row.knobSlider, &QSlider::valueChanged, this,
                    [this, name = row.name, knob = row.knob, lbl = row.knobLabel](int v) {
                        lbl->setText(QString::number(v));
                        // crashes' rate is 0.1..3 c/s → slider 1..30 maps /10.
                        const double val = (knob == QLatin1String("rate") &&
                                            name == QLatin1String("crashes"))
                                               ? v / 10.0 : double(v);
                        emit demoNoiseKnobChanged(name, knob, val);
                    });
            grid->addWidget(row.knobSlider, r, 3);
            grid->addWidget(row.knobLabel, r, 4);
        }
        m_rows.push_back(row);
        ++r;
    }
    root->addLayout(grid);

    // ── fault injection (RFC #4288 #4) ──────────────────────────────────────
    // Manual/live trigger for the same faults the `sim` automation verb drives.
    // Each button emits demoFaultRequested(<fault>); MainWindow forwards it to
    // backend->invokeExtension("sim", …). Lets the demo show AE degrading safely
    // (frozen scope, dropped slice, forced disconnect) on a click.
    auto* faultTitle = new QLabel(QStringLiteral("Fault Injection"), this);
    faultTitle->setStyleSheet(QStringLiteral("color:#f96;font-weight:bold;"));
    root->addWidget(faultTitle);

    // {label, fault-verb, optional arg}. Distinct amber styling from the noise
    // controls so faults read as "danger", not routine.
    struct FaultBtn { const char* label; const char* fault; const char* arg; };
    static const FaultBtn kFaultBtns[] = {
        {"High SWR",    "swr",        "3.5"},
        {"Drop Slice",  "dropslice",  ""},
        {"Stall Scope", "stallscope", ""},
        {"Disconnect",  "disconnect", ""},
        {"Malformed",   "malformed",  ""},
        {"Clear",       "clear",      ""},
    };
    auto* faultRow = new QHBoxLayout();
    faultRow->setSpacing(4);
    for (const FaultBtn& fb : kFaultBtns) {
        auto* b = new QPushButton(QString::fromLatin1(fb.label), this);
        const bool isClear = QLatin1String(fb.fault) == QLatin1String("clear");
        b->setStyleSheet(isClear
            ? QStringLiteral("QPushButton{background:#243;color:#bfb;border:1px solid #465;"
                             "border-radius:4px;padding:3px 7px;font-size:11px;}"
                             "QPushButton:hover{background:#2a5a2a;}")
            : QStringLiteral("QPushButton{background:#432;color:#fdb;border:1px solid #654;"
                             "border-radius:4px;padding:3px 7px;font-size:11px;}"
                             "QPushButton:hover{background:#6a4a2a;}"));
        const QString fault = QString::fromLatin1(fb.fault);
        const QString arg = QString::fromLatin1(fb.arg);
        connect(b, &QPushButton::clicked, this, [this, fault, arg]() {
            // swr carries its ratio as "swr 3.5"; the rest are bare verbs. The
            // receiver splits on the first space.
            emit demoFaultRequested(arg.isEmpty() ? fault
                                                  : fault + QLatin1Char(' ') + arg);
        });
        faultRow->addWidget(b);
    }
    faultRow->addStretch();
    root->addLayout(faultRow);

    root->addStretch();
}

void DemoApplet::pushSceneToEngine()
{
    // Send every row's current state so the engine mixer == the controls. Levels
    // and knobs first, then the enable toggle last (so a channel turns on already
    // at its shown level, no audible level jump).
    for (const ChannelRow& row : m_rows) {
        emit demoNoiseLevelChanged(row.name, row.level->value());
        if (row.knobSlider) {
            const int v = row.knobSlider->value();
            const double val = (row.knob == QLatin1String("rate") &&
                                row.name == QLatin1String("crashes"))
                                   ? v / 10.0 : double(v);
            emit demoNoiseKnobChanged(row.name, row.knob, val);
        }
        emit demoNoiseToggled(row.name, row.toggle->isChecked());
    }
}

void DemoApplet::applyPresetToControls(const QString& presetName)
{
    // A preset is absolute: everything off, then the named channels on. We can't
    // read the mixer's post-preset levels back cheaply, so reflect enabled-state
    // by clearing all toggles then re-checking those the preset turns on. The
    // engine already applied the authoritative levels; the toggles just track it.
    NoiseMixer probe;
    probe.loadPreset(presetName);
    for (ChannelRow& row : m_rows) {
        bool ok = false;
        const auto c = NoiseMixer::fromName(row.name, &ok);
        const bool on = ok && probe.channel(c).enabled;
        // block signals so reflecting state doesn't re-emit intents to the engine
        // (the preset request already set the engine authoritatively).
        const QSignalBlocker bt(row.toggle);
        row.toggle->setChecked(on);
        if (on) {
            const QSignalBlocker bl(row.level);
            const int lvl = int(probe.channel(c).levelDb);
            row.level->setValue(lvl);
            row.levelLabel->setText(QString::number(lvl));
        }
    }
}

}  // namespace AetherSDR
