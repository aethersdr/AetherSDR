#include "rig/bridged.hpp"

#include <utility>

namespace data2g::rig {
namespace {

class BridgedBackend : public RigBackend {
public:
    BridgedBackend(HamlibConfig config, std::shared_ptr<SerialTransport> transport,
                   BackendFactory make_inner)
        : config_(std::move(config)),
          transport_(std::move(transport)),
          make_inner_(std::move(make_inner)) {}

    ~BridgedBackend() override { close(); }

    void open() override {
        HamlibConfig inner_config = config_;

        bridge_ = std::make_unique<LoopbackBridge>(transport_);
        bridge_->start();  // opens the transport too; throws RigError
        inner_config.device = bridge_->endpoint();

        // **Hamlib must not be asked to key a control line here.**
        // `ser_set_dtr` is a `TIOCMSET` ioctl and the descriptor it
        // holds is a socket, so the call fails and the radio never
        // keys. `Vox` is the value that means "do not key at all"
        // (hamlib.cpp maps it to ptt_type "None"), which is exactly
        // right: something else -- us, below -- is doing it.
        const bool line_ptt = ptt_uses_control_line(config_.ptt_method);
        if (line_ptt) inner_config.ptt_method = PttMethod::Vox;

        // The serial-line tokens go the same way. `serial_speed`,
        // `data_bits`, `dtr_state` and the rest are applied by Hamlib's
        // `serial_open`, which a network port never reaches -- so
        // setting them would be silently doing nothing. The transport
        // owns the line settings, and applied them in its own `open()`.
        try {
            park_control_lines(line_ptt);
            inner_ = make_inner_(inner_config);
            if (!inner_) throw RigError("no rig backend was built");
            inner_->open();
        } catch (...) {
            inner_.reset();
            bridge_.reset();  // stops the pump and closes the transport
            throw;
        }
    }

    void close() noexcept override {
        if (inner_) {
            inner_->close();
            inner_.reset();
        }
        // After the inner backend, never before: Hamlib's `rig_close`
        // may write a last command (some backends restore the mode they
        // changed), and pulling the bridge out from under it would turn
        // an orderly close into a broken pipe on the way out.
        bridge_.reset();
    }

    void set_ptt(bool on) override {
        if (ptt_uses_control_line(config_.ptt_method)) {
            if (!transport_) throw RigError("no transport to key");
            if (config_.ptt_method == PttMethod::Dtr) {
                transport_->set_dtr(on);
            } else {
                transport_->set_rts(on);
            }
            return;
        }
        require_inner();
        inner_->set_ptt(on);
    }

    double frequency_hz() override {
        require_inner();
        return inner_->frequency_hz();
    }

    std::string description() const override {
        std::string text = inner_ ? inner_->description() : std::string("rig");
        if (transport_) {
            const std::string via = transport_->description();
            if (!via.empty()) text += " via " + via;
        }
        return text;
    }

private:
    void require_inner() const {
        if (!inner_) throw RigError("the rig is not open");
    }

    // Interfaces that steal their power from a control line need it
    // held for the whole session, which is what `LineState` is for on
    // the desktop. Over a socket Hamlib cannot do it, so it happens
    // here.
    void park_control_lines(bool line_ptt) {
        if (!transport_) return;
        const bool dtr_is_ptt = line_ptt && config_.ptt_method == PttMethod::Dtr;
        const bool rts_is_ptt = line_ptt && config_.ptt_method == PttMethod::Rts;

        // A parked state on the line that keys the radio would be a
        // transmitter held on from the moment the session opens. The
        // PTT line is parked unkeyed instead, whatever the setting
        // says.
        if (dtr_is_ptt) {
            transport_->set_dtr(false);
        } else if (config_.dtr == LineState::High) {
            transport_->set_dtr(true);
        } else if (config_.dtr == LineState::Low) {
            transport_->set_dtr(false);
        }

        if (rts_is_ptt) {
            transport_->set_rts(false);
        } else if (config_.rts == LineState::High) {
            transport_->set_rts(true);
        } else if (config_.rts == LineState::Low) {
            transport_->set_rts(false);
        }
    }

