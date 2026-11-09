---
title: "Kenwood TS-890S"
slug: "/kenwood-ts890"
description: "Connect a TS-890S directly over Ethernet in the experimental fork."
status: "Experimental, hardware validation pending"
applies_to: ["Kenwood TS-890S"]
---

:::info[Status]

**Status:** Experimental, hardware validation pending · **Applies to:** Kenwood TS-890S

:::

This development fork connects directly to the TS-890S over Ethernet. It uses
KNS for radio control and the radio's network audio. No USB cable or companion
server is required. Hardware validation is pending.
The radio's reported firmware version appears in **Radio Setup** after identity
and firmware readback.

This first contribution is experimental receive support. Native memories,
transverter operation and TX acceptance are outside its scope. Advanced
native auto-mode tables, scan/preferences, operating display/beeps, CW/PSK
decoder settings and detailed RX EQ/routing menus have software-test coverage
but remain hardware-unverified. Their availability does not imply that their
physical effect or restoration has been validated on every firmware version.

## Setup

Enable KNS network operation and built-in VoIP on the radio, and configure a
KNS account. Open **File → Connect to Radio... → Connect by IP** and choose
**Kenwood TS-890S (network, experimental)** under **Radio type:**.

Enter the radio's IP address or host name, **Network user:** and **Password:**.
Each credential must contain 1–32 printable ASCII characters without semicolons.
Select **KNS administrator account** only if that is the account you configured
on the radio. The default **TCP port:** is 60000. **UDP audio port:** is fixed at 60001,
as required by the radio. Transmit audio uses local UDP source port 60002;
custom UDP ports are not supported. A mapped TCP control port is supported.
Under **Radio variant:**, select the regional model shown on your radio:
**K (up to 60 MHz)** or **E/E2 (up to 74.8 MHz)**. **Unspecified (up to 60 MHz)**
uses the conservative receive range. All variants start at 30 kHz. This selection
sets the client tuning limits; it does not authorize transmission outside the
radio's permitted bands.
Click **Connect by IP**. Saved details load only into fields you have not edited.
Changing credentials, ports, variant or source path while address lookup is
pending cancels that attempt; select **Connect by IP** again to use your changes.
**Disconnect** also cancels a pending connection and stops automatic reconnection.

Passwords are session-only by default. Select **Save password in keychain** to
save one after a successful login. Saved passwords are scoped to the host,
TCP port, username and account role.
Connection requests use staged credentials only for the matching host and TCP
port; selecting another service on the same host does not reuse them.
A password saved for another connection is not loaded if a keychain write fails.
Without keychain support it remains in memory for this application session.
Radio frequency, mode and other radio-persisted settings are read from the radio.
After successful authentication, recent addresses restore the Kenwood radio
type, TCP port and chosen source path. Both a host name and its resolved IPv4 address
refer to that connection profile. Failed attempts do not replace it.

## Using the radio

The radio supplies one receive slice. VFO A and VFO B are tuning memories, not
two simultaneous receivers. Tuning and mode changes are adopted from the radio's
responses. To select VFOs and split, open **Settings → Radio Setup... → RADIO →
Native VFOs**. Choose **Receive VFO:** and **Transmit VFO:**; different VFOs select
split. Enter a frequency and select **Apply VFO A** or **Apply VFO B** to tune
that memory. The confirmed frequencies and split state appear below the controls.
VFO changes are unavailable while the radio reports transmitting.
The **Radio tuning** group on **Native VFOs** shows **Radio receive FINE**,
**Radio MHz step** and **Radio frequency lock**. These are radio controls;
**Radio receive FINE** does not change the application's **STEP**, and
**Radio frequency lock** is separate from the slice lock. The MHz step size is
configured on the radio and does not change the application's wheel step.
Select **Read tuning settings** to request fresh FINE, MHz-step and lock values.
Changes retain the last confirmed value until radio readback arrives. Unknown,
pending or unavailable controls remain dimmed with a reason. FINE changes here
apply only to receive tuning.
These experimental controls still require live-radio validation.

The **Native relative tuning** group provides explicit radio tuning steps. Select
**Read native step context** first. For a multiplier step, choose Up or Down and
×1, ×2, ×5, ×10, ×50 or ×100, then select **Apply multiplier step**. For counted
steps, choose a direction and select **Apply counted steps**. **Use radio
single-step form** requests one native step; clear it to enter an explicit count
from 0 to 99. An explicit zero is sent as zero, not changed to one.

Both actions target the receive-selected physical VFO. The radio determines the
step size and resulting frequency, which AetherSDR reads back without predicting
movement. Existing wheel, keyboard and client **STEP** behavior is unchanged.
Changing receive source, frequency, FINE, MHz-step or lock observations retires
old action drafts; Read again and choose the intended parameters. TF-SET,
unsupported memory sources and transmitting keep these controls unavailable
with a reason. Transverter operation is deferred: connect with the radio's
transverter function Off. An observed On state closes this experimental session
without changing the radio. **Cancel native read** cancels
local waiting; it does not undo a step or send a radio stop command. These native
actions have representative live step/effect and restoration checks; not every direction, multiplier and count has hardware qualification.

