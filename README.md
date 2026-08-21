# SC Audio Relay

Streams live Linux desktop audio to the Steam Controller's haptics. 

Only supports Steam Controller 2026
Linux + PipeWire only for now. Windows support can come later.

## Build on Arch

```bash
sudo pacman -S --needed base-devel cmake pkgconf git libpipewire libsamplerate hidapi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Install

```bash
sudo install -Dm755 build/sc-audio-relay /usr/local/bin/sc-audio-relay
```

## Run

Connect the controller, play some music, then:

```bash
sc-audio-relay --virtual-output --gain 0.5
```

Pick **Steam Controller Haptics** as the app's output. It exists while SC Audio
Relay is running and sends audio to the controller, not your speakers.

Or mirror your current output:

```bash
sc-audio-relay --mirror-default --gain 0.5
```

You can also manually set the mirrored output device to something else, find its PipeWire name and pass it in:

```bash
wpctl status -n
wpctl inspect ID
sc-audio-relay --mirror-device "node.name.from.above"
```

Press Ctrl+C to stop. If the controller hits a `/dev/hidraw` permission error,
install the usual Steam/controller udev rules and reconnect it.

## Credits

Big thanks to [Pixel1011](https://github.com/Pixel1011) for making
[TritonLib](https://github.com/Pixel1011/TritonLib), which handles talking to
the controller. Go check out their
[SteamHapticsPlayer](https://github.com/Pixel1011/SteamHapticsPlayer) too. It
plays audio files through the haptics and kicked off this whole idea.

## License

Licensed under the Apache License 2.0. See `LICENSE` for the full terms and
`NOTICE` for third-party attributions.
