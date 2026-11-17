#pragma once

// Station antenna switches (models/StationAntenna.h) in the RX ANT and TX ANT
// fields of VfoWidget and RxApplet: one submenu per device, after the radio's
// own ports, and the same field text in both. A device owns the field only
// when the radio publishes no port of its own.

#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/StationAntenna.h"

#include <QAction>
#include <QCoreApplication>
#include <QMenu>
#include <QString>
#include <QStringList>
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
        sub->menuAction()->setToolTip(subTip);
        sub->menuAction()->setStatusTip(subTip);
        if (!view.unavailable.isEmpty()) {
            sub->addAction(view.unavailable)->setEnabled(false);
        } else if (!view.note.isEmpty()) {
            sub->addAction(view.note)->setEnabled(false);
        }
        const QString current = !showCurrent ? QString() : tx ? view.txCurrent : view.rxCurrent;
        for (const StationAntennaChoice& choice : choices) {
            QAction* act = sub->addAction(choice.label);
            act->setData(stationAntennaActionData(view.id, choice.token));
            act->setCheckable(true);
            act->setChecked(choice.token == current);
            const bool enabled = choice.enabled && view.unavailable.isEmpty();
            act->setEnabled(enabled);
            const QString tip = enabled ? choice.hint
                : !view.unavailable.isEmpty() ? view.unavailable : choice.reason;
            act->setToolTip(tip);
            act->setStatusTip(tip);
        }
    }
}

inline bool radioPublishedAntennas(const RadioModel& radio, const SliceModel& slice, bool tx)
{
    return !(tx ? slice.txAntennaList() : slice.rxAntennaList()).isEmpty()
           || !radio.antennaList().isEmpty();
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
    QString toolTip;
};

// What the field shows while a device owns it, or nothing: the first device
// that reports a position for this radio, as the device reports it.
inline std::optional<StationAntennaField> stationAntennaField(const RadioModel* radio,
                                                              const SliceModel* slice, bool tx)
{
    if (radio == nullptr || slice == nullptr || radioPublishedAntennas(*radio, *slice, tx)) {
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
    return out;
}

} // namespace AetherSDR