Receive mode and filter controls follow the selected receive VFO's physical
memory. Mode changes on the radio request fresh VFO state; changing the receive
VFO clears pending filter edits until that memory's mode is confirmed. Displayed
left/right positions are not used to identify VFO A or VFO B.
Native memory-channel operation is not mapped. If the radio enters memory
mode, **Native VFOs** labels that source **Memory channel (unmapped)** and dims
**Transmit VFO:**. Choose **Receive VFO:** **A** or **B** to return to physical
VFO operation; transmit selection recovers after radio readback. CW text and
TX-filter edits cannot use a previous VFO's state while the transmitter source
is unmapped. The A/B frequency readouts still describe those physical memories. Reconnecting
clears staged frequency entries. UTF-8 memory names in radio status do not
interrupt the connection, even though memory controls remain unmapped.
Each **Apply VFO** button remains dimmed until
that session reports the corresponding physical VFO frequency.
Typed frequency entries remain drafts when a later readback arrives, including
recovery after invalid status. Reconnecting clears those drafts. Native VFO,
tuning, filter and receive DSP displays keep the newer accepted readback when
older notifications arrive afterward. Invalid status dims affected controls
until valid readback recovers.

AGC and the native noise blanker are unavailable in FM and FM data mode.
Beat cancellation is available in SSB, AM, FM and their data modes; it stays
dimmed in CW, RTTY and PSK. These restrictions follow the confirmed receive mode.
NR1 remains available in FM. The radio stores its RF gain setting, but RF gain
has no effect while receiving FM.
When the radio selects NR2, the shared level slider is dimmed because that
slider represents NR1 level. Use **Native Receive DSP** for NR2's time constant.
Selecting NR1 restores its shared level control after readback.

Open **Settings → Radio Setup... → RADIO → Native Receive AGC** for the
radio's Slow, Mid and Fast timing presets and quick recovery settings.
Timing uses native indices **1–20**, from fastest to slowest; these are not
milliseconds. Quick recovery threshold uses native indices **1–10**, not dB.
Each preset has a confirmed value, temporary draft, **Apply** button and
separate reset to the radio's initial value. Changing one preset preserves
the other two confirmed values. **Read radio AGC** refreshes the observations.
**Restore previous radio AGC selection** is available while AGC is confirmed
Off; the resulting selection waits for radio readback. Preset and quick
recovery controls remain available with AGC Off, but are unavailable in FM
and FM data mode. These experimental controls still need live-radio and
audio-effect validation. Turning AGC Off can make received audio unexpectedly loud.
Changing the receive mode or VFO withdraws the old native AGC confirmation.
The page shows unknown values until current radio readback arrives; an older
pending update cannot restore the previous values.

Open **Settings → Radio Setup... → RADIO → Native Receive Routing** for
**Radio RX ANT input** and **External receiver antenna output**. These
switches retain the main ANT1/ANT2 and drive-output selections. RX ANT can
be turned off here without selecting a different main antenna. Values wait
for complete radio readback; an unknown value is unavailable rather than Off.
A frequency change withdraws the old routing confirmation and refreshes it.
**Read radio receive routing** requests current state again.
The radio stores these settings in its antenna memories. External receiver
output splits the received signal to ANT OUT and reduces receive gain by
approximately 3 dB. Physical input/output routing still needs live-radio validation.

Open **Settings → Radio Setup... → RADIO → Native Receive DSP** for the
radio's APF, manual notch, NR1/NR2, NB1/NB2 and beat cancellation settings.
The same page provides **RX monitor (open squelch)** and **DSP monitor (expand
passband)**. Choose **On** or **Off** explicitly; the selection follows radio
readback. Closing the page sends no monitor-release command. These controls
have no documented reset operation, so their **Reset** buttons remain disabled.
DSP monitoring temporarily expands the radio's IF/AF passband and forces APF
off. A monitor transition refreshes APF observations rather than inventing an
Off value. Configured filter values do not prove the effective monitored
passband edges. APF and filter projection remain unavailable until DSP monitor
is confirmed Off and fresh dependent settings arrive. Monitor effects and front-panel agreement still need live-radio
validation. These are receive monitors, separate from the transmit monitor.
Choose one setting at a time. Its displayed value stays at the last confirmed
radio value until readback arrives. These are separate from computer audio DSP.
Controls remain visible and dimmed when the mode, prerequisite or confirmed
state is unavailable; their description explains why. APF shift is shown in Hz
and NR2 time in milliseconds. Notch position and NB2 depth/width use the radio's
indices rather than an estimated physical unit. This experimental control path
still requires live-radio validation.
**Reset** requests the radio's documented default for that setting; the shown
value still waits for readback. Settings without a documented reset keep that
button dimmed. **Read settings** refreshes current values without changing them.
The **Audio peak filter** choices are **Off** and **On**, following the radio’s confirmed enable state.
The shared **APF** switch also follows confirmed native enable state. Its
shared 0–100 adjustment stays dimmed and its numeric readout shows `--` because the TS-890S uses separate
bandwidth, shift and gain settings; use **Native Receive DSP** for those.

