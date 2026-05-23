# Profile Import/Export Manual Integration Checklist

Use this checklist with real FlexRadio hardware before release because the
database package transfer is radio-side behavior.

- Export profiles-only from a FLEX-6000 v4.x radio and verify the resulting
  `.ssdr_cfg` imports in SmartSDR for Windows.
- Export a `.ssdr_cfg` package from SmartSDR for Windows and import it into
  AetherSDR.
- Import a package containing same-name profiles and confirm the destructive
  replacement warning appears before upload.
- Export on older firmware if available, especially v2.x and v3.x.
- Cancel an active export and confirm no partial `.ssdr_cfg` is committed.
- Force a failed import upload and confirm the UI reports failure rather than
  success, then refreshes profile lists when the radio recovers.
- Try SmartLink/WAN and confirm unsupported transfer directions are disabled
  with a clear explanation.
