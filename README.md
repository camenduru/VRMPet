# VRMPet

A VRM avatar that stands on your Windows desktop and talks, using Fish Audio for
speech-to-text and text-to-speech plus its agent as the LLM. The avatar is drawn
into a layered, click-through overlay window, so there is no frame and no
background - just the pet.

## Build

From Git Bash (it sets up the MSVC toolchain and Ninja for you):

```sh
./build.sh            # incremental
./build.sh --fresh    # wipe build/ and rebuild everything
```

The result is `build/run/VRMPet.exe`, with `assets/` and `config.txt` staged
beside it.

## Run

```sh
cd build/run
./VRMPet.exe
```

That is overlay mode: the avatar appears in the bottom-right corner and waits to
be spoken to. Double-check the API key and agent are set first (see below).

## Configure

Settings live in `config.txt` next to `VRMPet.exe` (a copy with every key and a
comment is in the repo root). It is read once, at startup.

At minimum, set these two:

```ini
FISH_API_KEY = <your Fish Audio key>
AGENT        = <your agent's name on fish.audio>
```

Other useful keys:

| key | meaning |
| --- | --- |
| `MODEL` | TTS voice model (default `drama-3`) |
| `GREET` | what the pet says on startup; empty = wait |
| `avatar.zoom` | pet size, `1.0` as framed, up to `4.0` |
| `band.visible` | `0` to start with no speech bubbles |
| `overlay.margin` | gap in px from the screen edge |

To regenerate a fresh file with every key:

```sh
./VRMPet.exe config > config.txt
```

## Controls

Right-click the pet:

- **Close VRMPet**
- **Mute / unmute the microphone**
- **Send the clipboard as a message**
- **Show transcript bubbles** - toggle the speech band on/off
- **Open config.txt**
- **Return to the corner**
- drag the pet to move it

Keyboard (global):

| keys | action |
| --- | --- |
| `Ctrl+Shift+V` | paste the clipboard as a chat message |
| `Ctrl+Shift+H` | widen/narrow the transcript band |
| `Ctrl+Shift+M` | mute/unmute the microphone |
| `Ctrl+Shift+Q` | quit |

## Command line

```sh
VRMPet.exe [overlay] [model.vrm] [azimuth] [msaa] [seconds] [anim.vrma]
VRMPet.exe ask "hello"     # overlay + one scripted turn, then quit
```

Probes run one leg on its own, with no window and no network where possible:

```sh
VRMPet.exe agent "hello"   # one agent turn, text only
VRMPet.exe tts "hello"     # one line of speech, written as wav
VRMPet.exe asr out/turn.wav
VRMPet.exe mic 10          # the microphone and speech gate
VRMPet.exe bubble "hello"  # the bubble band alone, written to a BMP
VRMPet.exe config          # the complete config.txt, to stdout
```

## Notes

- `avatar.zoom` scales the render target, so the pet grows without being
  cropped. Higher zoom renders a larger buffer (zoom `4.0` is `4096x4096`), so
  keep it modest if the frame rate matters.
- If the pet shows but never replies, it is almost always `FISH_API_KEY` or
  `AGENT` missing from the `config.txt` beside the exe; the log line
  `VRMPet: config ... (N keys set)` tells you what was read.