The receive-mode selectors stay dimmed until the radio reports receive state
and TF-SET off. They are also unavailable during local keying or cleanup until
fresh receive readback arrives. The app does not change TF-SET or force receive
state when connecting. It reads an initial status snapshot to discover receive
or transmit operation without waiting for the operator to change state. A newer
RX/TX event or local transmit command supersedes that startup snapshot.

Ordinary RF power edits require confirmed physical transmit VFO mode and
frequency with normal antenna output selected. Requests above the documented
limit are refused: 100 W for HF/50 MHz non-AM, 25 W for AM, 50 W for 70 MHz
non-AM, and command value 13 for 70 MHz AM (representing 12.5 W on the radio).
AM data mode uses the AM limits. The radio's configured limiter may impose a
lower ceiling; the displayed value follows readback. Drive-output levels and
memory-channel transmit sources are not mapped to watt edits.

The radio supplies network audio at 16 kHz; AetherSDR resamples it for
its playback and decoder paths. Audio re-synchronizes when a sustained stream
restarts with new sequence numbering, while isolated duplicate or reordered
packets remain discarded.

Use the computer's microphone and speaker gain first. If native VoIP levels
need adjustment, open **Settings → Radio Setup... → RADIO → Network Audio Levels**.
**VoIP input (to radio):** and **VoIP output (from radio):** accept levels 0–100.
Enter a requested value and choose **Apply input level** or **Apply output level**.
The confirmed levels below update only after the radio responds. These are
radio-owned VoIP settings, separate from microphone gain. An administrator
login is required to change them; ordinary accounts can view confirmed levels
with the editing controls dimmed.
The output readback keeps the newer confirmed level if an older notification
or initial read result arrives afterward. Requested levels remain drafts during
readback updates; reconnecting starts a fresh draft. Invalid status dims editing
until valid radio readback recovers.

Open **Settings → Radio Setup... → RADIO → Native Power Limits** to read the
radio's per-band limits. Choose **Mode:** and **Band:**, then use **Read limit**
to refresh that configuration. Selection does not tune the radio. Enter a
**Requested limit:** from 5–100 W and select **Apply limit** to change only that
row. **TX tuning** has its own row, separate from ordinary RF power.
**Enable power limiter** controls the separate global limiter switch; both
controls wait for radio readback. These are configured ceilings, not measured
output or permission to transmit. Writes stay dimmed until the radio reports
receive operation and no local transmit action is pending. Handing off a stop
command does not satisfy that requirement. Configuring the TX-tuning row
does not start a TUNE carrier. Set the desired tuning power before starting
TUNE; power editing remains unavailable while a carrier or local transmit
intent is active.

The backend reads the TX-tuning row for the physical transmit VFO's documented
band separately from ordinary RF power. In split operation this follows the
transmit VFO. The address stays unavailable for transverter/drive output, memory
TX, unknown XIT offset, or an offset outside the same documented band. Reading
this setting does not key the radio or enable the global limiter.

For scope display, turn **EXPAND** off on the radio. In **Center** mode, use
the centered scope position. Fixed and auto-scroll coverage follows the radio’s
reported edges. The display uses relative signal levels; it is not calibrated
antenna dBm. The client does not change scope mode or EXPAND on connection.
To keep the waterfall window stationary while tuning, select **Fixed** in
the spectrum **Display** menu’s **Scope:** row. The VFO marker moves within
the radio’s reported window. **Center** follows the VFO on the radio itself;
**Auto-scroll** can move the window when tuning reaches its edge.
In Fixed mode, current displayed bounds are read separately from the selected
stored range. Moving or zooming that scope writes only stored ranges 1–3;
temporary range 0 remains read-only. Auto-scroll bounds are read without
changing a stored range.
In Center and Auto-scroll modes, zoom requests select a supported discrete
span in the requested direction. The display waits for radio readback.
The native spans are 5, 10, 20, 30, 50, 100, 200 and 500 kHz. Their command
indices were measured on firmware 1.01 in USB/voice and through the application
in CW. Other firmware and untested mode banks remain unverified.

The VFO meter and **S-Meter** applet show the radio’s native **dots** display deflection on a
0–70 scale. This is an uncalibrated reading, not dBm or an S-unit conversion.
Stale or unavailable samples park the meter. Calibrated external meter output
and SmartMTR signal scales remain unavailable for this raw reading.

SSB and CW adjustments in the shared passband controls use the radio's
documented steps. SSB/data filter adjustment becomes
available after the radio reports its filter style. Presets outside the native
passband limits are also dimmed. The client waits for radio responses before
updating live controls.
Rapid frequency-wheel steps accumulate as pending input while radio readback
is delayed. The live frequency display still follows radio reports; disconnect
or a refused tuning request retires the pending input.

A receive-antenna request retains the last confirmed selection until radio
readback. Requests require a known receiving context with TF-SET off and no
local transmit intent. **ANT1** and **ANT2** select the shared main RX/TX antenna
and disable the dedicated receive input. **RX-ANT** selects that receive input
while preserving the main antenna, drive-output and external-receiver-output
settings. These antenna controls still require live-radio validation.

