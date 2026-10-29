# Issue #6723 widget evidence

These are grabs of the production TunerApplet forward-power gauge in an
isolated macOS/Qt 6.12 widget harness. Input is a constructed 500 W radio
with KXPA100 selected for shared meters, and a 400 W tuner reading.

Before: the existing selection-only input gives gaugeMax=200 and
gaugeFraction=1. After: tunerMaxWatts gives gaugeMax=600 and
gaugeFraction=0.666667. Both retain selection.maxWatts=100.

This is widget-level evidence, not a full application, automation-bridge,
firmware or RF measurement. The harness used compiled theme defaults.
Source fix: 1663efd7bce2c31391001d1f0e51be0c1dcc5b54.
