# NX-Pctl-Manager

Nintendo Switch parental-controls manager for Atmosphère/custom-firmware consoles. It uses the restricted system `pctl` service; use at your own risk and consult [the reverse-engineering notes](docs/pctl-reverse-engineering.md).

## Plutonium UI migration

The `migration/plutonium-ui` branch replaces the Borealis entry point with [Plutonium](https://github.com/XorTroll/Plutonium). The new menu currently exposes status refresh, PIN setup, preset all-days and per-day play limits, play-timer removal, a 21:00–06:00 bedtime preset, timeout mode, network-clock seeding, optional central-server synchronization, parental-control deletion, companion-app unlinking, and a diagnostic dump.

This is an active port, not a release build. The new UI is not yet feature-parity with the previous Borealis interface: the bedtime time is currently a preset, per-day changes are applied immediately rather than staged, and the Plutonium build has not yet been confirmed on hardware. Please review changes before installing them on a console.

## Launch authorization

- With the default build, if a console PIN exists, the app asks for it at launch (up to five attempts); cancel or failure exits. If no PIN is registered, there is no local PIN gate.
- A build with `REMOTE_CONTROL=1` instead requires the configured central-server authentication to succeed at launch. The current client expects an `http://` URL and is intended for a trusted LAN; plain HTTP does not protect credentials against interception. Do not expose it to untrusted networks.
- The console PIN is not sent to the central server.

## Build

Requires [devkitPro](https://devkitpro.org/), the Switch toolchain/libnx, and Plutonium's SDL2-related packages:

```sh
git submodule update --init --recursive
export DEVKITPRO=/opt/devkitpro
make
```

The Makefile builds the pinned Plutonium submodule before the application. To enable the optional central-server client, use `make REMOTE_CONTROL=1` only when a Switch-compatible libcurl and TLS dependency set is available. The default build leaves that transport disabled.

The old Borealis-based CMake build is retired on this branch; use the devkitPro Makefile.

## Diagnostics

The application log is saved to `sd:/switch/nx_pctl_manager.log`. The read-only diagnostic report is saved to `sd:/switch/nx_pctl_dump.txt`; it is intended for bug reports and masks the PIN digits.

## License

See [LICENSE](LICENSE). Plutonium is a separate third-party dependency; its license is in `extern/plutonium/LICENSE`.