Open **Settings → Radio Setup... → RADIO → Native Auto Mode** for the radio's
automatic mode policy. **Read auto-mode settings** reads the enabled state,
division count and 32 indexed upper-frequency boundaries. Stored mode labels
describe the table's policy; the live operating mode still follows the radio.
The survey contains observations from different times and is not an atomic
backup while another operator changes settings.

**Auto mode enabled draft** prepares On or Off; **Apply auto-mode enabled**
sends that request. Enter a new upper boundary in **Hz**, choose a stored mode
and use **Register new division** to add it. A complete current table with fewer
than 32 entries is required. An existing boundary is refused because its
replacement behavior is unqualified. Select a populated row and use
**Delete selected division** to remove that entry. Selection alone sends no
write. The radio shifts following divisions; AetherSDR waits for fresh reads
rather than moving rows optimistically.

These actions change shared radio settings. They require confirmed receive
state, TF-SET off and current configuration-bank observations. A stale or
incomplete snapshot disables table changes with a reason. If the reported count
and observed rows disagree, those observations remain visible and table changes
stay disabled; read again for a fresh survey. An accepted request
remains unverified until subsequent observations are consistent; those reads
do not prove a transaction or exclude another operator's edits. **Cancel reads**
stops local verification and cannot undo an already-sent change. No failed
request is automatically retried or restored. Auto-mode controls still need
live-radio effect and restoration validation.

Open **Settings → Radio Setup... → RADIO → Native Transverter** for read-only
native status and raw frequency observations. Select **Controls**,
**Frequency companions** or **All state**, then **Read transverter state**.
Opening the page uses cached observations. **Apply transverter function** and
**Apply native offset** remain dimmed with a reason because transverter changes
and RF coordinate projection are deferred in this experimental RX build.
**Preview offset** performs client arithmetic only; it is not a confirmed RF
frequency, a tuning conversion or a receiver capability.

Connect with the radio's transverter function Off. Ordinary tuning and scope
coordinates wait for confirmed Off status. If the radio reports On, AetherSDR
closes the session with an explanation. Turn the function Off on the radio
before reconnecting; AetherSDR never changes it automatically.
**Cancel reads** cancels local waiting without sending an inverse or a default.

Open **Settings → Radio Setup... → RADIO → Native CW Receive** for the
radio's own Morse decoder. **Read native CW controls** reads the communication
screen, threshold, decode filter, decoding state and supporting receive status.
Opening the page uses cached observations. These controls are separate from
AetherSDR's host audio decoder.

**Last reported CW auto zero-in** shows the radio's last activity report.
**Read native CW controls** refreshes it along with the decoder settings.
**Unknown** means no current qualified observation is available; a retained last
observation is not live confirmation. **Inactive** does not prove successful
alignment. Native auto zero-in can change frequency; its **Start** and **Stop**
actions are not implemented, and opening or closing this page never starts,
stops or restores it. This observation is separate from antenna tuning and
host audio decoding.

**CW communication screen** selects **Closed** or **Open**; **Native CW
decoding** selects **Stopped** or **Decode**. Opening the screen and enabling
decoding are separate actions. Their positive actions require CW or CW-R;
closing or stopping does not require that mode. **Native decode threshold** is
an index from 1 to 30, not dB. **Native decode filter** offers **Off**, **Normal**
and **Narrow** for the decoder. Choose a draft and use its row's **Apply**.
Actions require current readback and confirmed receive state without local
transmit intent. Confirmed SWL disables decode enablement. No action changes
the operating mode or navigates the native screen automatically.

The read-only history shows last received native characters, underscores for
undecodable input and the radio's eight supported prosigns. It retains up to
1,024 events and reports dropped or invalid events. Earlier text remains when
receive conditions change; it is not evidence that decoding is currently
active. **Clear local history** sends no radio command and remains available
after disconnect while the same backend cache is retained. Closing this page
does not stop the radio's decoder.

The native character stream uses the radio's automatic information output;
reading the page does not enable that output. During native CW decoding,
radio NR1, NR2, APF and receive DSP EQ do not function. AetherSDR preserves their
reported settings and its separate host DSP. Requests remain unverified until
readback is consistent; cancellation cannot undo a sent change. Native decode
effects, firmware-specific character framing and restoration still require
live-radio validation.

Open **Settings → Radio Setup... → RADIO → Native Operating Display** for
native display controls. **Read operating display** reads the configuration
bank, transverter state, basic display, function screen and edit state.
Opening this page uses cached observations and sends no radio commands.
Unlisted function-screen IDs remain visible as numbers rather than receiving
an assumed meaning.

Choose **Standard** or **SWL** in **Basic radio display draft**, then select
**Apply basic display**. These actions require confirmed receive state and no
local transmit intent. Entering SWL also requires a confirmed transverter Off
state; returning to Standard does not. Function and edit screens are separate
diagnostics and do not impose a blanket ban on changing the basic display.
SWL changes the physical radio display and restricts several native functions;
the page shows the radio's restrictions.

**Send radio ESC** explicitly sends the radio's ESC action and may discard an
edit. It requires current function-screen and edit-state observations. Closing
the dialog and **Cancel reads** do not send ESC. After either action AetherSDR
reads the basic display, function screen and edit state again. Matching basic
display observations are reported as consistent, rather than verified. ESC
remains unverified because its result depends on the radio's current screen;
AetherSDR does not assume it returns every nested screen to zero. Canceling
reads cannot undo a sent action, and actions are never automatically retried
or restored. These controls still need live-radio effect and restoration
validation.

