#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace AetherSDR {

struct RadioCapabilities;

// `aethersdr_rx_span_rates` and `aethersdr_rx_span`, the span group of the
// extension set (docs/user/docs/tci-server.md). Parsing, the span model and the
// snap only; TciServer applies a set through RadioModel::requestPanBandwidth(),
// the path the desktop's span controls use.
namespace TciSpanExtension {

inline constexpr const char* kToken = "rx_span";
inline constexpr const char* kRatesCommand = "aethersdr_rx_span_rates";
inline constexpr const char* kSpanCommand = "aethersdr_rx_span";

// What the span is made of, read from RadioCapabilities::panSpanModel. An
// absent record is answered as a continuous, per-pan span, as the desktop's
// span controls treat it (PanSpanControlGate.h): absence never restricts.
struct Model {
    bool followsRate{false};  // span IS the sample rate: only `rates` exist
    bool radioWide{false};    // one span for every pan
    QVector<int> rates;       // Hz, ascending; empty unless followsRate
};
Model model(const RadioCapabilities& caps);

// The gate: a connected radio whose pan display control declares the span
// (PanDisplayControl::declaresSpan(), the predicate that keeps the desktop's
// -/+ span pair live), and, when the span follows the sample rate, at least
// one rate to snap to.
bool offered(bool connected, const RadioCapabilities& caps);

struct Request {
    int trx{0};
    bool isSet{false};
    qint64 hz{0};
};
// `<trx>` (read) or `<trx>,<hz>` (set); integers, trx >= 0, hz > 0.
std::optional<Request> parseSpan(const QStringList& args);

// The span a set asks the radio for. With followsRate, the listed rate nearest
// by ratio (log distance, as Hl2Backend and AnanBackend snap; the lower one on
// a tie), among the rates inside [minHz, maxHz] when those limits are known
// (> 0). Otherwise the request clamped to the known limits. `rates` must be
// ascending.
qint64 snap(qint64 hz, const Model& model, qint64 minHz, qint64 maxHz);

// The rates a set can reach now: `rates` inside [minHz, maxHz] when known.
QVector<int> reachableRates(const Model& model, qint64 minHz, qint64 maxHz);

// `aethersdr_rx_span_rates:<follows_rate>,<radio_wide>[,<hz>...];`
QString ratesLine(const Model& model, const QVector<int>& reachable);
// `aethersdr_rx_span:<trx>,<hz>;`
QString spanLine(int trx, qint64 hz);

} // namespace TciSpanExtension
} // namespace AetherSDR
