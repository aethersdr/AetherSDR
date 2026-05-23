#include "SpotSettingsDialog.h"
#include "GuardedSlider.h"
#include "models/RadioModel.h"
#include "core/AppSettings.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QColorDialog>

namespace MasterSDR {

SpotSettingsDialog::SpotSettingsDialog(RadioModel* model, QWidget* parent)
    : QDialog(parent), m_model(model)
{
    setWindowTitle("Spot Settings");
    setMinimumSize(380, 520);
    resize(380, 520);

    // Load persisted settings
    auto& s = AppSettings::instance();
    m_spotsEnabled       = s.value("IsSpotsEnabled", "True").toString() == "True";
    m_memoriesEnabled    = s.value("IsMemorySpotsEnabled", "False").toString() == "True";
    m_overrideColors     = s.value("IsSpotsOverrideColorsEnabled", "False").toString() == "True";
    m_overrideBg         = s.value("IsSpotsOverrideBackgroundColorsEnabled", "True").toString() == "True";
    m_overrideBgAutoMode = s.value("IsSpotsOverrideToAutoBackgroundColorEnabled", "True").toString() == "True";
    int levels   = s.value("SpotsMaxLevel", 3).toInt();
    int position = s.value("SpotsStartingHeightPercentage", 50).toInt();
    int fontSize = s.value("SpotFontSize", 16).toInt();
    m_spotColor  = QColor(s.value("SpotsOverrideColor", "#FFFF00").toString());
    m_bgColor    = QColor(s.value("SpotsOverrideBgColor", "#000000").toString());
    m_bgOpacity  = s.value("SpotsBackgroundOpacity", 48).toInt();
    m_spotShowLines = s.value("IsSpotsLinesEnabled", "True").toString() == "True";
    // Migrate from old minutes key to new seconds key
    int lifetimeSec = s.value("DxClusterSpotLifetimeSec", 0).toInt();
    if (lifetimeSec <= 0)
        lifetimeSec = s.value("DxClusterSpotLifetime", 30).toInt() * 60;

    auto* root = new QVBoxLayout(this);

    auto* title = new QLabel("Spot Settings");
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; color: #c8d8e8; }");
    root->addWidget(title);
    root->addSpacing(8);

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    int row = 0;

    auto save = [this](const QString& key, const QVariant& val) {
        auto& s = AppSettings::instance();
        s.setValue(key, val);
        s.save();
        emit settingsChanged();
    };

    // ── Spots: Enabled/Disabled ─────────────────────────────────────────
    grid->addWidget(new QLabel("Spots:"), row, 0);
    m_spotsToggle = new QPushButton(m_spotsEnabled ? "Enabled" : "Disabled");
    m_spotsToggle->setCheckable(true);
    m_spotsToggle->setChecked(m_spotsEnabled);
    m_spotsToggle->setFixedWidth(80);
    m_spotsToggle->setStyleSheet(
        "QPushButton { background: #206030; color: white; border: 1px solid #305040; padding: 3px; }"
        "QPushButton:!checked { background: #603020; }");
    connect(m_spotsToggle, &QPushButton::toggled, this, [this, save](bool on) {
        m_spotsEnabled = on;
        m_spotsToggle->setText(on ? "Enabled" : "Disabled");
        save("IsSpotsEnabled", on ? "True" : "False");
    });
    grid->addWidget(m_spotsToggle, row++, 1, Qt::AlignLeft);

    // ── Memories: Enabled/Disabled ──────────────────────────────────────
    grid->addWidget(new QLabel("Memories:"), row, 0);
    m_memoriesToggle = new QPushButton(m_memoriesEnabled ? "Enabled" : "Disabled");
    m_memoriesToggle->setCheckable(true);
    m_memoriesToggle->setChecked(m_memoriesEnabled);
    m_memoriesToggle->setFixedWidth(80);
    m_memoriesToggle->setToolTip(
        "Show radio memory channels as a spot-like feed on the panadapter.");
    m_memoriesToggle->setStyleSheet(
        "QPushButton { background: #206030; color: white; border: 1px solid #305040; padding: 3px; }"
        "QPushButton:!checked { background: #603020; }");
    connect(m_memoriesToggle, &QPushButton::toggled, this, [this, save](bool on) {
        m_memoriesEnabled = on;
        m_memoriesToggle->setText(on ? "Enabled" : "Disabled");
        save("IsMemorySpotsEnabled", on ? "True" : "False");
    });
    grid->addWidget(m_memoriesToggle, row++, 1, Qt::AlignLeft);

    // ── Levels slider ───────────────────────────────────────────────────
    grid->addWidget(new QLabel("Levels:"), row, 0);
    auto* levelsRow = new QHBoxLayout;
    m_levelsSlider = new GuardedSlider(Qt::Horizontal);
    m_levelsSlider->setRange(1, 10);
    m_levelsSlider->setValue(levels);
    m_levelsValue = new QLabel(QString::number(levels));
    m_levelsValue->setFixedWidth(24);
    m_levelsValue->setAlignment(Qt::AlignRight);
    levelsRow->addWidget(m_levelsSlider);
    levelsRow->addWidget(m_levelsValue);
    connect(m_levelsSlider, &QSlider::valueChanged, this, [this, save](int v) {
        m_levelsValue->setText(QString::number(v));
        save("SpotsMaxLevel", QString::number(v));
    });
    grid->addLayout(levelsRow, row++, 1);

    // ── Position slider ─────────────────────────────────────────────────
    grid->addWidget(new QLabel("Position:"), row, 0);
    auto* posRow = new QHBoxLayout;
    m_positionSlider = new GuardedSlider(Qt::Horizontal);
    m_positionSlider->setRange(0, 100);
    m_positionSlider->setValue(position);
    m_positionValue = new QLabel(QString::number(position));
    m_positionValue->setFixedWidth(24);
    m_positionValue->setAlignment(Qt::AlignRight);
    posRow->addWidget(m_positionSlider);
    posRow->addWidget(m_positionValue);
    connect(m_positionSlider, &QSlider::valueChanged, this, [this, save](int v) {
        m_positionValue->setText(QString::number(v));
        save("SpotsStartingHeightPercentage", QString::number(v));
    });
    grid->addLayout(posRow, row++, 1);

    // ── Font Size slider ────────────────────────────────────────────────
    grid->addWidget(new QLabel("Font Size:"), row, 0);
    auto* fontRow = new QHBoxLayout;
    m_fontSizeSlider = new GuardedSlider(Qt::Horizontal);
    m_fontSizeSlider->setRange(8, 32);
    m_fontSizeSlider->setValue(fontSize);
    m_fontSizeValue = new QLabel(QString::number(fontSize));
    m_fontSizeValue->setFixedWidth(24);
    m_fontSizeValue->setAlignment(Qt::AlignRight);
    fontRow->addWidget(m_fontSizeSlider);
    fontRow->addWidget(m_fontSizeValue);
    connect(m_fontSizeSlider, &QSlider::valueChanged, this, [this, save](int v) {
        m_fontSizeValue->setText(QString::number(v));
        save("SpotFontSize", QString::number(v));
    });
    grid->addLayout(fontRow, row++, 1);

    // ── Spot Lifetime slider (non-linear: seconds → minutes → hours) ──
    grid->addWidget(new QLabel("Spot Lifetime:"), row, 0);
    auto* lifeRow = new QHBoxLayout;

    static QVector<int> lifeSteps;
    if (lifeSteps.isEmpty()) {
        for (int s = 10; s <= 55; s += 5)  lifeSteps.append(s);        // 10–55 sec
        for (int m = 5;  m <= 55; m += 5)  lifeSteps.append(m * 60);   // 5–55 min
        for (int h = 1;  h <= 24; h++)      lifeSteps.append(h * 3600); // 1–24 hr
    }
    auto formatLifetime = [](int secs) -> QString {
        if (secs < 60)   return QString("%1 sec").arg(secs);
        if (secs < 3600) return QString("%1 min%2").arg(secs / 60).arg(secs / 60 == 1 ? "" : "s");
        int hrs = secs / 3600;
        if (hrs == 24) return QStringLiteral("1 day");
        return QString("%1 hr%2").arg(hrs).arg(hrs == 1 ? "" : "s");
    };
    // Find closest step index for the saved value
    int lifeIdx = 0;
    for (int i = 0; i < lifeSteps.size(); ++i)
        if (std::abs(lifeSteps[i] - lifetimeSec) < std::abs(lifeSteps[lifeIdx] - lifetimeSec))
            lifeIdx = i;

    auto* lifetimeSlider = new GuardedSlider(Qt::Horizontal);
    lifetimeSlider->setRange(0, lifeSteps.size() - 1);
    lifetimeSlider->setValue(lifeIdx);
    auto* lifetimeValue = new QLabel(formatLifetime(lifeSteps[lifeIdx]));
    lifetimeValue->setFixedWidth(90);
    lifetimeValue->setAlignment(Qt::AlignRight);
    lifeRow->addWidget(lifetimeSlider);
    lifeRow->addWidget(lifetimeValue);
    connect(lifetimeSlider, &QSlider::valueChanged, this, [save, lifetimeValue, formatLifetime](int idx) {
        int secs = lifeSteps.value(idx, 1800);
        lifetimeValue->setText(formatLifetime(secs));
        save("DxClusterSpotLifetimeSec", QString::number(secs));
    });
    grid->addLayout(lifeRow, row++, 1);

    // ── Override Colors + color picker ──────────────────────────────────
    grid->addWidget(new QLabel("Override Colors:"), row, 0);
    auto* colorRow = new QHBoxLayout;
    m_overrideColorsToggle = new QPushButton(m_overrideColors ? "Enabled" : "Disabled");
    m_overrideColorsToggle->setCheckable(true);
    m_overrideColorsToggle->setChecked(m_overrideColors);
    m_overrideColorsToggle->setFixedWidth(80);
    m_overrideColorsToggle->setStyleSheet(
        "QPushButton { background: #206030; color: white; border: 1px solid #305040; padding: 3px; }"
        "QPushButton:!checked { background: #603020; }");
    connect(m_overrideColorsToggle, &QPushButton::toggled, this, [this, save](bool on) {
        m_overrideColors = on;
        m_overrideColorsToggle->setText(on ? "Enabled" : "Disabled");
        save("IsSpotsOverrideColorsEnabled", on ? "True" : "False");
    });
    colorRow->addWidget(m_overrideColorsToggle);

    m_colorPickerBtn = new QPushButton;
    m_colorPickerBtn->setFixedSize(24, 24);
    updateColorSwatch(m_colorPickerBtn, m_spotColor);
    connect(m_colorPickerBtn, &QPushButton::clicked, this, [this, save]() {
        QColor c = QColorDialog::getColor(m_spotColor, this, "Spot Text Color");
        if (c.isValid()) {
            m_spotColor = c;
            updateColorSwatch(m_colorPickerBtn, c);
            save("SpotsOverrideColor", c.name());
        }
    });
    colorRow->addWidget(m_colorPickerBtn);
    colorRow->addStretch();
    grid->addLayout(colorRow, row++, 1);

    // ── Override Background + Auto + color picker ───────────────────────
    grid->addWidget(new QLabel("Override Background:"), row, 0);
    auto* bgRow = new QHBoxLayout;
    m_overrideBgEnabled = new QPushButton("Enabled");
    m_overrideBgEnabled->setCheckable(true);
    m_overrideBgEnabled->setChecked(m_overrideBg);
    m_overrideBgEnabled->setFixedWidth(70);
    m_overrideBgAuto = new QPushButton("Auto");
    m_overrideBgAuto->setCheckable(true);
    m_overrideBgAuto->setChecked(m_overrideBgAutoMode);
    m_overrideBgAuto->setFixedWidth(50);
    QString bgStyle =
        "QPushButton { background: #206030; color: white; border: 1px solid #305040; padding: 3px; }"
        "QPushButton:!checked { background: #603020; }";
    m_overrideBgEnabled->setStyleSheet(bgStyle);
    m_overrideBgAuto->setStyleSheet(bgStyle);
    connect(m_overrideBgEnabled, &QPushButton::toggled, this, [this, save](bool on) {
        m_overrideBg = on;
        save("IsSpotsOverrideBackgroundColorsEnabled", on ? "True" : "False");
    });
    connect(m_overrideBgAuto, &QPushButton::toggled, this, [this, save](bool on) {
        m_overrideBgAutoMode = on;
        save("IsSpotsOverrideToAutoBackgroundColorEnabled", on ? "True" : "False");
    });
    bgRow->addWidget(m_overrideBgEnabled);
    bgRow->addWidget(m_overrideBgAuto);

    m_bgColorPickerBtn = new QPushButton;
    m_bgColorPickerBtn->setFixedSize(24, 24);
    updateColorSwatch(m_bgColorPickerBtn, m_bgColor);
    connect(m_bgColorPickerBtn, &QPushButton::clicked, this, [this, save]() {
        QColor c = QColorDialog::getColor(m_bgColor, this, "Spot Background Color");
        if (c.isValid()) {
            m_bgColor = c;
            updateColorSwatch(m_bgColorPickerBtn, c);
            save("SpotsOverrideBgColor", c.name());
        }
    });
    bgRow->addWidget(m_bgColorPickerBtn);
    bgRow->addStretch();
    grid->addLayout(bgRow, row++, 1);

    // ── Background Opacity slider ───────────────────────────────────────
    grid->addWidget(new QLabel("Background Opacity:"), row, 0);
    auto* opacRow = new QHBoxLayout;
    m_bgOpacitySlider = new GuardedSlider(Qt::Horizontal);
    m_bgOpacitySlider->setRange(0, 100);
    m_bgOpacitySlider->setValue(m_bgOpacity);
    m_bgOpacityValue = new QLabel(QString::number(m_bgOpacity));
    m_bgOpacityValue->setFixedWidth(24);
    m_bgOpacityValue->setAlignment(Qt::AlignRight);
    opacRow->addWidget(m_bgOpacitySlider);
    opacRow->addWidget(m_bgOpacityValue);
    connect(m_bgOpacitySlider, &QSlider::valueChanged, this, [this, save](int v) {
        m_bgOpacity = v;
        m_bgOpacityValue->setText(QString::number(v));
        save("SpotsBackgroundOpacity", QString::number(v));
    });
    grid->addLayout(opacRow, row++, 1);

    // ── Spot Lines ──────────────────────────────────────────────────────
    grid->addWidget(new QLabel("Spot Lines:"), row, 0);
    m_spotLinesToggle = new QPushButton(m_spotShowLines ? "Enabled" : "Disabled");
    m_spotLinesToggle->setCheckable(true);
    m_spotLinesToggle->setChecked(m_spotShowLines);
    m_spotLinesToggle->setFixedWidth(80);
    m_spotLinesToggle->setToolTip("Show vertical lines from the spectrum up to each spot label.\nDisable during contests to reduce clutter.");
    m_spotLinesToggle->setStyleSheet(
        "QPushButton { background: #206030; color: white; border: 1px solid #305040; padding: 3px; }"
        "QPushButton:!checked { background: #603020; }");
    connect(m_spotLinesToggle, &QPushButton::toggled, this, [this, save](bool on) {
        m_spotShowLines = on;
        m_spotLinesToggle->setText(on ? "Enabled" : "Disabled");
        save("IsSpotsLinesEnabled", on ? "True" : "False");
    });
    grid->addWidget(m_spotLinesToggle, row++, 1, Qt::AlignLeft);

    // ── Total Spots ─────────────────────────────────────────────────────
    grid->addWidget(new QLabel("Total Spots:"), row, 0);
    m_totalSpotsLabel = new QLabel("0");
    m_totalSpotsLabel->setStyleSheet("QLabel { color: #c8d8e8; font-weight: bold; }");
    grid->addWidget(m_totalSpotsLabel, row++, 1);

    root->addLayout(grid);
    root->addStretch();

    // ── Clear All Spots button ──────────────────────────────────────────
    auto* btnRow = new QHBoxLayout;
    auto* clearBtn = new QPushButton("Clear All Spots");
    clearBtn->setFixedWidth(120);
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        m_model->sendCommand("spot clear");
        m_totalSpotsLabel->setText("0");
        emit settingsChanged();
    });
    btnRow->addWidget(clearBtn);
    btnRow->addStretch();
    root->addLayout(btnRow);
}

void SpotSettingsDialog::updateColorSwatch(QPushButton* btn, const QColor& color)
{
    btn->setStyleSheet(QString(
        "QPushButton { background: %1; border: 2px solid #405060; border-radius: 3px; }"
        "QPushButton:hover { border-color: #c8d8e8; }").arg(color.name()));
}

} // namespace MasterSDR
