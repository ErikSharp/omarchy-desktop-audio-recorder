# Implementation status

The source implementation follows the approved native GTK4/layer-shell and
libpulse architecture. It is written in C because Rust and Cargo were absent
from the build environment; GTK4, gtk4-layer-shell, libpulse, and FFmpeg remain
external runtime dependencies. `packaging/install.sh` and
`packaging/uninstall.sh` manage the user binary and the marked Hyprland binding.

Waveform frames use a fixed 300-slot history. New frames enter at the right
edge and move left by one slot as later frames arrive, so startup does not
stretch a handful of samples across the full display. The supplied screen
recording could not be inspected because its path was unavailable in the
workspace; this change is based on the reported startup motion and the draw
history logic.

The current recorder resolves the default sink on each press, verifies that
its selected source is that sink's monitor, requests a fixed monitor stream,
and refuses source movement. Mono and standard front-left/front-right stereo
are supported. Other channel layouts fail closed. Captured PCM is converted by
PipeWire PulseAudio to 24-bit little-endian PCM before FLAC encoding. It does
not capture a microphone directly.

The implementation builds with `make`. Two short captures through the CLI
worker path finalized as 48 kHz, 2-channel FLAC and passed integrity and
non-silence checks. The worker was adjusted to start a separate session; the
normal press/status/release path remained alive through finalization. Test audio
and partial files were removed after validation. The installer verified the
unmodified F10 press/release bindings with no Hyprland config errors, and a
repeat installation did not duplicate them. A physical F10 gesture produced a
valid short stereo FLAC; the recorder returned to idle. Virtual key injection
did not trigger the binding, while the physical key path was verified.

Still open: F10 event ordering under modifier/submap changes, output-change and silence
handling, overlay appearance and click-through behavior, notification click,
failure recovery, resource use, lock/session shutdown, and independent
microphone-exclusion checks. Stereo is supported; unqualified multichannel
layouts remain intentionally unsupported. Versioned release packaging is
implemented in `packaging/release.sh`. Version 0.1.0 was published to the
configured Syncthing release folder; the archive checksum was verified before
and after publication. Receipt by another peer, second-machine qualification,
upgrade and uninstall execution remain unverified.

After the user reported that the overlay was top-left with no visible waveform,
the layer-shell anchors were corrected to center the surface. The waveform
history now advances through fixed slots from right to left. The label row sits
above the waveform; on map, the overlay uses the F9 display's 85%-of-monitor
vertical position and raises its surface by 12 px to align its 48 px waveform
with F9's centered 48 px waveform within a 72 px surface. A cropped
desktop view confirmed the earlier centered position; that view also exposed a
dark theme accent and clipping from excessive waveform gain. The fill now uses
a contrast-safe accent and reduced gain. The user then reported repeated
PulseAudio error notifications. The source was traced to the normal Pulse
context teardown being handled as a connection failure; the state callback now
ignores disconnect states during an intentional stop. A live visual check of
the current positioning has not been run.
