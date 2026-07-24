#include "gui/WaterfallHistoryBuffer.h"

#include <QDebug>

#include <algorithm>
#include <utility>

using AetherSDR::WaterfallHistoryBuffer;

namespace {

int fail(const char* message)
{
    qCritical().noquote() << message;
    return 1;
}

} // namespace

int main()
{
    WaterfallHistoryBuffer history;
    history.configure(8, 600);
    if (!history.isConfigured() || history.size() != QSize(8, 600)) {
        return fail("history configuration was not retained");
    }
    if (history.allocatedBytes() != 0 || history.allocatedChunkCount() != 0) {
        return fail("history allocated pixel rows eagerly");
    }

    quint8* newest = history.writableRow(599);
    if (!newest) {
        return fail("last logical ring slot was not writable");
    }
    for (int x = 0; x < 8; ++x) {
        newest[x] = static_cast<quint8>(x * 10);
    }
    if (history.allocatedChunkCount() != 1
        || history.allocatedBytes() >
            WaterfallHistoryBuffer::kRowsPerChunk * history.width()) {
        return fail("first row allocated more than one bounded chunk");
    }
    if (!history.row(599) || history.row(599)[7] != 70) {
        return fail("written intensity row was not readable");
    }

    quint8* older = history.writableRow(0);
    if (!older || history.allocatedChunkCount() != 2) {
        return fail("a distant ring slot did not allocate its own chunk");
    }
    std::fill(older, older + history.width(), quint8(200));

    if (!history.resizeWidth(4) || history.width() != 4) {
        return fail("width resize failed");
    }
    const quint8* resizedNewest = history.row(599);
    if (!resizedNewest || resizedNewest[0] != 0
        || resizedNewest[1] != 20 || resizedNewest[3] != 60) {
        return fail("width resize did not preserve nearest-neighbor intensity");
    }
    if (!history.row(0) || history.row(0)[3] != 200) {
        return fail("width resize lost a second allocated chunk");
    }

    WaterfallHistoryBuffer copied = history;
    quint8* copiedOldest = copied.writableRow(0);
    if (!copiedOldest) {
        return fail("copied history row was not writable");
    }
    copiedOldest[3] = 17;
    if (!history.row(0) || history.row(0)[3] != 200
        || copied.row(0)[3] != 17) {
        return fail("copy-on-write did not isolate history chunks");
    }

    WaterfallHistoryBuffer moved = std::move(history);
    if (history.isConfigured() || !moved.isConfigured()
        || !moved.row(599) || moved.row(599)[3] != 60) {
        return fail("move did not transfer ownership and clear the source");
    }
    history.configure(4, 600);
    if (!history.isConfigured() || history.allocatedBytes() != 0
        || history.writableRow(0) == nullptr) {
        return fail("moved-from history could not be configured safely");
    }
    history = std::move(moved);
    if (moved.isConfigured() || !history.row(0)
        || history.row(0)[3] != 200) {
        return fail("move assignment did not preserve allocated rows");
    }

    history.discardRows();
    if (!history.isConfigured() || history.allocatedBytes() != 0
        || history.row(599) != nullptr) {
        return fail("discardRows did not release chunks while retaining shape");
    }

    history.reset();
    if (history.isConfigured() || history.writableRow(0) != nullptr) {
        return fail("reset did not clear the logical history shape");
    }
    return 0;
}
