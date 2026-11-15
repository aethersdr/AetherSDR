# AetherSDR patches to ggmorse

The source snapshot is pinned to ggerganov/ggmorse commit
`8fb433d6cd6a71940f51b5724663ec0c75bf0b62` (see `COMMIT`). AetherSDR carries
three local changes to the otherwise exact upstream snapshot:

1. `src/ggmorse.cpp`: Æ, Ø and Å added to the Morse alphabet as Latin-1
   `0xC6`, `0xD8` and `0xC5` — Æ `·−·−` (also Ä), Ø `−−−·` (also Ö),
   Å `·−−·−` (ITU-R M.1677-1). They are in regular use on Nordic bands and
   upstream decodes none of them (#1280, PR #1318). PR #1318 first added Å as
   `·−−−·`; PR #2270 corrected it (#2264).
2. `include/ggmorse/ggmorse.h` + `src/ggmorse.cpp`: `speedRangeMin_wpm` and
   `speedRangeMax_wpm` in `ParametersDecode` bound the coarse speed search
   (`-1` keeps upstream's 5–55 WPM), so the CW panel's WPM range reaches the
   decoder (`CwDecoder` sets them from the slider; PR #3330). To go with it the
   coarse pass steps 2 WPM (upstream 10), every speed is capped at 115 WPM, a
   fixed `speed_wpm` is accepted below 200 (upstream 100), and `intervalsAll`
   grows from 100 to 120 to hold the larger speed index. Upstream's own
   `&& s < 55` loop bound is deleted so the raised clamps are reachable.
   The comment above the coarse search states those limits (#6669 corrected it from "5..115").
3. `src/ggmorse.cpp`: the decode loop no longer writes decoded characters,
   `?` and word spacing to stdout (`printf` / `fflush(stdout)`); `rxData` is
   the output, and the stdout copy filled CI logs with test Morse (#5745).

## When refreshing ggmorse

Check whether upstream has gained any of these changes; drop any it now has.
Otherwise reapply them, keeping every other file exact, and update `COMMIT`.
