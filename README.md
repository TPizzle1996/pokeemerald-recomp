# pokeemerald-recomp

`pokeemerald-recomp` is a native desktop recompilation/port of Pokémon Emerald,
built from the [pret Pokémon Emerald decompilation](https://github.com/pret/pokeemerald).
The public alpha supports Linux x64 and Windows x64.

> **Alpha software:** `v0.1.0-alpha` is an early test release. Back up important
> saves and expect bugs or compatibility changes.

## A Pokémon Emerald ROM is required

**No Pokémon Emerald ROM is included in this repository or in any release.** You
must provide your own compatible ROM. The launcher validates and imports it
locally; the ROM is never uploaded, and gameplay remains unavailable until a
valid import succeeds.

The supported ROM is:

- Revision: Pokémon Emerald (USA, Europe), Rev 0 (`BPEE01`)
- SHA-1: `f3ae088181bf583e55daf962a92bb46f4f1d07b7`
- Size: 16 MiB (16,777,216 bytes)

## Features

- Native Linux x64 and Windows x64 execution
- Emerald-style launcher and separate player profiles
- Remappable keyboard and controller inputs
- Hold and toggle fast-forward
- Quick Save (`F5`) and Quick Load (`F9`)
- Save-state manager (`F6`) and manual save-state slots (`F7`)
- Mid-battle save states
- Save-state screenshots and manager previews
- Native normal-save persistence for each profile
- Fullscreen toggle (`F10`)

## Install a release

### Linux x64

1. Extract `pokeemerald-recomp-v0.1.0-alpha-linux-x64.tar.gz`.
2. Keep the `images/` directory beside the executable.
3. Run `./pokeemerald-recomp` from the extracted directory. You may also run it
   by absolute path from another working directory.

The Linux build uses the system SDL2 and SDL2_image runtime libraries. Install
their 64-bit runtime packages with your distribution's package manager if they
are not already present.

### Windows x64

1. Extract `pokeemerald-recomp-v0.1.0-alpha-windows-x64.zip`.
2. Keep the included DLLs and artwork beside `pokeemerald-recomp.exe`.
3. Double-click `pokeemerald-recomp.exe`.

The archive includes the required SDL2 runtime DLLs. MinGW, MSYS2, Visual Studio,
and a separate SDL installation are not required.

On first launch, select a ROM you legally own. A successful import opens the
profile selector; choose a profile to start the game. Imported game content,
profiles, saves, settings, and save states are written to per-user application
data, not to the release folder.

Typical data locations are:

- Linux: `$XDG_DATA_HOME/pokeemerald/pokeemerald/`, or
  `~/.local/share/pokeemerald/pokeemerald/` when `XDG_DATA_HOME` is unset
- Windows: `%APPDATA%\PokemonEmeraldRecomp\`

## Default controls

| GBA control | Keyboard |
| --- | --- |
| A | `Z` |
| B | `X` |
| Start | `Enter` |
| Select | `Backslash` |
| L | `A` |
| R | `S` |
| D-pad | Arrow keys |
| Fast-forward (hold) | `Space` |
| Fast-forward (toggle) | `Shift+Space` |
| Quick Save | `F5` |
| Save-state manager | `F6` |
| Manual save-state slot | `F7` |
| Quick Load | `F9` |
| Fullscreen | `F10` |
| Pause | `Ctrl+P` |
| Soft reset | `Ctrl+R` |

Controls can be changed from the launcher's settings screen.

## Build and package

The canonical Linux x64 build is:

```sh
make -f Makefile_pc NATIVE_LINUX=1 LINUX64=1 rom -j"$(nproc)"
```

Release targets build clean staging layouts under `dist/` and versioned archives
under `release/`:

```sh
make -f Makefile_pc release-linux -j"$(nproc)"
make -f Makefile_pc release-windows -j"$(nproc)"
make -f Makefile_pc release -j"$(nproc)"
```

Windows dependencies are downloaded from the official SDL release archives and
verified against pinned SHA-256 checksums. Packaging copies files from an explicit
allowlist and never reads from `rom/`. Building both release targets requires a C
toolchain, an x86_64 MinGW-w64 GCC cross-toolchain, `curl`, `tar`, and `zip`.

See [INSTALL.md](INSTALL.md) for upstream decompilation setup details and `docs/`
for port architecture notes.

## Legal

Pokémon and Pokémon Emerald are trademarks of Nintendo, Creatures Inc., and
GAME FREAK inc. This unofficial fan project is not affiliated with or endorsed
by those companies.

The scoped [LICENSE](LICENSE) applies only to original multiplatform-port changes
contributed through this fork. It does not relicense upstream code, third-party
components, or copyrighted game assets.
