# NX-Pctl-Manager

Nintendo Switch parental-controls manager. Requires custom firmware (Atmosphère recommended) and a firmware version compatible with the system `pctl` service. Use at your own risk; see the existing reverse-engineering notes under `docs/`.

## UI migration

The `migration/plutonium-ui` branch replaces the Borealis UI entry point with **[Plutonium](https://github.com/XorTroll/Plutonium)**. This branch is an in-progress port: the current entry point is a minimal functional shell around the existing parental-control service and optional central-server client. Not every former screen and workflow has been ported yet, and the branch has not been verified on hardware.

## Build from source

Requires [devkitPro](https://devkitpro.org/), the Switch toolchain/libnx, and Plutonium's SDL2-related packages. From the repository root:

```sh
git submodule update --init --recursive
export DEVKITPRO=/opt/devkitpro
make
```

The Makefile builds the Plutonium static library from the pinned submodule before building `nx_pctl_manager.nro`. To build with the optional central-server client, use `make REMOTE_CONTROL=1` only after installing a Switch-compatible libcurl and TLS dependency set. The default build leaves that transport disabled.

The old Borealis-based CMake build has been retired; use the devkitPro Makefile above.

## Central-control client

The existing central-control client is experimental. Its current configuration and protocol are documented in `docs/central-control-server.md`. Do not assume the server gate is enabled in the default build. Never send the console PIN to the server.

## License

See [LICENSE](LICENSE). Plutonium is a separate third-party dependency under its own license; see `extern/plutonium/LICENSE`.
