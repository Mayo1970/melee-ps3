# Melee-PS3

A native port of Super Smash Bros. Melee to the PlayStation 3, built on the
[doldecomp/melee](https://github.com/doldecomp/melee) decompilation. It runs as homebrew on a PS3 with CFW or HEN.
It is built with the open-source [PSL1GHT](https://github.com/ps3dev/PSL1GHT) SDK.

Melee-PS3 is not an emulator. The game code is compiled for the Cell PPE, GX is translated to the RSX, and the
GameCube DSP runs on an SPU.

Melee-PS3 is work in progress.

## Install and set up

1. Install `melee.pkg` with the Package Manager on the XMB.
2. Copy your disc image to `/dev_hdd0/game/MELE00001/USRDIR/games/` or to `/dev_usbXXX/games/` on a USB drive.
   Subfolders are fine.
3. Start **Super Smash Bros: Melee** from the Game column.

**Melee-PS3 does not include any game data.** You need your own disc of the US version, `GALE01` revision 2 (1.02).

Supported disc formats: `.iso`, `.gcm`, `.ciso`, or an extracted disc folder (`sys/` and `files/`).

| Path | Content |
|---|---|
| `/dev_hdd0/game/MELE00001/USRDIR/games/` | Disc images. |
| `/dev_hdd0/game/MELE00001/USRDIR/options.txt` | Settings from the PS3 menu. |
| `/dev_hdd0/game/MELE00001/USRDIR/disc.txt` | Optional. A game ID or a path to choose the disc. Without it, the first `GALE01` revision 2 disc is used. |
| `/dev_hdd0/game/MELE00001/USRDIR/card/` | Memory card saves, as GCI files. |

## Controls

### DualShock 3

| Input | GameCube |
|---|---|
| Left stick | Control stick |
| Right stick | C-stick |
| **Cross** | A |
| **Square** | B |
| **Circle** | X |
| **Triangle** | Y |
| **R1** | Z |
| **L2 / R2** | L / R (analog) |
| D-pad | D-pad |
| **Start** | Start |
| **Select** | PS3 menu |

### GameCube controller

Plug the Wii U GameCube adapter into a USB port. No setup is needed. Rumble works when the adapter's power plug is
connected. To open the PS3 menu, hold **Z** and press **D-pad Up**.

## PS3 menu

Press **Select** on a DualShock 3, or hold **Z** and press **D-pad Up** on any controller. The menu opens only
outside matches. **B**, **Start** or **Select** close it. Settings are saved to `options.txt`.

| Option | Effect |
|---|---|
| Widescreen | 16:9 rendering. Letterboxed on a 4:3 TV. |
| Unlock All | Unlocks all characters, stages, events 1-50 and trophies. |
| Tap Jump | ON or OFF, per controller port. |
| C-Stick | SMASH (default) or TILT, per port. TILT gives tilt attacks on the C-stick instead of smash attacks. |
| C-Stick 1P | CAMERA (default) or ATTACK. In 1-P modes, ATTACK makes the C-stick attack as in VS instead of moving the camera. |
| D-Pad Swap | DualShock 3 only, per port. The D-pad acts as the control stick. |
| Input Delay | Netplay input delay, 0-7 frames. Default 2. |
| Netplay | Host or join an online match. |

## Netplay

Online 1v1 with rollback.
- Each player picks their own fighter. The host picks the stage in the normal VS menus.
- Rules are the host's VS rules.

### Play online

1. Both players install the **same build**. A different build refuses the pairing.
2. Host: open the PS3 menu, go to **NETPLAY**, choose **HOST**, press **A**. The row shows an 8-digit code.
3. Joiner: go to **NETPLAY**, choose **JOIN**, press **A**, enter the host's code, press **A**.
4. When the row shows **PLAYER 1** / **PLAYER 2**, close the menu and both start a VS match.

The host opens UDP port **51000** with UPnP. If the router has no UPnP, forward UDP 51000 to the host's PS3 by hand.
The joiner needs no setup. The code is the host's public IPv4 address in hexadecimal (`94.35.119.59` is `5E23773B`).

## Compiling

The build makes `ps3/build/ppu/game/melee.pkg`. The maintainer builds in Docker with `ppu-gcc` 7.2.0.

### With Docker

`ps3/build.sh` runs `make` in a Docker image named `ps3dev-latest:latest`. Use the same image as NotDC-PS3:

```sh
docker pull mayo2853/ps3dev-latest:latest
docker tag mayo2853/ps3dev-latest:latest ps3dev-latest:latest
```

On Windows, run the scripts from Git Bash with Docker Desktop.

### Steps

1. Extract your disc into the repo:

   ```sh
   python ps3/tools/datgen/extract.py Melee.iso orig/GALE01
   ```

2. Build the `.dat` schema image and generate the native `.dat` readers (the run command is in `datgen.sh`):

   ```sh
   docker build -t melee-datgen:1 ps3/tools/datgen
   ps3/tools/datgen/datgen.sh
   ```

3. Build the package:

   ```sh
   ./ps3/build.sh -j12 game
   ```

### Build commands

| Command | Result |
|---|---|
| `./ps3/build.sh game` | `ps3/build/ppu/game/melee.pkg` |
| `./ps3/build.sh clean` | Deletes `ps3/build/ppu/` |

Port code lives in `ps3/`. Game and SDK sources change only under `TARGET_PC` or `TARGET_PS3`, so the matching
GameCube build stays the same. For the decompilation itself (matching build, tooling, contributing), see
[doldecomp/melee](https://github.com/doldecomp/melee).

## License

The port code in `ps3/` is licensed under the GNU General Public License, version 2 or (at your option) any later
version. See [ps3/LICENSE](../ps3/LICENSE). The files in `ps3/gx/` that come from Dolphin keep their GPLv2+ headers.

The decompilation outside `ps3/` comes from [doldecomp/melee](https://github.com/doldecomp/melee) and keeps its
upstream terms. The tools in `tools/` keep their own licenses. Super Smash Bros. Melee is © Nintendo / HAL
Laboratory. This repository and its releases contain no game data.

## AI disclosure

Melee-PS3 is an AI-assisted project. The port itself was tested on real PS3 hardware.

## Credits

- [doldecomp/melee](https://github.com/doldecomp/melee) and its contributors: the decompilation this port is built on.
- [PSL1GHT](https://github.com/ps3dev/PSL1GHT) / [ps3toolchain](https://github.com/ps3dev/ps3toolchain): PS3
  homebrew toolchain and runtime library.
- [Dolphin](https://github.com/dolphin-emu/dolphin): the AX HLE that the SPU audio follows.