    HamlibConfig config_;
    std::shared_ptr<SerialTransport> transport_;
    BackendFactory make_inner_;
    std::unique_ptr<LoopbackBridge> bridge_;
    std::unique_ptr<RigBackend> inner_;
};

}  // namespace

SerialParams resolve_serial_params(const HamlibConfig& config,
                                   const SerialDefaults& defaults) {
    SerialParams params;
    params.device_id = config.device;
    params.baud = config.baud > 0 ? config.baud : defaults.baud;

    switch (config.data_bits) {
        case DataBits::Seven: params.data_bits = 7; break;
        case DataBits::Eight: params.data_bits = 8; break;
        case DataBits::Default: params.data_bits = defaults.data_bits; break;
    }
    switch (config.stop_bits) {
        case StopBits::One: params.stop_bits = 1; break;
        case StopBits::Two: params.stop_bits = 2; break;
        case StopBits::Default: params.stop_bits = defaults.stop_bits; break;
    }
    switch (config.parity) {
        case Parity::None: params.parity = SerialParams::kNoParity; break;
        case Parity::Odd: params.parity = SerialParams::kOdd; break;
        case Parity::Even: params.parity = SerialParams::kEven; break;
        case Parity::Default: params.parity = defaults.parity; break;
    }
    switch (config.handshake) {
        case Handshake::None: params.flow = SerialParams::kNoFlow; break;
        case Handshake::XonXoff: params.flow = SerialParams::kXonXoff; break;
        case Handshake::Hardware: params.flow = SerialParams::kRtsCts; break;
        case Handshake::Default: params.flow = defaults.flow; break;
    }

    // The initial line states, which on a real serial port come from
    // the *operating system* rather than from Hamlib -- see
    // `SerialParams::dtr`. Default therefore means asserted, not
    // "leave it alone": leaving it alone is what a desktop never does.
    params.dtr = config.dtr != LineState::Low;
    params.rts = config.rts != LineState::Low;

    // ...except the line doing PTT, which `rig_open` explicitly drops
    // so that opening the radio does not key it. Same rule, same
    // reason, and the reason it is safe to raise the *other* line: a
    // desktop already does, so no working station depends on it being
    // low.
    if (config.ptt_method == PttMethod::Dtr) params.dtr = false;
    if (config.ptt_method == PttMethod::Rts) params.rts = false;

    // Hamlib's three -RIG_ECONF cases, re-derived because its own are
    // unreachable for a network port. Each one is a setting that cannot
    // do what it says: the flow-control driver and the PTT code would be
    // driving the same wire.
    const bool rts_held = config.rts != LineState::Default;
    const bool dtr_held = config.dtr != LineState::Default;
    if (rts_held && params.flow == SerialParams::kRtsCts) {
        throw RigError("RTS cannot be held at a fixed level while hardware "
                       "handshaking is using it");
    }
    if (config.ptt_method == PttMethod::Rts) {
        if (params.flow == SerialParams::kRtsCts) {
            throw RigError("PTT by RTS cannot share the line with hardware "
                           "handshaking");
        }
        if (rts_held) {
            throw RigError("PTT by RTS cannot share the line with a fixed RTS "
                           "level");
        }
    }
    if (config.ptt_method == PttMethod::Dtr && dtr_held) {
        throw RigError("PTT by DTR cannot share the line with a fixed DTR level");
    }
    return params;
}

std::unique_ptr<RigBackend> make_bridged_backend(
    HamlibConfig config, std::shared_ptr<SerialTransport> transport,
    BackendFactory make_inner) {
    if (!make_inner) throw RigError("make_bridged_backend: no backend factory");

    // No transport: the operator chose a network rig. Hamlib opens the
    // socket itself, for model 2 and for a native backend alike, and
    // there is nothing to add -- wrapping it would only put two of our
    // threads in the path of a socket Hamlib is perfectly able to open.
    if (!transport) {
        if (!is_network_device(config.device)) {
            throw RigError("'" + config.device +
                           "' is not an address Hamlib will dial, and no device "
                           "was opened for it");
        }
        return make_inner(config);
    }
    return std::make_unique<BridgedBackend>(std::move(config), std::move(transport),
                                            std::move(make_inner));
}

}  // namespace data2g::rig
