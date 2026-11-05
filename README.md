# Omarchy Desktop Audio Recorder

This project contains a user-level F10 desktop-audio recorder for Omarchy. It
captures the monitor source belonging to the current default output, displays a
small GTK4 layer-shell waveform labelled **Desktop**, and saves FLAC files in
`~/Music/Recordings/`. It does not use Voxtype's microphone capture or alter F9.

The implementation and its remaining validation limits are described in
[the technical design](docs/technical-design.md). The implementation is C
because Rust/Cargo is not installed in the build environment; the native GTK4,
gtk4-layer-shell and libpulse APIs remain the selected architecture.

## Build and commands

Build with `make`. Runtime requirements are GTK4, gtk4-layer-shell, libpulse,
PipeWire's PulseAudio server, FFmpeg, `flac`, and the Omarchy notification
helper. Run `./omarchy-desktop-recorder doctor` to inspect availability.

Use `press` on key-down and `release` on key-up. `stop` also finalizes a live
recording, `status` reports the current session, and `reveal PATH` opens a
saved file in the file manager. F10 bindings are provided in
`integration/hypr/bindings.lua`. Install the binary and owned Hyprland binding
with `bash packaging/install.sh`; remove them with `bash packaging/uninstall.sh`.
The installer checks F10 conflicts, backs up the bindings file, and validates
the reload.

Create a versioned Linux deployment archive and SHA-256 file with
`bash packaging/release.sh 0.1.0`. The archive includes the built executable,
the installer and uninstaller, the F10 binding module, and this README. After
extracting it, run `bash packaging/install.sh` from the extracted directory.
Choose a new version for each release; the script refuses to replace existing
artifacts.

The recorder currently refuses output layouts with more than two channels.
This is deliberate: the inspected host's 2.1 channel map has not passed the
independent FLAC channel-identification gate in the design. The live stereo
capture files were removed after validation. The F10 binding is installed on
the current host; a physical F10 press/release was confirmed on this host.
