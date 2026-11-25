#pragma once

// Station antenna switches (models/StationAntenna.h) in the RX ANT and TX ANT
// fields of VfoWidget and RxApplet: one submenu per device, after the radio's
// own ports, and the same field text in both. A device owns the field when
// the radio has nothing to choose there: no port of its own, or one fixed one.

#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/StationAntenna.h"

#include <QAccessible>
#include <QAction>
#include <QCoreApplication>
#include <QMenu>
#include <QString>
#include <QPoint>
#include <QStringList>
#include <QTextDocument>
#include <QToolTip>
#include <QWidget>
#include <QVariant>

#include <algorithm>
#include <optional>

namespace AetherSDR {

// A pick from a device submenu: the action's data names the device and port.
struct StationAntennaPick {
    QString id;
    QString token;
};

inline QVariant stationAntennaActionData(const QString& id, const QString& token)
{
    return QStringList{QStringLiteral("station-antenna"), id, token};
}

inline std::optional<StationAntennaPick> stationAntennaPickFrom(const QVariant& data)
{
    const QStringList parts = data.toStringList();
    if (data.typeId() != QMetaType::QStringList || parts.size() != 3
        || parts.at(0) != QLatin1String("station-antenna")) {
        return std::nullopt;
    }
    return StationAntennaPick{parts.at(1), parts.at(2)};
}

// Device labels are operator text on the device: a tooltip shows them as
// typed, never as markup (QToolTip renders text that looks like rich text).
inline QString stationAntennaToolTip(const QString& text)
{
    return text.isEmpty() ? QString() : Qt::convertFromPlainText(text, Qt::WhiteSpaceNormal);
}

// A refused pick, said at the control: a tooltip for the eye and an
// announcement for a screen reader (docs/a11y.md: a tooltip alone is not
// enough). Polite, so it queues behind what the reader is saying.
inline void showStationAntennaRefusal(QWidget* anchor, const QString& refusal)
{
    if (anchor == nullptr || refusal.isEmpty()) {
        return;
    }
    QToolTip::showText(anchor->mapToGlobal(QPoint(0, 0)), stationAntennaToolTip(refusal), anchor);
    if (QAccessible::isActive()) {
        QAccessibleAnnouncementEvent event(anchor, refusal);
        event.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
        QAccessible::updateAccessibility(&event);
    }
}

// QAction text with '&' shown as typed, not taken as a mnemonic.
inline QString stationAntennaActionText(QString text)
{
    return text.replace(QLatin1Char('&'), QLatin1String("&&"));
}

// One submenu per device, titled as a station switch. Its entries stay
// listed when they cannot be picked: disabled, the reason as the tooltip; a
// device that cannot be used at all shows its reason first, as a line of the
// submenu and as the submenu's tooltip.
inline void addStationAntennaMenus(QMenu& menu, const QVector<StationAntennaView>& views,
                                   bool tx, bool showCurrent = true)
{
    bool first = true;
    for (const StationAntennaView& view : views) {
        const QVector<StationAntennaChoice>& choices = tx ? view.tx : view.rx;
        if (!view.active || (choices.isEmpty() && view.unavailable.isEmpty())) {
            continue;
        }
        if (first) {
            menu.addSeparator();
            first = false;
        }
        QMenu* sub = menu.addMenu(stationAntennaMenuTitle(view.heading));
        sub->setObjectName(QStringLiteral("stationAntennaMenu_%1").arg(view.id));
        sub->setToolTipsVisible(true);
        const QString subTip = view.unavailable.isEmpty() ? view.note : view.unavailable;
        sub->menuAction()->setToolTip(stationAntennaToolTip(subTip));
        sub->menuAction()->setStatusTip(subTip);
        if (!view.unavailable.isEmpty()) {
            sub->addAction(stationAntennaActionText(view.unavailable))->setEnabled(false);
        } else if (!view.note.isEmpty()) {
            sub->addAction(stationAntennaActionText(view.note))->setEnabled(false);
        }
        const QString current = !showCurrent ? QString() : tx ? view.txCurrent : view.rxCurrent;
        for (const StationAntennaChoice& choice : choices) {
            QAction* act = sub->addAction(stationAntennaActionText(choice.label));
            act->setData(stationAntennaActionData(view.id, choice.token));
            act->setCheckable(true);
            act->setChecked(choice.token == current);
            const bool enabled = choice.enabled && view.unavailable.isEmpty();
            act->setEnabled(enabled);
            const QString tip = enabled ? choice.hint
                : !view.unavailable.isEmpty() ? view.unavailable : choice.reason;
            act->setToolTip(stationAntennaToolTip(tip));
            act->setStatusTip(tip);
        }
    }
}

// The radio's own ports for one direction: the slice's list, else the radio's.
inline qsizetype radioAntennaCount(const RadioModel& radio, const SliceModel& slice, bool tx)
{
    const QStringList own = tx ? slice.txAntennaList() : slice.rxAntennaList();
    return own.isEmpty() ? radio.antennaList().size() : own.size();
}

// Whether the RX ANT / TX ANT buttons are enabled, for the availability
// registry in VfoWidget and RxApplet. RX is restricted only on a receive-only
// radio with one fixed input and nothing else to offer; a station switch that
// offers RX ports for this radio is something else to offer, so receiving
// never waits on transmit permission. TX stays tied to canTransmit.
inline bool rxAntennaFieldAvailable(bool connected, const RadioCapabilities& caps,
                                    const SliceModel* slice, bool hasVirtualAntennas,
                                    bool hasStationRxAntennas)
{
    return !connected || caps.canTransmit || slice == nullptr
           || slice->rxAntennaList().size() != 1 || hasVirtualAntennas
           || hasStationRxAntennas;
}

inline bool txAntennaFieldAvailable(bool connected, const RadioCapabilities& caps)
{
    return !connected || caps.canTransmit;
}

struct StationAntennaField {
    QString text;
    QString toolTip;   // ready for setToolTip: device text escaped
};

// What the field shows while a device owns it, or nothing: the first device
// that reports a position for this radio, as the device reports it. A device
// owns the field when the radio has at most one port in that direction (the
// HL2's RX "ANT", an RTL's "RX"): that port is always the one in use, so the
// switch's position is the only thing the field can tell.
inline std::optional<StationAntennaField> stationAntennaField(const RadioModel* radio,
                                                              const SliceModel* slice, bool tx)
{
    if (radio == nullptr || slice == nullptr || radioAntennaCount(*radio, *slice, tx) > 1) {
        return std::nullopt;
    }
    const QVector<StationAntennaView> views = radio->stationAntennaViews();
    if (views.isEmpty()) {
        return std::nullopt;
    }
    const auto reported = std::find_if(views.cbegin(), views.cend(),
                                       [tx](const StationAntennaView& view) {
                                           return !(tx ? view.txCurrent : view.rxCurrent).isEmpty();
                                       });
    const StationAntennaView& view = reported != views.cend() ? *reported : views.first();
    StationAntennaField out;
    out.text = tx ? view.txLabel : view.rxLabel;
    out.toolTip = QCoreApplication::translate("StationAntennaMenu", "%1 antenna: %2, on the %3")
                      .arg(tx ? QCoreApplication::translate("StationAntennaMenu", "Transmit")
                              : QCoreApplication::translate("StationAntennaMenu", "Receive"),
                           out.text, stationAntennaMenuTitle(view.heading));
    const QString more = view.unavailable.isEmpty() ? view.note : view.unavailable;
    if (!more.isEmpty()) {
        out.toolTip += QLatin1Char('\n') + more;
    }
    out.toolTip = stationAntennaToolTip(out.toolTip);
    return out;
}

} // namespace AetherSDR
