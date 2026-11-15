# Radio DSP parameter reset

Authority: published FlexLib API **4.3.16.42034**, `FlexLib/Slice.cs`,
`SendDspControlsResetCommand` (lines 1965–1971). The backend emits exactly
`slice reset <owned-id> dsp_param=<token>` from this fixed whitelist:

| UI target | Wire token |
| --- | --- |
| All | `all` |
| NR | `nr` |
| ANF | `anf` |
| NRL | `lms_nr` |
| ANFL | `lms_anf` |
| NRF | `nrf` |
| NRS | `speex_nr` |
| ANFT | `anft` |

The [official DSP Controls Panel guide](https://helpdesk.flexradio.com/hc/en-us/articles/56306070856475-How-to-Use-the-DSP-Controls-Panel)
defines All as every DSP block's parameter reset, including NB/APF levels,
with enable states retained. It is one Basic operation, not a loop over
individual targets. NRS has only a Basic level parameter. Individual NR,
ANF, NRL, ANFL, NRF and ANFT resets belong to advanced settings. NRS/NRF
require the newer hardware platforms. FM modes exclude the panel; notch
algorithms also exclude CW, RTTY, digital and FreeDV modes.

The guide names the entitlement **Advanced Noise Mitigation**.
`FeatureLicense.cs:219–223` recognizes `noise_reduction`, and observed radio
status reports `NOISE_REDUCTION enabled=1 reason=PLUS`. Mapping that explicit
feature to advanced reset access is an inference from these sources; no
published source inspected explicitly equates the two names. Unknown or
false feature status refuses advanced resets. Enabled feature status is
accepted regardless of subscription expiry or reason, preserving retained
entitlement. Parameter-status presence never establishes entitlement.

Support is conservative: completely parsed numeric firmware **4.3.16+**, a
known platform, current exact slice object, confirmed owned id, supported
radio-confirmed mode matching the displayed mode, and the relevant entitlement. Both backend replacement and disconnect
retire the session token. The VFO captures radio, slice, target and session
on button press and consumes that context synchronously on click. A fresh
press after reclaim works; an old press cannot cross reconnect or rebinding.
Selection and status never dispatch. No reset value is guessed or persisted.

`dsp_parameter_reset_test` injects production model status and Flex command
sinks without connecting to firmware. Its default CTest opens no listener.
The optional `--bridge-fixture` mode starts the real AutomationServer at
`aethersdr-dsp-reset-fixture`; fixture-only controls inject status, license
and lifecycle changes separately from Reset. This demonstrates client
routing and reconciliation, not acceptance or defaults on physical hardware.
The fixture never applies synthetic defaults in the reset callback.
