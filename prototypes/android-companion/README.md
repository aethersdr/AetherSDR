# Android companion spike (mobile thin client)

Throwaway prototype de-risking an **Android companion app** (remote head:
discovery, connect, tune, RX audio, spectrum) before the aetherd RFC's
thin-client step lands. Lives in `prototypes/` on purpose — same pattern
as `prototypes/hl2`: an in-tree mobile client is architecture ahead of
the RFC (per `AGENTS.md`, maintainer-only), so the unknowns get proven
cheaply and disposably first. Self-contained: own CMake project, **does
not link `aethercore`**, root build untouched.

## Unknowns this spike proves

| Phase | Proves |
|---|---|
| 1 | Qt 6.11 for Android builds + deploys a C++20 Qt Quick APK |
| 2 | SmartSDR UDP :4992 discovery broadcast reception on Android WiFi (MulticastLock) |
| 3 | TCP :4992 command channel + slice tune from touch UI |
| 4 | RX audio via Qt Multimedia (AAudio) at usable latency |
| 5 | FFT packet ingest → drag-to-tune spectrum strip in Quick |
| 6 | Foreground service; RX survives screen lock |

Phases 1–2 are in this tree. Protocol facts mirror
`src/core/RadioDiscovery.{h,cpp}` (same project — no clean-room needed);
the spike code itself is original and minimal.

## Build (macOS host)

Prereqs: JDK 21, Android SDK (platform 35, build-tools 35, NDK 27.2),
Qt 6.11.1 `android_arm64_v8a` + macOS host kit installed via `aqt` under
`~/Qt`.

```bash
export ANDROID_SDK_ROOT=/usr/local/share/android-commandlinetools
~/Qt/6.11.1/android_arm64_v8a/bin/qt-cmake -S . -B build -G Ninja \
  -DQT_HOST_PATH=$HOME/Qt/6.11.1/macos \
  -DANDROID_SDK_ROOT=$ANDROID_SDK_ROOT \
  -DANDROID_NDK_ROOT=$ANDROID_SDK_ROOT/ndk/27.2.12479018
cmake --build build --target apk
adb install build/android-build/build/outputs/apk/debug/android-build-debug.apk
```

Phone and radio must share a WiFi/LAN segment; the discovery list
populates from live broadcasts within ~1 s of a radio being present.

## Emulator validation (no phone / no radio)

Validated 2026-07-24 on the x86_64 emulator: build the
`android_x86_64` kit variant into `build-x86` (same configure line,
swap the kit path), then:

```bash
avdmanager create avd -n spike -k "system-images;android-35;google_apis;x86_64" -d pixel_7
emulator -avd spike -no-window -no-audio &
adb install -r build-x86/android-build/build/outputs/apk/debug/android-build-debug.apk
adb emu 'redir add udp:14992:4992'   # host 14992 → guest 4992
adb shell am start -n org.aethersdr.companion/org.qtproject.qt.android.bindings.QtActivity
```

Send synthetic discovery datagrams to `127.0.0.1:14992` (key=value
payload per `RadioDiscovery.cpp`) and the radio card appears. Host port
is 14992 because a running desktop AetherSDR already owns UDP 4992.

Emulator proves parse + model + UI only. Still phone-only: real WiFi
broadcast delivery / MulticastLock behavior (spike phase 2 sign-off)
— untested until Android hardware is available.
