# Fat Princess Recomp on Linux and Steam Deck

The Linux version is the same game as the Windows one, built natively: no Wine or Proton. It draws with Vulkan and uses SDL2 for the window, controllers, keyboard, mouse and sound.

## What you need

| | Where it comes from |
|---|---|
| This package | `FatPrincess-Linux` (or your own build: `./build.sh`) |
| The game files | The `vfs` folder from the Windows install of Fat Princess Recomp |
| Your settings (optional) | `fatprincess.ini` from the Windows install. It carries your key bindings and the Fat Roles key, and works unchanged on Linux. |
| PS3 firmware `dev_flash` | Found automatically if RPCS3 is installed (AppImage, Flatpak, EmuDeck or RetroDECK). Otherwise, copy a `dev_flash` folder next to `fatprincess.sh`. |

The Steam Deck runs it as it comes. On other Linux systems you need a Vulkan driver, plus the system's SDL2 (`libsdl2-2.0-0` / `sdl2`), which most desktops already have.

## Steam Deck: setting it up

1. **Switch to Desktop Mode** (Steam button, then Power, then Switch to Desktop).
2. **Copy the folder over.** Put `FatPrincess-Linux` somewhere on the Deck, for example `/home/deck/Games/FatPrincess`, using a USB stick, an SD card or a network share.
3. **Add the game files.** Copy `vfs` into that folder, next to `fatprincess.sh`. Copy `fatprincess.ini` too if you want your settings and the Fat Roles key.
4. **Add it to Steam.** In Steam, choose *Games*, then *Add a Non-Steam Game to My Library*, then *Browse*. Set the file type to *All Files*, pick `fatprincess.sh`, and press *Add Selected Programs*.
5. **Leave compatibility off.** This is a native Linux game, so don't force a Proton version in its Properties. You can rename the entry to *Fat Princess*.
6. **Go back to Gaming Mode and play.** The Deck's controls act as an Xbox controller. With *Button icons* on Auto, every prompt in the game shows A, B, X and Y.

The first start takes a few extra seconds while the game prepares its patched data. It may also stutter briefly the first time each effect appears, while shaders compile; after that they come from the cache.

## Controls

| Steam Deck | PS3 button | In the game |
|---|---|---|
| A | Cross | Jump, confirm |
| B | Circle | Pick up, escort, back |
| X | Square | Attack (hold to charge), build, repair |
| Y | Triangle | Switch weapon, Fat Roles specials |
| L1 / R1 | L1 / R1 | Lock on, Warrior shield |
| D-pad | D-pad | Bots follow / release, taunts |
| View / Menu | Select / Start | Map / menu |
| L3 + R3 | L3 + R3 | Unstuck |

With a keyboard, the bindings in `fatprincess.ini` apply when no controller is connected, exactly as on Windows.

| Key | Action |
|---|---|
| <kbd>Alt</kbd> + <kbd>Enter</kbd> or <kbd>F11</kbd> | Full screen on / off (desktop) |
| <kbd>F8</kbd> | Record 30 s and write `perf_report.txt` |
| <kbd>F10</kbd> | Write a freeze report to `run.log` |

## Settings

There is no launcher on Linux yet. Settings live in `fatprincess.ini` next to `fatprincess.sh`, and the file is the same as on Windows. Edit it in Desktop Mode with any text editor (Kate on the Deck). Settings you might change:

```ini
[Graphics]
; desktop Linux: 1 = start full screen (Gaming Mode always is)
Fullscreen=1
; FPS in the window title
ShowFPS=1

[Controls]
; auto, xbox, keyboard or ps
ButtonIcons=auto
MenuHints=1
SkipHowToPlayPrompt=1

[Bots]
; classic or improved
Mode=classic

[Paths]
; empty = look for RPCS3's
DevFlash=
; Fat Roles key (32 hex digits)
DlcKey=
```

## Troubleshooting

Everything the game does is written to `run.log` next to `fatprincess.sh`.

- **`version 'GLIBC_2.xx' not found`:** the system is older than the build. Update SteamOS, or build on that machine with `./build.sh`.
- **No sound:** `run.log` says `no PS3 firmware dev_flash found`. Install RPCS3, or copy a `dev_flash` folder next to `fatprincess.sh`, or set `DevFlash=` in the settings.
- **Button prompts show PlayStation icons:** the font was not found, and `run.log` says `no font found`. Keep the `fonts` folder next to the game.
- **Nothing happens in Gaming Mode:** start `fatprincess.sh` from a terminal in Desktop Mode, and read the message it prints or `run.log`.

## Building it yourself

```sh
# Debian/Ubuntu: sudo apt install clang cmake ninja-build libsdl2-dev
# Arch:          sudo pacman -S clang cmake ninja sdl2
./build.sh                 # builds ./FatPrincessRecomp (the first build takes a while)
./tools/package_linux.sh   # makes dist/FatPrincess-Linux.tar.gz
```

The build targets x86-64-v3 (AVX2), which covers the Steam Deck and any Intel Haswell or AMD Zen CPU and newer. For an older CPU, set `-DFP_LINUX_ARCH=x86-64-v2` in `build.sh`.