Open **Settings → Radio Setup... → RADIO → Native VFO Scan** to read the
radio's scan state. **Read VFO scan state** reads scan controls, receive-source
qualification and the native Tone/CTCSS companion settings. Each row has its own
**Apply** button; editing a choice alone sends no command. Confirmed values stay
separate from drafts until radio readback.

Select **VFO** under **Scan selection** before applying **On** under **VFO scan**.
Applying **Off** explicitly stops a qualified VFO scan. **Program** remains
visible but unavailable because native memory-dependent scan is unsupported.
Scan speed uses native indices **1–9**, available in non-FM modes; these indices
are not measured time or frequency units. Tone scan offers **Off**, **Tone** and
**CTCSS** in standard FM. Data-FM tone scanning remains unqualified. During
TF-SET, scan selection remains available, but scan target and effective-mode
qualification for the other controls is still pending.

Tone/CTCSS scanning automatically enables the corresponding native tone
function. The page shows its observed type and native frequency indices; it does
not write or automatically restore those companion settings. **Cancel reads**
stops local reads or verification; it does not stop a scan already started on the
radio. A canceled, refused or timed-out operation is never automatically retried,
stopped or reversed. Matching observations show consistency, not a correlated
radio acknowledgement. These controls still need live-radio effect and
restoration validation.

Open **Settings → Radio Setup... → RADIO → Native Scan Preferences** for the
radio's persistent **Scan Hold** and **Scan Resume** settings. **Read scan
preferences** reads the current configuration bank and both native values.
**Apply** stores the selected value in that bank. Editing a choice alone sends
no command; confirmed values change only after radio observations.

Scan Hold applies to non-FM scanning: **On** lets a tuning adjustment hold
reception for five seconds. Scan Resume applies to FM scanning: **Time-operated**
uses the radio's timed pause, while **Carrier-operated** waits until the signal
has been absent for two seconds. These are native preferences, not AetherSDR
pause timers. Both preferences can be edited while their mode is inactive.

Each **Reset to radio initial value** button requests the initial value for
that single native menu setting. It does not select an AetherSDR default or
reset the radio. The page reads the actual value afterward; it does not claim
that readback proves the reset caused it. Writes require a known current bank
and current row observation. A bank change retires old drafts and requires a
new read. The page never switches banks or restores preferences automatically.
**Cancel reads** cancels local verification without undoing a stored operation.
Persistent-write effects and restoration still need live-radio validation.

Native memory support is unavailable in this TS-890S build. **Settings → Radio
Setup... → RADIO → Native Memories** remains visible with disabled controls and
a reason. Normal, program, extended and quick-memory reading, recall and editing
are unsupported. The page sends no memory commands.

Open **Settings → Radio Setup... → RADIO → Native PSK Receive** for the
radio's **PSK AFC range** preference. **Read PSK receive settings** reads the active
CONFIG bank and its native range. Choose **±8 Hz** or **±15 Hz**, then **Apply**
to change that bank's stored preference. Changing banks retires the old draft;
read the new bank and select the choice again before applying it.

**Reset to radio initial value** asks the radio for its initial range. The last
observed value remains visible while readback is pending. A subsequent range
observation is displayed, but does not prove the reset caused it or that it is
the initial value. The page never selects a bank, activates AFC or NET, starts
a decoder, or restores a preference automatically. Range effects and persistent
write/restoration behavior still need live-radio validation. This page controls
the native range preference; it does not represent full native PSK decoding.

Open **Settings → Radio Setup... → RADIO → Native Receiver Options** for
**Physical speaker mute**, **Receiver signal blanking** and the last observed
**BUSY LED** state, plus **Last reported native recording/playback**. Use
**Read receiver options** to request these four native observations. Reading does not require a selected VFO, receive mode, TF-SET state or
configuration bank. Opening the page uses cached observations only.

Native recording/playback is read-only. The page reports the last received native
state and playback progression; it does not start, stop, pause or resume anything.
Pause/resume status codes retain their native ambiguity. Zero progression means
no playback progression was reported, not completed playback. Reports are discrete;
silence does not mean stopped, and failures are only shown when actually received.
Malformed or failed reads show Unknown while retaining the last observation. No
recording/playback actions, automatic polling or restoration are performed.

