#pragma once

#include <QString>
#include <QtGlobal>

#include <map>
#include <optional>

namespace AetherSDR {

// Sorts live-preview results into blocks, one per ASR segment, so the panel can
// show each segment's provisional text and replace it with that segment's
// final text when the backend has decoded it.
//
// Block N is the audio between the end of segment N-1 and the end of segment
// N; segment ids come from AsrSegmenter and start at 0 after every reset. A
// result belongs to the block that contains its start sample. Within a block,
// the recogniser's final results accumulate, its latest provisional result
// replaces the previous one, and the pieces read in audio order.
//
// Pure and single-threaded: AsrEngine drives it on the main thread.
class LivePreviewAssembler {
public:
    struct Update {
        quint64 block = 0;
        QString text;
    };

    void reset();

    // Segment `id` closed at `endSample`: block `id` ends there and block
    // id + 1 starts there.
    void closeBlock(quint64 id, qint64 endSample);

    // A recogniser result. Returns the block's new full text when it changed;
    // nothing when the result belongs to an already-resolved block or changes
    // nothing on screen.
    std::optional<Update> addResult(qint64 startSample, qint64 endSample, const QString& text,
                                    bool isFinal);

    // Segment `id` has been decoded (or discarded): its block, and any older
    // one, no longer takes preview text.
    void resolve(quint64 id);

private:
    struct Content {
        std::map<qint64, QString> finals; // by start sample
        qint64 provisionalStart = -1;
        QString provisional;
        QString shown;                    // last text returned for this block
    };

    std::map<quint64, qint64> m_closedEnd; // closed, unresolved blocks: id -> end sample
    std::map<quint64, Content> m_content;
    quint64 m_openBlock = 0;
    qint64 m_resolvedEnd = 0;   // results starting before this belong to resolved blocks
    bool m_anyResolved = false;
    quint64 m_lastResolved = 0;
};

} // namespace AetherSDR
