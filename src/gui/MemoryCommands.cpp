#include "MemoryCommands.h"

#include "core/MemoryFieldValues.h"
#include "models/SliceModel.h"

#include <QPointer>

namespace AetherSDR {

QString encodeMemoryText(const QString& value)
{
    // Strip NUL/control bytes first, then apply the protocol space-encoding so
    // a hand-entered or imported value can never push a raw control byte (which
    // breaks the radio and other SmartSDR-compatible software) onto the wire.
    return MemoryFields::sanitizeText(value).replace(' ', QChar(0x7f));
}

MemoryEntry captureMemoryFromSlice(const RadioModel& model,
                                   const SliceModel& slice,
                                   const QString& name)
{
    MemoryEntry memory;
    memory.group = model.activeGlobalProfile();
    memory.owner = model.callsign();
    memory.freq = slice.frequency();
    memory.name = name.trimmed();
    memory.mode = slice.mode();
    memory.step = slice.stepHz();
    memory.offsetDir = slice.repeaterOffsetDir();
    memory.repeaterOffset = slice.fmRepeaterOffsetFreq();
    memory.toneMode = slice.fmToneMode();
    memory.toneValue = slice.fmToneValue().toDouble();
    memory.squelch = slice.squelchOn();
    memory.squelchLevel = slice.squelchLevel();
    // Memory filter fields are USB-form on the wire for FDV modes (FlexLib
    // Memory.cs clamps FDV rx_filter_low to >= 0), while the client stores
    // FDVL canonically negative since #3434. Mirror back to the ecosystem
    // form at capture so radio-side memory slots (and the CSV export built
    // from this struct) stay compatible with SmartSDR/FlexLib clients —
    // a stored negative low would be clamped to 0 there, degrading the slot.
    {
        int memLow = slice.filterLow();
        int memHigh = slice.filterHigh();
        if (SliceModel::filterPolarityLsbFamily(memory.mode)
            && memory.mode.startsWith(QStringLiteral("FDV"))
            && memLow < 0) {
            const int lo = memLow, hi = memHigh;
            memLow  = -hi;
            memHigh = -lo;
        }
        memory.rxFilterLow = memLow;
        memory.rxFilterHigh = memHigh;
    }
    memory.rttyMark = slice.rttyMark();
    memory.rttyShift = slice.rttyShift();
    memory.diglOffset = slice.diglOffset();
    memory.diguOffset = slice.diguOffset();
    return memory;
}

MemoryUpdateData buildMemoryUpdateData(const MemoryEntry& memory)
{
    MemoryUpdateData update;

    auto appendField = [&update](const QString& key, const QString& value) {
        if (!update.commandSuffix.isEmpty())
            update.commandSuffix += ' ';
        update.commandSuffix += key + '=' + value;
        update.kvs[key] = value;
    };

    appendField("group", encodeMemoryText(memory.group));
    appendField("owner", encodeMemoryText(memory.owner));
    appendField("freq", QString::number(memory.freq, 'f', 6));
    appendField("name", encodeMemoryText(memory.name));
    appendField("mode", MemoryFields::modeToWire(memory.mode));
    appendField("step", QString::number(memory.step));
    appendField("repeater", MemoryFields::offsetDirToWire(memory.offsetDir));
    appendField("repeater_offset", QString::number(memory.repeaterOffset, 'f', 6));
    appendField("tone_mode", MemoryFields::toneModeToWire(memory.toneMode));
    appendField("tone_value", QString::number(memory.toneValue, 'f', 1));
    appendField("squelch", memory.squelch ? "1" : "0");
    appendField("squelch_level", QString::number(memory.squelchLevel));
    appendField("rx_filter_low", QString::number(memory.rxFilterLow));
    appendField("rx_filter_high", QString::number(memory.rxFilterHigh));
    appendField("rtty_mark", QString::number(memory.rttyMark));
    appendField("rtty_shift", QString::number(memory.rttyShift));
    appendField("digl_offset", QString::number(memory.diglOffset));
    appendField("digu_offset", QString::number(memory.diguOffset));

    return update;
}

void createMemoryFromSlice(RadioModel* model,
                           const SliceModel* slice,
                           const QString& name,
                           QObject* callbackContext,
                           MemoryCreateCallback cb)
{
    const bool useCallbackGuard = callbackContext != nullptr;
    const QPointer<QObject> callbackGuard(callbackContext);
    auto notify = [cb, useCallbackGuard, callbackGuard](int code,
                                                        const QString& body,
                                                        int memoryIndex) {
        if (!cb)
            return;
        if (useCallbackGuard && callbackGuard.isNull())
            return;
        cb(code, body, memoryIndex);
    };

    if (!model || !slice) {
        notify(-1, QStringLiteral("No active slice is available."), -1);
        return;
    }

    const MemoryEntry memory = captureMemoryFromSlice(*model, *slice, name);
    const MemoryUpdateData update = buildMemoryUpdateData(memory);

    model->sendCmdPublic("memory create",
        [model, update, notify](int code, const QString& body) {
        if (code != 0) {
            notify(code, body, -1);
            return;
        }

        bool ok = false;
        const int index = body.trimmed().toInt(&ok);
        if (!ok) {
            notify(-1,
                   QStringLiteral("The radio returned an invalid memory id."),
                   -1);
            return;
        }

        model->sendCmdPublic(
            QString("memory set %1 %2").arg(index).arg(update.commandSuffix),
            [model, update, notify, index](int setCode, const QString& setBody) {
            if (setCode == 0) {
                model->handleMemoryStatus(index, update.kvs);
                notify(0, setBody, index);
                return;
            }

            model->sendCmdPublic(QString("memory remove %1").arg(index),
                [model, notify, index, setCode, setBody](int removeCode, const QString& removeBody) {
                if (removeCode == 0) {
                    QMap<QString, QString> removed;
                    removed["removed"] = QString{};
                    model->handleMemoryStatus(index, removed);
                }

                QString detail = setBody.simplified();
                if (detail.isEmpty())
                    detail = QStringLiteral("The radio rejected one or more memory fields.");
                if (removeCode != 0 && !removeBody.simplified().isEmpty())
                    detail += QStringLiteral(" Cleanup also failed: %1")
                        .arg(removeBody.simplified());
                notify(setCode, detail, index);
            });
        });
    });
}

} // namespace AetherSDR