The two editable options have separate **On**/**Off** drafts and **Apply**
buttons. Applying requires confirmed receive state, no local transmit intent
and the current field observation. The physical-speaker option is separate
from ordinary mute and remote-administration speaker policy. Receiver signal
blanking also blanks the received IF signal; it is separate from noise
blankers. The radio does not automatically report physical-speaker mute through
AI, so AetherSDR explicitly reads it back after a request. Values remain last
observed samples, not proof that a request caused the reported state.

BUSY is a read-only radio LED observation, not a client squelch or scan policy.
After 1.5 seconds AetherSDR marks its sample unknown while retaining the last
observed value. This is a client freshness limit, not a Kenwood protocol limit.
Read again to refresh it; no automatic polling is started. **Cancel reads**
cancels local verification and cannot undo a change already sent. Closing the
page sends no setting change. No default, reset or automatic restoration is
applied. Speaker mute is cancelled by radio power-off; signal blanking is not
backed up by the radio. Physical speaker/IF effects and network-audio behavior
still need live-radio validation.

Open **Settings → Radio Setup... → RADIO → Native Radio Messages** to read
the radio's two message sources and beep volume. **Read radio messages**
refreshes both sources without dismissing or confirming a message. Each source
keeps its own message ID and description. Source 2 takes priority when its
confirmed ID is nonzero; priority remains unknown until the relevant source
has reported. Unrecognized IDs remain visible with their number.

Beep volume uses native indices **0–20**, with **0** meaning Off. Editing the
field creates a temporary draft. **Apply beep volume** stores the requested
value in the radio; **Reset beep volume to radio initial value** requests the
radio's initial setting. Both wait for actual readback, and an unverified
request leaves the last confirmed value visible. Use **Read radio messages**
for fresh readback before retrying. These controls require confirmed receive
state. Beep changes are separate from PC audio volume and are never replayed
on reconnect. Native message and beep controls still need live-radio validation.

Open **Settings → Radio Setup... → RADIO → Native Scope** for the radio's
scope settings. **Read radio scope** requests fresh values. Each **Apply** sends
a request; the confirmed value changes only on radio readback. Unknown,
pending and unsupported controls stay visible and dimmed with a reason. The
page includes the physical display, RF presentation, audio-scope/oscilloscope
settings and Ethernet spectrum output. These are separate from AetherSDR's
own waterfall and host audio processing controls.

**Bandscope output** offers Off and the three LAN cycle choices. **Filter
spectrum output** offers Off and LAN. COM/USB choices remain visible and
unavailable for this Ethernet connection. Opening the page does not enable an
output or change the radio display. The span choices retain the eight-entry
map qualified on firmware 1.01 in USB operating mode; other firmware/mode banks
remain untested. **Reset span** requests the radio's initial value rather than
selecting an extra span choice.

**Stored fixed range** entries require both endpoints in Hz. **Apply both
endpoints** and **Reset stored range** target that stored slot. Current scope
bounds are shown separately and never copied into an unobserved stored slot.
Inactive stored-range read syntax is not established; a sent request can remain
unverified without a tagged reply. The page does not switch range selection to
inspect a slot. Before changing a persistent range, record values you can
restore. **Clear scope** clears the radio's waterfall; **Center marker** uses
the radio's Fixed/Auto-scroll action. A sent action is not a verified outcome.

The native AF plot shows 213 sample positions with a relative 0 to −50 dB
vertical scale. It has no calibrated frequency or antenna dBm axis. Fresh data
requires confirmed AI and LAN filter output; the radio suppresses this stream
while its Audio Scope display is selected. Missing or stale data becomes
unknown without enabling outputs automatically. Unsupported shifted/expanded
RF geometry withdraws AetherSDR's projection with a reason instead of drawing
an assumed centered span. These experimental controls and stream conditions
still require live-radio validation.

For native settings, open **Settings → Radio Setup... → RADIO → Native Filters**.
AM/FM and their data modes offer **Low cut:** and **High cut:** in audio Hz.
RTTY (FSK) and PSK offer **Passband width:**; their second control stays dimmed. SSB/data
uses either cuts or **Passband width:** and **Shift:** according to the radio's
filter style. CW uses width and shift. Each selection changes only that setting;
the displayed selection returns to its confirmed value until the radio responds.
Changing the receive VFO or mode retires pending settings from the previous
context. Controls are unavailable during reported transmission. AM/FM audio cuts
and FSK/PSK widths do not change the shared RF passband display.

The **FM Normal/Narrow** group reads the radio's separate FM/FM-D setting.
Opening the page does not request or change that setting. Select **Read FM width**
to read the current display roles and native values. The right display is read
only when split or TF-SET makes it applicable. Unknown observations remain
**Unknown**; the display labels are not fixed VFO A/B labels.

Choose **Normal** or **Narrow**, then select **Apply FM width** to change the
active receive context. Apply requires a fresh owned read, confirmed physical
receive VFO, FM/FM-D, RX and TF-SET off, with no pending transmit or tune intent.
These are not audio-cut values, measured passband widths or deviation readings.
The selected mode may also be used for later transmission. A changed context
retires the draft: read again and choose the intended value again. **Cancel read**
stops only the pending read; it does not undo a setting. There is no automatic
retry, reset or restoration. **sent_unverified** means the native command was
handed to transport; **readback_consistent** means a later observation agrees,
not that the physical effect has been verified.

The **Radio filter banks** group selects **Active receive filter:** A, B or C.
Each bank has **roofing**, **IF shape** and **AF type** settings. **Auto** is a
roofing selection; the separate **actual roofing bandwidth** readout shows the
radio's chosen bandwidth. **270 Hz** requires confirmed optional-filter
installation. Filter C requires the radio's three-filter configuration; the app
reads that configuration and does not change it.

Select **Read settings** to request current bank settings. Values remain at their
last confirmed readback while a request is pending. Changing the active bank or
its settings temporarily disables passband edits until fresh bank and native
filter values arrive. Bank and filter-style changes preserve pending frequency
steps and unrelated receive controls. Editing another bank preserves the active
passband.
In FM/FM data, roofing and IF shape remain read-only; AF type is separate.
**Reset** asks for the radio's initial value for that individual bank setting,
then waits for normal readback instead of assuming a default. Unknown,
unsupported or unavailable choices remain visible and dimmed with reasons.
These new bank controls still require live-radio validation.

Open **Settings → Radio Setup... → RADIO → Native Receive EQ** for the radio's
receive equalizer. **Radio receive EQ enabled** and **Selected effect:** show
confirmed radio values. Selecting an effect can enable EQ on the radio; both
values wait for readback. This page is separate from host audio EQ.

The curve has 18 bands from 0 to 5100 Hz in 300 Hz steps. **Confirmed** shows
the radio's curve; **Draft** accepts temporary values from −24 to +6 dB.
**Apply curve to radio** stores all 18 draft values in the selected radio effect.
Changing the receive source, mode, selected effect or confirmed curve retires an
older draft. Drafts are never saved or replayed on connection.
After a receive-source or mode change, the enable state, effect and curve
remain unknown until current readback. Older updates cannot repopulate them.

**Reset selected bands to radio initial values** and **Reset all bands to radio
initial values** request the radio's own initial settings; they do not assume a
flat curve. Apply/reset can change stored named presets as well as User effects.
Stored effects may also be used by other receive modes.

**Copy selected effect to destination** overwrites the chosen User 1–3 effect.
A sent request remains unverified. **Select copied destination effect** changes
the active effect; then use **Read radio EQ** and **Inspect copied destination**
to compare its observed curve with the captured source. A difference can reflect
concurrent changes and does not establish whether the copy failed. Returning to
the previous effect requires an explicit **Selected effect:** choice.
These new native EQ controls remain experimental and require live-radio
validation; stored-curve testing requires an agreed scratch preset and restoration.

The PHONE applet's **Low Cut** and **High Cut** controls adjust the selected
transmit VFO's filter in SSB, AM and their data modes. Normal and data modes
have separate radio-owned settings. Low cut supports 10, 100, 200, 300, 400 and
500 Hz; high cut supports 2500, 2600, 2700, 2800, 2900, 3000, 3500 and 4000 Hz.
The displayed values change after radio readback. Controls stay dimmed with a
reason until both edges and the physical transmit mode are known, and in modes
without a TX filter (CW, FM, FSK and PSK). Connecting does not restore a saved
filter. In split operation, these controls follow the transmit VFO.

The P/CW CW face's **Sidetone** switch controls the radio's native sidetone
setting after its current value is read. Turning it off selects native Off.
Turning it on from Off selects the monitor-linked volume option documented by
Kenwood's PC command guide; a fixed native volume that is already on is preserved.
The switch follows radio readback. Its adjacent percentage volume controls remain
dimmed; no fixed-level-to-percentage mapping is implemented. The radio's sidetone
audibility through KNS and PC playback has not been verified on hardware.

The P/CW **MON** switch controls the radio's transmit monitor. Its level uses
20 native steps, shown as 0–100 in steps of 5. CW **Delay:** supports 0–1000 ms
in 50 ms steps. Speed, pitch, squeeze mode A/B and paddle reversal use the
radio's own keyer settings; changes do not alter paddle-port configuration.
Choose A/B squeeze mode and **Swap: Dot/Dash** under **Settings → Radio Setup...
→ SIGNAL → Phone & CW**. The separate Iambic enable switch remains unmapped. **CWU** and **CWL** set
the native CW BFO sideband to USB or LSB after its current value is reported.
Choose **CW** or **CWR** in the slice mode selector for normal or reverse CW.

Power, gain, monitor and mapped keyer settings remain at their last observed
values until the radio answers. Unmapped sidetone, pan, accessory and profile
controls stay dimmed with a reason. Client **Hold Delay** is unavailable for this
native keyer; speed changes do not replay a remembered break-in delay.

CW text requires the radio to confirm **CW** or **CWR** on the selected transmit
VFO. In split operation, the receive VFO's mode does not qualify the text keyer.
Changing the transmit VFO or either displayed mode clears that qualification
until fresh physical VFO readback arrives. Physical VFO modes refresh every five
seconds because Kenwood does not report them automatically through `SF`.

The native CW keyer accepts a queued message without reporting a qualified
end-of-message event. Handing the text to the radio does not cancel it. The local
CW operation remains owned until explicit cancellation, loss of authority or
session teardown. Press **Escape** in CWX to cancel the queued tail and release
that local operation before switching owners or starting TUNE. Neither a receive
gap between CW elements nor buffer-space availability is treated as proof that
the whole message has drained.

When the radio reports an ATU cycle in progress and then stopped, AetherSDR
retires that cycle's local activity. A stopped cycle is not labelled a successful
match, and physical receive state still requires its own radio readback.

Finite generated audio drains its converter tail before its local release
allowance ends. Output remains bounded and tied to the original transmit
operation; cancellation discards queued audio. The network queue accommodates
the generated-audio producer's initial buffer. A stalled output pump discards
pending audio rather than replaying it in a burst.

Transmit code is experimental and has not been validated with hardware. PTT
requires the radio to confirm both the account's transmit permission and its
enabled state. Selecting PC audio explicitly selects the radio's LAN input;
connecting alone does not change the transmit source. Keep initial hardware
validation receive-only.

To choose the radio's transmit input combinations, open
**Settings → Radio Setup... → RADIO → Native Transmit Inputs**. **SEND/PTT input:**
and **DATA SEND input:** are independent. Each offers **Microphone**, **ACC 2**,
**USB Audio**, **LAN**, or a microphone plus one rear input. Select a complete
combination while the radio confirms receive operation. Each selector waits for
its own readback and remains dimmed when its selection is unknown, during
transmission, or while a local transmit intent is active. Selecting an input does
not key the transmitter. The radio owns these settings; connecting never restores
a remembered selection.

Turning **PC Audio** on selects the radio-persisted pure **LAN** choice on
**DATA SEND input:** for host PCM. Turning it off does not restore the previous
DATA SEND selection. This is a radio routing change, including during receive.
Configuring LAN
on SEND/PTT or mixing microphone with LAN does not enable an additional host PCM
route. After an input edit, keying on that path waits for fresh radio readback.

**TUNE** requests the radio's native CW carrier. It requires confirmed receive
operation, transmit permission, a physical TX VFO, normal output with the
transverter off, known XIT, and native tuning-power and limiter readbacks. The
client refreshes that target, then confirms the separate TX-tuning power row
before sending the carrier request. If the requested setting differs, it updates
that radio-owned row once and waits for exact confirmation. Ordinary RF power
and the global limiter switch are preserved. These are configured ceilings, not
measurements of actual output.

Preflight has a three-second deadline. Release TUNE to cancel a pending request;
late replies cannot restart it. Rejection, timeout or changed target ends the
matching local intent. An active target or tuning-power change requests receive
operation; a sent stop does not prove the transmitter is idle. Network failure
does not guarantee RF stopped. Check the radio after control failure. Actual
carrier power, limiter interaction and RF stop behavior remain unverified on
hardware. Set power before starting TUNE; live power edits are unavailable.

Turning PC audio off preserves the radio's ordinary SEND/PTT input selection.
Changing PC-audio intent or the radio's DATA SEND source discards queued network
audio; selecting LAN again accepts only newly submitted audio.

Receive integrations preserve the radio's native mode identity. TCI advertises
lowercase native names, including `cwr`, `rttyr`, `psk`, `pskr` and `dam`.
A TCI client must understand the advertised name to use it. Hamlib/rigctld uses
`CWR`, `RTTYR`, `PSK`, `PSKR`, `PKTAM` and `PKTFM` for the corresponding modes.
The SmartCAT compatibility dialect has fewer mode codes; an unrepresentable
mode returns `?;` instead of reporting USB. This also applies to composite CAT
status replies that require an unrepresentable mode code.

In rigctld, select a native mode with passband `0` to preserve its radio filter.
A request combining a different mode and a positive width is currently refused
until that filter bank is confirmed. Positive shared-width requests are also
unavailable for AM/AM data, FM/FM data, RTTY/reverse RTTY and PSK/reverse PSK;
use the native filter controls where available. These are remaining integration
gaps and have not been accepted as complete RX parity.

## Known issues

- Live TUNE-power editing, calibrated meters and additional RF passband mappings
  remain incomplete ([#1](https://github.com/s53zo/AetherSDR/issues/1)).
- Expanded and shifted-center scope coverage are not yet mapped; scope frames
  are withheld in those modes rather than displayed at an incorrect frequency
  ([#1](https://github.com/s53zo/AetherSDR/issues/1)).
- TS-890S hardware validation is pending
  ([#1](https://github.com/s53zo/AetherSDR/issues/1)).

## Troubleshooting

- **Login refused:** check the account, password and administrator selection;
  disconnect other applications using the radio's KNS session.
- **Connected but no VFO or scope state:** confirm the radio's main power is on,
  then reconnect. The KNS network service can accept a login while the radio is
  in standby; connecting does not power it on automatically.
- **Timer operation restricts remote control:** let the radio's timer operation
  finish, then reconnect. AetherSDR closes that session rather than continuing
  normal polling or changing a timer setting.
- **Radio hostname lookup timed out:** name resolution has a 10-second limit.
  Enter the radio's IPv4 address directly, or check DNS and try again. A late
  lookup answer cannot start a connection after timeout or replace a newer attempt.
- **Cannot bind audio port:** close another application using UDP port 60001, or
  check the chosen network interface.
- **Rejected command warning:** check the radio's current mode and operating
  state before trying the control again. AetherSDR retains the last confirmed
  value; the radio's refusal does not identify which command it rejected.
- **Communication error or receive buffer overrun:** the connection closes and
  local audio producers stop. Check the radio and network before reconnecting.
  A closed connection does not confirm that the transmitter stopped.
- **No scope:** use Center mode, disable EXPAND and confirm the radio's scope is
  enabled.

## See also

- [Supported Radios](./supported-radios.md)
- [TCI Server](./tci-server.md)
