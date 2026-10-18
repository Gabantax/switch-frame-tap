# Switch Frame Tap for Android and Android TV

An **Android / Android TV / Google TV viewer for Android 6.0 (API 23) and later**,
made by [didix16](https://github.com/didix16). It receives H.264 video and PCM audio
directly from Switch Frame Tap over the local network or USB, like the PC viewer,
and looks like it: the same home screen, Settings drawer, colours and typeface. The
controller stays connected to the Switch; the app only shows and plays the stream.

Since Switch Frame Tap 0.7.14 the app ships with each release
(`switch-frame-tap-<version>-android.apk`) and carries the release's version. It
has played on a Hisense Android TV (Android 11) over USB; other TVs, phones and
latency are still being reported, so please tell us how it does on yours.

## Installation and connection

1. Download `switch-frame-tap-<version>-android.apk` from the release, copy it to
   the TV, TV box or phone, and open it with a file manager that can install APKs.
   Allow installation from that file manager when Android asks. The app appears as
   **Switch Frame Tap**. (Coming from the 0.1.x preview APK: uninstall it first, it
   was signed with a different key.)
2. In the Switch Frame Tap app on the console, set Connection to match (USB - PC
   viewer, or Network) and restart the console if it asks. Close the PC viewer:
   the console serves one viewer at a time.
3. **Network:** choose Network in the app. The Switch and this device must be on the
   same local network; the Switch can stay docked. Leave the IP address blank and the
   app finds the Switch (UDP 9951) or uses the last address; otherwise type the
   address the console's app shows. Streaming uses TCP 9950.
4. **USB:** choose USB in the app and connect the Switch's USB-C port to a USB host
   port of the device with a data cable (the Switch in handheld). Allow USB access
   when Android asks. Webcam modes are not supported by this app.
5. Select **Connect** and open a game. The picture replaces the home screen; while
   there is none, the home screen says why, as the console reports it ("Connected -
   no game is running", ...). The app connects again by itself the next time it opens.

## Controls

- TV remote: the arrows move, OK selects. **Menu** opens Settings anywhere; during
  the game **OK** and **Back** open it too, and **Back** closes it. On the home
  screen Back leaves the app.
- Phone or tablet: tap the gear for Settings, or tap the game.
- **Settings:** Connection (USB / Network), **Mute** (this device only), **Stable
  audio** (a 60 ms buffer instead of 20 ms, if the sound drops out - it reconnects),
  **Stats overlay** (frame rates, bitrate, decoder, dropped frames, sound and the
  device's audio buffer).

The app closes the connection and releases audio and USB when it goes to the
background, and reconnects when it comes back.

## Quality and latency

The app preserves the received H.264 stream without re-encoding or reducing its
resolution. Quality is selected on the Switch, whose network sender can adjust it
to the available bandwidth. Start with High and 1080p if your configuration supports
them. If playback pauses or repeatedly resynchronizes, try Medium or 720p on the
console. Actual 1080p60 capture also depends on the game and Switch setup; the app
does not turn a 30 fps game into 60 fps.

Hardware H.264 decoders are preferred. On Android 11 and later, low-latency decoding
is requested when the driver advertises support. If an optimized configuration
fails, the app tries a basic configuration and other decoders, with software as a
last resort. Software decoding can increase latency.

Queues are bounded. The newest decoded output is presented, and input reference
recovery starts at an IDR after a gap or queue overflow. A device or connection
that cannot keep up can cause skipped frames or a pause until the next keyframe.
Maximum quality and a fixed low latency cannot be guaranteed on every TV.

Version 0.1.1 fixes repeated decoder initialization while waiting for the first
input buffer. It uses relative presentation timestamps and handles drivers that
rewrite them. It also removes the fixed 120 ms output rejection threshold, which
could discard every frame in a deeper pipeline. A decoder that accepts input but
produces no output is replaced at a subsequent IDR.

Audio is no longer flushed when video resynchronizes or whenever its native queue
exceeds 80 ms. Some TVs require a larger native buffer before playback can start.
AudioTrack bounds the system queue while the application keeps its own queue small.
On Android 12 and later, a low start threshold is requested when the driver allows it.

**Statistics do not measure total end-to-end latency.** Queue + decode measures
the interval from receiving the packet header to submitting decoded output for
display. It excludes network delay before arrival and the panel's own processing.
Console age comes from the sender and is a separate measurement. The protocol does
not provide a shared presentation clock for audio and video; the viewer limits
queue accumulation with small buffers.

## Compatibility

The same APK contains Java code and resources without CPU-specific native libraries.
It is intended for Android TV and Google TV devices, including Sony TVs running
Android TV, with **Android 6.0 or later**. There is no confirmed model compatibility
list yet. Sony TVs with another operating system or Android older than 6.0 are not
supported by this version.

USB is optional and detected at runtime. The TV firmware must expose USB host
support and permit access to device `1209:5f1e`, interface 0, bulk IN endpoint `0x81`.
A port that supports USB storage does not by itself confirm this capability.
Network reception does not depend on USB host support.

## Building

See [BUILDING.md](BUILDING.md) for requirements, environment setup, SDK installation,
build commands, tests, and optional distribution packaging.

Releases are built by GitHub Actions (`.github/workflows/viewer-android.yml`) on each
version tag; by hand it can also take screenshots on an Android TV and a phone
emulator. With JDK 17 and Android SDK Platform 34 / Build Tools 34.0.0 configured, run
from `android-tv/` on Windows:

```powershell
.\gradlew.bat :app:assembleDebug :app:protocolTest :app:playbackTest :app:lintDebug
python tools/package.py
```

On Linux/macOS, use `bash gradlew` in place of `.\gradlew.bat`.
The APK is generated at `app/build/outputs/apk/debug/app-debug.apk`. Packaging copies
it to `../dist/` with the corresponding source archive and SHA-256 checksums.

This manually installed preview targets SDK 34. A Google Play release would require
reviewing the current target SDK and newer Android local-network permissions.

## Testing without a Switch

From the full repository root, on a PC on the same local network as the TV:

```shell
python android-tv/tools/replay_server.py logs/m83-runI/stream-first10.sft --tone
```

Enter the address printed by the server in the TV app and select **Connect**.
The server loops a real H.264 capture and generates a 440 Hz stereo PCM tone. This
allows testing Android decoding, audio, and TCP reconnection separately from the
console. The older capture contains only IDR frames; it does not replace testing
the current IDR/P stream, USB, or actual Switch latency. The fixture belongs to the
original repository and is not included in the Android source ZIP.

See [VALIDATION.md](VALIDATION.md) for completed checks and the pending TV test plan.

## Credits and license

Made by **didix16** (PR #17), based on the protocol and viewer by **Tomas Uribe**,
[switch-frame-tap](https://github.com/TomasUribe/switch-frame-tap).
`LICENSE` contains GPL-2.0, and `UPSTREAM-NOTICE` preserves the upstream credits.
The app bundles DejaVu Sans (`app/src/main/assets/fonts/`, its licence alongside).
The distribution includes the corresponding source code for the Android viewer.

Official API references:
[MediaCodec](https://developer.android.com/reference/android/media/MediaCodec),
[low-latency decoding](https://source.android.com/docs/core/media/low-latency-media),
[AudioTrack](https://developer.android.com/reference/android/media/AudioTrack),
[USB](https://developer.android.com/reference/android/hardware/usb/UsbDeviceConnection),
[Android TV](https://developer.android.com/training/tv/get-started/create).