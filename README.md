<p align="center">
    <a href="https://github.com/xenia-canary/xenia-canary/tree/canary_experimental/assets/icon">
        <img height="256px" src="https://raw.githubusercontent.com/xenia-canary/xenia/master/assets/icon/256.png" />
    </a>
</p>

<h1 align="center">Xenia Edge Kinect - Xbox 360 Emulator</h1>

Xenia Edge Kinect is an unofficial fork of [Xenia Edge](https://github.com/has207/xenia-edge)
that focuses on Kinect titles. You can play with a real Kinect sensor, or
with an ordinary webcam through body-tracking software such as
[Dollars MONO](https://www.dollarsmocap.com/).

This fork is not affiliated with the Xenia, Xenia Canary or Xenia Edge
projects. Report problems with Kinect, avatars or this build here, not
upstream.

## Kinect input

Pick the input with `--nui=<backend>` and enable NUI with
`--allow_nui_initialization=true`.

| Backend | Input | Platform | Status |
|---|---|---|---|
| `vmc` | Webcam, through any [VMC protocol](https://protocol.vmc.info/) sender such as Dollars MONO | Windows | Tested with Kinect Adventures! |
| `kinect` | Kinect for Windows sensor, or Xbox 360 Kinect with its PC USB/power adapter | Windows | Verified upstream in the Canary fork; not yet tested in this build |
| `zcam` | Depth frames from a local Z-Cam bridge on TCP port 8001 | Windows | Experimental |
| `replay` | A depth recording made with `--nui_record` | All | For testing |
| `nop` | No sensor | All | Default fallback |

### Webcam with Dollars MONO

1. Start Dollars MONO and track yourself with your webcam.
2. In Dollars MONO, turn on VMC protocol output to `127.0.0.1`, port `39539`.
3. Start the game:

   ```
   xenia_edge.exe --nui=vmc --allow_nui_initialization=true --allow_avatar_initialization=true "<path to game>"
   ```

The `vmc` backend renders the received pose as a Kinect depth image. The
game's own skeletal tracker then runs on that image, as it would on a
console. Stand so that your whole upper body is visible to the webcam.

| Flag | Default | Purpose |
|---|---|---|
| `--nui_vmc_port` | `39539` | UDP port the VMC sender sends to. |
| `--nui_vmc_distance_mm` | `2200` | Distance from the virtual sensor to the body's hips. |
| `--nui_vmc_mirror` | `false` | Swap left and right. |
| `--nui_vmc_stand` | `true` | Keep the legs straight, so a seated player still looks like a whole standing body. |
| `--nui_vmc_floor` | `true` | Render a floor plane under the body. |
| `--nui_vmc_hips_x` | `1.0` | How much of your sideways movement is followed; `0` keeps the body centred. |
| `--nui_vmc_reach` | `0.3` | Metres a raised hand is pushed toward the sensor. |
| `--nui_vmc_jump_gain` | `1.5` | How much a rise of the hips is amplified into a jump. |
| `--nui_show_depth_preview` | `false` | Show the depth frames the game receives. |

### Real Kinect sensor

Install the
[Kinect for Windows SDK 1.8](https://www.microsoft.com/en-us/download/details.aspx?id=40278)
(it includes the runtime and drivers), connect the sensor, and run with
`--nui=kinect`. No Kinect SDK is needed to build.

## Avatars

Run with `--allow_avatar_initialization=true`. Avatar assets come from the
game disc when it carries an `AvatarAssetPack` (Kinect Adventures! does). A
profile without an avatar gets a generated one. For other titles, point `--avatar_asset_pack_path` at
the avatar asset package from your own console or from a game disc's
`$SystemUpdate` folder. This fork ships no system files or keys.

## Tested games

| Game | Result |
|---|---|
| Kinect Adventures! (Europe, Rev 3) | Player detected with `--nui=vmc`, and the avatar is shown on the play-space screen. Full gameplay is still being tested. |

## Building

Build as you would build Xenia Edge. On Windows the build also needs
`meson` and `mako` (`pip install meson mako`), `ninja` on `PATH`, and the
Slang compiler (`python xenia-build.py slang`). Then:

```
python xenia-build.py build --config=release
```

The executable is written to `build\bin\Windows\Release\xenia_edge.exe`.

## More

[docs/nui.md](docs/nui.md) documents the Kinect protocol, flags, recording and
replay, and the Kinect Guide gesture. It was written for the Canary fork, so
read `xenia_canary.exe` as `xenia_edge.exe` and `xenia-canary.config.toml` as
`xenia-edge.config.toml`.

## Credits

- [has207/xenia-edge](https://github.com/has207/xenia-edge): the emulator this fork is based on.
- [ryantimjohn/xenia-canary](https://github.com/ryantimjohn/xenia-canary): the Kinect (NUI) stack, the `kinect` and `replay` backends, and the GPU read-back fixes the skeletal tracker needs.
- [dcwestra/xenia-edge](https://github.com/dcwestra/xenia-edge): the avatar implementation.
- The Xenia and Xenia Canary teams.

## About Xenia Edge

Xenia Edge is yet another experimental fork of the Xenia emulator, originally based on [Xenia Canary](https://github.com/xenia-canary/xenia-canary). Edge builds have evolved far beyond Canary in many areas, starting from the UI and ranging to kernel emulation, networking, graphics backend features, JIT improvements, arm64 and macOS support, and many others. Exact differences are hard to pin down any longer due to how extensive they are and how much has been ported back into Canary over time to various degrees. In general Edge will run more games, and run them better, but this code is under active development and there are always regressions so YMMV. If something isn't working on Edge it's worth testing on Canary.

While there is no official compatibility tracker for Xenia Edge at the moment, all known games at least get to gameplay and should render more or less correctly and run at their target framerates or better. At least on sufficiently decent hardware (generally RTX 2xxx class and above). Some games require game-specific config changes, so make sure to check the [Canary compatibility page](https://github.com/xenia-canary/game-compatibility/issues) if something is wrong, the issue is likely already known and has a documented solution.

## Status

Build (Windows / Linux / macOS): [![CI](https://github.com/has207/xenia-edge/actions/workflows/CI.yml/badge.svg?branch=edge)](https://github.com/has207/xenia-edge/actions/workflows/CI.yml) [![Codacy Badge](https://app.codacy.com/project/badge/Grade/cd506034fd8148309a45034925648499)](https://app.codacy.com/gh/has207/xenia-edge/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade)

Releases
--------
[Latest](https://github.com/has207/xenia-edge/releases/latest) ◦ [All](https://github.com/has207/xenia-edge/releases)

FAQ
---

- Q: How to use systemlink?<br>
  A: Systemlink is enabled by default and will work without further configuration with other Edge instances on the same network. If that's all you care about you can stop reading here. To connect to retail consoles you need an instance of [Xlink Kai](https://www.teamxlink.co.uk/) running somewhere on your network, most likely the same PC you're running Xenia but it's not necessary. On RGH/JTAG consoles you have to disable the Nova plugin in Aurora as that replaces normal systemlink on the console. If you plan to connect to console or users over the internet you will need to set ```systemlink_transport = "kai"``` in the Network settings. For remote connections you will need to add them as a friend in the Kai Engine interface once you started it, and they must do the same, or all of you need to be in the same Kai arena for the game. No other configuration should be needed. It's possible to use Kai locally with multiple Edge instances but it requires setting "Allow remote DDS connections" in the Kai settings and pointing the other Edge instances at your machine using ```systemlink_kai_engine = $IP_OF_PC_RUNNING_KAI```, but again you don't *need* Kai for local Xenia-only connections, those will work without any configuration changes using the default ```systemlink_transport = "tunnel"```.

- Q: How to run original Xbox games?<br>
  A: OG Xbox games are supported in the same manner they are on a real console, more precisely an RGH/JTAG console, through ```xefu```. You will want to search for how to run them there, the same information applies here. Start [here](https://consolemods.org/wiki/Xbox_360:Original_Xbox_Games_Compatibility_List). In other words, you'll likely want the "patched" xefu builds that remove restrictions on what games they support. Keep in mind xefu is not a single executable but a "family" where ```xbox.xex``` is the console's dispatcher that selects the appropriately whitelisted ```xefuN.xex```. These have all been community patched to run other games, so you'll want to figure out how it's done on RGH/JTAG setups and mostly follow those instructions. Do not expect compatibility/performance to be quite as good as a real console quite yet, it is still a work in progress, but many games should be playable.

- Q: How to run Xbox Live Indie (XBLIG) games?<br>
  A: XBLIG titles require an XNA runtime and launcher. The launcher comes with the system files, which you can install with ```Tools/Install System Files```. As for the runtime, it's an update for the launcher and you will need to get it the same place you find XBLIG titles themselves. Install the update with ```Tools/Install Content...```. The titles are not proper game packages like everything else and instead install as DLC content for the launcher. You can install them the same way or add the packages where they are with ```File/Add Games...```. Either way each one gets its own entry in the game list and starts from there or from the dashboard's My Games.
  
- Q: How to tell what options a particular game might need?<br>
  A: Many games that are not running well by default require one or two simple config changes that make them (near) perfect. To find out, check the Compatibility section in game details pane. If there is an existing compatibility page there will be a link to Master, Canary, or both. Prefer information on the Canary page, you will often find the answer there.

- Q: Why are translations into language X so bad?<br>
  A: They're AI-generated, if you speak the language and want to help edit the relevant .po file in the assets directory and submit it. If you don't know how to use git then just open a bug and attach the fixed .po file. Best way to edit those is with a program called Poedit. Or just open a bug and tell me what translation is bad and how to improve it.

- Q: Should I use the Windows build with wine/proton on Linux?<br>
  A: The native build for Linux is quite competitive with Windows now and is recommended over wine/proton.

- Q: Why is macOS build not as capable as Windows / Linux?<br>
  A: macOS is the newest port, with a completely different CPU and GPU backends that have not had nearly the amount of testing of the other platforms. Many games will have issues on macOS that run and render fine on Windows and Linux. This is working as intended for now. No need to tell me about issues you find with the macOS port. I know.

- Q: Can you add ARM64 Linux or Windows builds?<br>
  A: Yes, and I choose not to, there is no need to send me PRs or requests for these builds. They will be made available when I'm ready to support them, in the meantime you can build from source if you really want to run them.
  
- Q: Will you accept donations of money or games/hardware for testing?<br>
  A: No, this project is strictly a hobby and I would like to keep it that way. I pay for all games/hardware I test out of my own pocket and do not want to receive any compensation for my work on Xenia.

