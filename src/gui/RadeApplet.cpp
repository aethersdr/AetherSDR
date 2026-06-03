#ifdef HAVE_RADE

#include "RadeApplet.h"
#include "core/ThemeManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>

namespace {
// Base style for muted-grey informational labels (SNR value, offset, "SNR" caption).
// Signal-specific colors (#e0e040 / #00ff88) are substituted into this template
// by setRadeSnr; all other uses take the grey directly.
constexpr auto kMutedStyle =
    "QLabel { color: #8090a0; font-size: 10px;"
    " background: transparent; border: none; padding: 0; margin: 0; }";
} // namespace

namespace AetherSDR {

RadeApplet::RadeApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/rade"));

    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(4, 4, 4, 4);
    vbox->setSpacing(2);

    m_inactiveLabel = new QLabel(tr("RADE inactive"));
    m_inactiveLabel->setStyleSheet(
        "QLabel { color: #506070; font-size: 10px; "
        "background: transparent; border: none; }");
    m_inactiveLabel->setAlignment(Qt::AlignCenter);
    vbox->addWidget(m_inactiveLabel);

    m_dataRows = new QWidget;
    m_dataRows->setAttribute(Qt::WA_TranslucentBackground);
    auto* dataVbox = new QVBoxLayout(m_dataRows);
    dataVbox->setContentsMargins(0, 0, 0, 0);
    dataVbox->setSpacing(3);

    m_statusLabel = new QLabel;
    m_statusLabel->setTextFormat(Qt::RichText);
    ThemeManager::instance().applyStyleSheet(m_statusLabel,
        "QLabel { color: {{color.accent}}; font-size: 10px; font-weight: bold;"
        " background: transparent; border: none; padding: 0; margin: 0; }");
    dataVbox->addWidget(m_statusLabel);

    {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);

        auto* snrLbl = new QLabel(tr("SNR"));
        snrLbl->setStyleSheet(kMutedStyle);

        m_snrLabel = new QLabel(QStringLiteral("---"));
        m_snrLabel->setStyleSheet(kMutedStyle);

        row->addWidget(snrLbl);
        row->addWidget(m_snrLabel);
        row->addStretch();
        dataVbox->addLayout(row);
    }

    m_callsignLabel = new QLabel;
    ThemeManager::instance().applyStyleSheet(m_callsignLabel,
        "QLabel { color: {{color.accent}}; font-size: 10px; font-weight: bold;"
        " background: transparent; border: none; padding: 0; margin: 0; }");
    m_callsignLabel->hide();
    dataVbox->addWidget(m_callsignLabel);

    m_offsetLabel = new QLabel;
    m_offsetLabel->setStyleSheet(kMutedStyle);
    m_offsetLabel->hide();
    dataVbox->addWidget(m_offsetLabel);

    dataVbox->addStretch();
    m_dataRows->hide();
    vbox->addWidget(m_dataRows);
    vbox->addStretch();
}

void RadeApplet::setRadeActive(bool on, const QString& label)
{
    m_active = on;
    m_modeLabel = label.isEmpty() ? QStringLiteral("RADE") : label;

    m_inactiveLabel->setVisible(!on);
    m_dataRows->setVisible(on);

    if (!on) {
        m_statusLabel->setText({});
        m_snrLabel->setText(QStringLiteral("---"));
        m_snrLabel->setStyleSheet(kMutedStyle);
        m_callsignLabel->hide();
        m_callsignLabel->clear();
        m_offsetLabel->hide();
    } else {
        // Show initial unsynced state — syncChanged fires once audio starts flowing
        const QString led = QStringLiteral("<font color='#505050'>○</font>");
        m_statusLabel->setText(m_modeLabel + QLatin1Char(' ') + led);
        m_callsignLabel->clear();
        m_callsignLabel->hide();
        m_offsetLabel->hide();
    }
}

void RadeApplet::setRadeSynced(bool synced)
{
    if (!m_active) return;

    const QString led = synced
        ? QStringLiteral("<font color='#00ff88'>●</font>")
        : QStringLiteral("<font color='#505050'>○</font>");
    m_statusLabel->setText(m_modeLabel + QLatin1Char(' ') + led);

    if (!synced) {
        m_snrLabel->setStyleSheet(kMutedStyle);
        m_snrLabel->setText(QStringLiteral("---"));
        m_offsetLabel->hide();
    } else {
        // New sync — clear callsign from previous transmission
        m_callsignLabel->clear();
        m_callsignLabel->hide();
    }
}

void RadeApplet::setRadeSnr(float snrDb)
{
    if (!m_active) return;
    const QString color = (snrDb < 5.0f) ? QStringLiteral("#e0e040")
                                          : QStringLiteral("#00ff88");
    m_snrLabel->setStyleSheet(
        QString("QLabel { color: %1; font-size: 10px;"
                " background: transparent; border: none; padding: 0; margin: 0; }")
            .arg(color));
    m_snrLabel->setText(QString("%1dB").arg(qRound(snrDb)));
}

void RadeApplet::setRadeFreqOffset(float hz)
{
    if (!m_active) return;
    const QString sign = (hz >= 0) ? QStringLiteral("+") : QString{};
    m_offsetLabel->setText(
        QString("%1%2Hz").arg(sign).arg(static_cast<int>(hz)));
    m_offsetLabel->show();
}

void RadeApplet::setRadeCallsign(const QString& callsign)
{
    if (callsign.isEmpty()) {
        m_callsignLabel->clear();
        m_callsignLabel->hide();
    } else {
        m_callsignLabel->setText(callsign);
        m_callsignLabel->show();
    }
}

} // namespace AetherSDR

#endif // HAVE_RADE
