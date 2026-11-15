# Native receive audio through the existing DAX bridge

On Linux and macOS builds with WebSockets support, a backend declaring
`receiveAudioExport` can feed the existing DAX audio inputs. RTL supplies this
record for its actual typed slice formats. Its configured receiver capacity
still applies; the private eight-receiver evaluation does not alter ordinary
release capacity. Other backends retain their existing behavior.

Enable the DAX audio applet. Input 1 follows TCI receiver 0, input 2 follows
receiver 1, and so on through input 8/receiver 7. The live TCI receiver map supplies
both paths; sparse internal slice IDs are never interpreted as channel numbers.
Closing/recreating a slice preserves surviving assignments. The applet shows
the slice attached to each input. The TCI listener can be disabled or stopped;
DAX does not require a network client. Builds without the optional WebSockets
component do not offer native DAX because they lack that shared map.

The adapter accepts owning pre-monitor slice frames, so speaker mute and
speaker gain do not change exported audio. Per-receiver squelch still gates the
slice tap. DAX and TCI gains and consumer lifetimes are independent. DAX stop
never stops TCI or tears down a channel held by another consumer.

The existing macOS shared-memory ABI is float32 stereo at 24 kHz. Native 24, 44.1
and 48 kHz are converted continuously to that format; their sample rates are
never relabeled. Linux's existing bridge deliberately downmixes that stereo
signal and emits float32 mono at 48 kHz. Linux DAX therefore does not provide
stereo WFM, while macOS DAX and stereo TCI retain separate left/right samples.
44.1 kHz is an internal Digital decoder rate; TCI negotiation remains 8/12/24/48 kHz.

Each route has independent converter history, an epoch witness, a replay
cursor, and a revocable receiver binding. The adapter has eight channel slots,
a 32-producer bound, output blocks at most 1024 stereo frames, and no PCM queue.
A live old producer cannot seize a newly created receiver's route. Epoch/rate
changes, discontinuities, forward gaps, removal, stop and backend replacement
reset conversion and retained bridge output. The Linux RX-only bridge buffers
at most 65536 bytes (~341 ms at mono 48 kHz) to accommodate decoder bursts; the
legacy packet bridge retains its smaller capacity. Writes stay nonblocking.
The macOS bridge retains its existing bounded live-edge clamp.

Receive-only startup creates only RX endpoints: no TX sink/shared-memory
segment or TX poller, no TX producer, and no mic-selection/DAX-TX command.
The TX control is dimmed with an accessible explanation. Native audio export
does not advertise radio-side DAX stream selectors or DAX-IQ support.

A reset can discard samples retained by AetherSDR or its source FIFO/ring.
Samples already accepted by an external audio client cannot be recalled.
The updated macOS HAL reader uses a cursor compare/exchange so a simultaneous
route reset cannot be overwritten by an older callback. Older installed
four-input drivers must be updated separately before all-eight device testing;
application compilation alone does not validate that installed driver.

Socket-free checks exercise eight sparse receivers with eight concurrent TCI
clients, independent rates and stereo signatures, mute, stop/restart, listener
stop, epoch changes, live-producer refusal, removal/recreate and callback
revocation. The Linux bridge test injects anonymous output pipes and exercises
all eight downmix/gain paths plus queue/interpolator reset. Hardware, operating
system devices, timing and installed-driver qualification are separate runs.
