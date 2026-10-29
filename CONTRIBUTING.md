# Contributing

Bug reports, fixes and testing are all welcome.

**Reporting a bug**

- Say whether it happens on an **Xbox Series X|S** (which one, and the OS version) or on a PC, and how you installed
  it (Device Portal, Visual Studio, `install-pc.ps1`).
- Include the game (its name and serial, for example `SLUS-00594`).
- Attach `LocalState\SwanStation\logs\SwanStationPS5.log` (on a console, from the Device Portal's file explorer; on a PC,
  `%LOCALAPPDATA%\Packages\SwanStationXbox_*\LocalState\SwanStation\logs`). Copy it **before** starting the app again:
  each start keeps only the previous log, as `SwanStationPS5.old.log`.
- After a crash, add the dump (`%LOCALAPPDATA%\CrashDumps` on a PC, the Device Portal on a console).

**Changing the code**

- Build with `.\tools\build-xbox.ps1` (see the README). A change should build and the package should start.
- Changes to the SwanStation core go in `tools/patches/SSPS5.patch` or `SSXBOX.patch`, not in the submodule.
- Keep the SPDX header (`GPL-3.0-or-later`) on new source files.
- Never commit BIOS files, games, keys, `.env`, `build/` or `dist/`.
- The version lives in `SwanStationPS5_VERSION` (`src/SwanStationPS5.h`) and `Version` (`xbox/Package.appxmanifest`);
  change both together, and only when releasing.

**The PS5 version** has its own releases and issues: <https://github.com/darkxex/SwanStationPS5>.
