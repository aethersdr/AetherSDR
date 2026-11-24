// A byte pipe to a radio that is not a serial port.
//
// **Why this exists at all.** `docs/android.md` recorded rig control as
// dropping "for a structural reason, not a scoping one": Hamlib's
// serial layer opens a *path*, and Android hands an unprivileged app no
// `/dev/ttyUSB0`, so using it would mean patching a pinned dependency.
// The premise is right and the conclusion was wrong, because Hamlib
// does not actually require a serial port. `rig_open()` runs the
// configured pathname through `parse_hoststr()` -- which explicitly
// rejects `/dev/...` and `COM*` and accepts `host:port` -- and on a
// match sets `type.rig = RIG_PORT_NETWORK` for **any** model, not just
// model 2. `port_open()` then dispatches to `network_open()`. That is
// the mechanism behind the well-worn `rigctld -m <native model> -r
// <ser2net host>:4001` recipe, and it means a socket is a first-class
// transport for every backend Hamlib has.
//
// So: this interface is the byte pipe, `bridge.hpp` turns one into a
// loopback socket, and `bridged.hpp` hands Hamlib that socket's
// address. Nothing is patched, no backend is reimplemented, and the
// radio list on a phone is the radio list on the desktop -- which is
// the property worth all of this, since "which radios work" differing
// per platform is exactly what `native/cmake/hamlib.cmake` pins a
// version to avoid.
//
// **Qt-free and Hamlib-free on purpose**, and in `data2g_core` rather
// than `data2g_rig`: the whole mechanism is then testable in a
// `--no-rig` build against a fake transport, which is the same trade
// `rx::Decoder` makes so the receive loop tests need no onnxruntime.

#ifndef DATA2G_RIG_TRANSPORT_HPP
#define DATA2G_RIG_TRANSPORT_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "rig/backend.hpp"

namespace data2g::rig {

// One end of a serial-shaped link: USB CDC/FTDI/CP210x through the
// Android USB host API, or an RFCOMM socket to a Bluetooth radio.
//
// **Errors are `RigError`**, the same type `RigBackend` throws, because
// everything above here reports rig trouble one way and an operator
// cannot act on the distinction between "the radio refused" and "the
// cable fell out" any differently.
//
// **Threading contract, and it is load-bearing.** `read()` and
// `write()` are called concurrently, from two different threads, for
// the whole life of a session -- see `bridge.hpp` for why one thread
// per direction is what avoids an idle poll loop on a phone. Both
// Android transports satisfy this naturally (separate USB endpoints;
// `BluetoothSocket`'s input and output streams are independent), which
// is the only reason it is allowed to be a requirement. `close()` may
// be called from a third thread while a `read()` is blocked, and
// **must wake it** -- that is how a session shuts down without waiting
// out a read timeout. `set_dtr`/`set_rts` may overlap a read or a
// write: on USB they are control transfers, on a separate endpoint
// from the data.
// The line settings a transport actually has to be given.
//
// **Not the same shape as `HamlibConfig`'s, and the difference is the
// point.** There, `Default` means *do not set the token* and leave the
// backend's own value -- right for almost every rig, and the reason
// those are enums with a Default member rather than guesses at 8-N-1.
// Over a socket that promise cannot be kept by inaction: Hamlib applies
// its per-rig defaults in `serial_open`, which a network port never
// reaches, so a transport given nothing would run at whatever the USB
// chip powered up with. `bridged.hpp`'s `resolve_serial_params` closes
// that by asking Hamlib what it *would* have used -- so "the backend's
// own value" keeps meaning the same thing on a phone as on a desktop.
struct SerialParams {
    std::string device_id;
    int baud = 9600;
    int data_bits = 8;
    int stop_bits = 1;

    enum Parity { kNoParity = 0, kOdd = 1, kEven = 2 };
    int parity = kNoParity;

    enum Flow { kNoFlow = 0, kRtsCts = 1, kXonXoff = 2 };
    int flow = kNoFlow;

    // The state to leave DTR and RTS in once the link is open.
    //
    // **These are not a nicety, and they default to true for a reason
    // Hamlib states outright.** `src/rig.c`, opening a PTT-by-line
    // port: *"Needed on Linux because the serial port driver sets
    // RTS/DTR on open - only need to address the PTT line as we offer
    // config parameters to control the other (dtr_state &
    // rts_state)"*. Hamlib never raises them itself; it relies on the
    // OS having done so, and only ever explicitly drops the line that
    // is doing PTT. A serial port opened by `rigctl` therefore presents
    // a radio with both lines high.
    //
    // A bridged transport reaches none of that -- `serial_open` is not
    // in its path -- and `usb-serial-for-android` deasserts both in
    // `openInt()`. So an Icom that was silent on a phone and answered
    // instantly on a desktop, same cable, same chip, same baud, was
    // being handed a different pair of control lines. This is the same
    // omission `resolve_serial_params` exists to fix, one field later.
    bool dtr = true;
    bool rts = true;
};

// What a given Hamlib backend would have configured a serial port with.
// Filled by `rig::serial_defaults(model)` in `data2g_rig`; a plain
// struct of ints here so `data2g_core` can carry it with no libhamlib.
// The `parity` and `flow` encodings are `SerialParams`'s.
struct SerialDefaults {
    int baud = 9600;
    int data_bits = 8;
    int stop_bits = 1;
    int parity = SerialParams::kNoParity;
    int flow = SerialParams::kNoFlow;
};

class SerialTransport {
public:
    virtual ~SerialTransport() = default;

    // Acquire the device and apply the line settings.
    //
    // Deliberately *not* done by whoever constructs the transport: this
    // blocks, sometimes for seconds, and the entire point of
    // `RigController` is that nothing which blocks on a radio happens
    // anywhere but its worker thread. Permission prompts are the
    // caller's problem and must be settled before this is reached --
    // they need a UI thread and an activity, which a rig worker has
    // neither of.
    virtual void open() = 0;

    // Release it. Called from the pump's teardown and must not throw;
    // by then there is nobody left to tell, exactly as with
    // `RigBackend::close`. Must be idempotent, and must unblock a
    // concurrent `read()`.
    virtual void close() noexcept = 0;

    // Up to `n` bytes, blocking at most `timeout_ms`. Returns the count,
    // which is 0 on a timeout -- **a timeout is not an error**, it is
    // the ordinary state of a radio nobody has asked anything.
    // Returning 0 forever is what an idle link looks like.
    virtual std::size_t read(std::uint8_t* dst, std::size_t n, int timeout_ms) = 0;

    // All `n` bytes, or throw. CAT commands are short and a partial
    // write is a corrupt command, so there is no short-write case worth
    // handing upward.
    virtual void write(const std::uint8_t* src, std::size_t n) = 0;

    // Modem control lines, for the PTT methods that key a radio by
    // asserting one.
    //
    // **These cannot go through Hamlib on this path and that is not an
    // oversight.** `ser_set_dtr` is a `TIOCMSET` ioctl; the descriptor
    // Hamlib holds here is a socket, so the ioctl simply fails. The
    // line has to be driven on the transport itself, which is why
    // `bridged.cpp` intercepts DTR/RTS keying instead of delegating it.
    virtual void set_dtr(bool on) = 0;
    virtual void set_rts(bool on) = 0;

    // For status text: "USB CP2102 (Silicon Labs)", "BT: FT-891".
    virtual std::string description() const = 0;
};

}  // namespace data2g::rig

#endif
