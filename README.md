# aegidub

**aegidub** is an AI dubbing editor built on [Aegisub](https://github.com/TypesettingTools/Aegisub). It takes a video and its subtitles and turns them into a dubbed video: it translates the lines, works out who says each one, gives every character a voice, directs the emotion, speaks the lines with [ElevenLabs](https://elevenlabs.io), and mixes the dub over the video's own background sound.

It was made for dubbing films and series into **Mongolian**, but works for any language your AI model and ElevenLabs voices support.

[![Download for Windows](https://img.shields.io/github/v/release/tuvshinorg/aegidub?label=Download%20for%20Windows&logo=windows&style=for-the-badge)](https://github.com/tuvshinorg/aegidub/releases/latest)

[Монгол хэлээр унших](README.mn.md)

## What's the same as Aegisub

aegidub keeps all of Aegisub. If you know Aegisub, you know aegidub:

- The same subtitle editor: the grid, the edit box, timing, styles, the audio waveform and spectrum, video playback with subtitles drawn by libass, visual typesetting tools, karaoke, Lua/MoonScript automation and every subtitle format Aegisub reads and writes.
- `.ass` files stay compatible both ways. Everything aegidub adds (voices, emotions, the original text) is stored in places Aegisub keeps but ignores, so Aegisub and other players open aegidub's files normally.
- The same hotkeys, menus and settings, plus new ones.
- The same licence and credits: aegidub is a fork, and Aegisub's authors keep their copyright.

## What's different

| Feature | What it does |
|---|---|
| **Projects** | A start screen like CapCut's: each video is a project card with a thumbnail, named by date and hash, e.g. `20261005-a1b2c3d4e5f6`. A project folder holds the subtitles, generated speech and renders. |
| **AI Translate** | Translates lines with an OpenAI-compatible model, keeping the original text in an *Original* column so it can be restored or re-translated. |
| **AI Detect Speakers** | Guesses who says each line from the dialogue and the show's characters, and fills the *Character* column. |
| **Detect Speakers from Audio** | Separates the voices from the music, groups the lines by how they sound with **pyannote**, then has the model name the character of each voice. More reliable than the text alone for short lines like "What?!". The first run installs pyannote (needs Python). |
| **Voice Cast** | Assigns an ElevenLabs voice to each character. *Auto-detect* casts from the voices ElevenLabs verified for Mongolian, matching gender, age and personality. *Play* speaks one of the character's own lines. |
| **Series cast** | A `.cast.json` file shared by all episodes of a series, so characters keep their voices from episode to episode. |
| **AI Detect Emotions** | Fills an *Emotion* column with ElevenLabs audio tags such as `[sad]` or `[whispers]`. Click a cell to change one. |
| **Spoken text** | Numbers, symbols and foreign words are written out for the speech engine (`10:30` becomes "арван цаг гучин минут") while the subtitle shows `10:30`. |
| **Generate Dub Track** | Speaks every line with its character's voice and emotion and builds one audio track timed to the subtitles. Unchanged lines are reused, so you only pay for what changed. |
| **Fit column** | Like CPS, but measured on the real speech: how much of the time before the next line each line's audio takes. Red means it runs over. |
| **AI Shorten Long Lines** | Rewrites lines that don't fit, shorter but with the same meaning, telling the model how long each one is and how long it may be. |
| **Render Dubbed Video** | Separates the video's voices from its music and effects with **htdemucs**, mixes the dub over the background (optionally with the original voices quietly underneath), can remaster the loudness, and writes a new video with the dub as the default track. |
| **Built-in voice separation** | htdemucs runs inside the program on the CPU, with nothing to install. If Python with demucs and an NVIDIA GPU is present it is used instead, about ten times faster. |
| **Updates** | Checks this repository's GitHub releases only, and always asks before updating. Nothing about your computer is sent. |

## The dubbing workflow

1. **Projects → New project**: choose the video, and import subtitles if you have them.
2. **AI Translate**, if the subtitles aren't in your language yet.
3. **AI Detect Speakers**, then fix any wrong names in the *Character* column.
4. **Voice Cast → Auto-detect voices**, listen with *Play*, and save the series cast.
5. **AI Detect Emotions**, and adjust the *Emotion* column by ear with **Play Dub of Line**.
6. **Generate Dub Track**, then **AI Shorten Long Lines** for anything red in the *Fit* column.
7. **Render Dubbed Video**.

## Installing

Download `aegidub-…-x64-setup.exe` from the [releases](https://github.com/tuvshinorg/aegidub/releases) and run it. The installer installs the Microsoft Visual C++ runtime itself if the computer doesn't have a recent one.

The installer isn't code-signed yet, so Windows may show "Windows protected your PC". Click **More info**, then **Run anyway**.

Requirements:

- Windows 10 or 11, 64-bit, with a processor that supports AVX2 (most made since 2013) for the built-in voice separator.
- An **OpenAI** API key (or any OpenAI-compatible service) for translation, speakers, casting, emotions and shortening: *Preferences → AI Translation*.
- An **ElevenLabs** API key for the voices: *Preferences → Voice Cast*.
- Optional, for fast voice separation: an NVIDIA GPU and Python with `demucs`, `torch` (CUDA) and `soundfile`.
- Optional, for *Detect Speakers from Audio*: Python 3. aegidub installs pyannote itself the first time; an NVIDIA GPU makes it much faster.

The voice separation model (84 MB) is downloaded once, the first time it is needed.

## Privacy and costs

- API keys are stored only in your own settings (`%APPDATA%\aegidub\config.json`) or environment variables (`OPENAI_API_KEY`, `ELEVENLABS_API_KEY`). They are never part of the program or the project files.
- The AI features send subtitle text to the AI service you configure, and the voice features send it to ElevenLabs. Both charge for what you use. aegidub asks before every batch, and reuses generated speech so nothing is paid for twice.
- Rendering and voice separation happen on your computer.

## Building from source (Windows)

Prerequisites: Visual Studio 2022 (or its Build Tools) with the Windows SDK, Python 3 and Meson (`pip install meson`). Ninja ships with Visual Studio.

From an "x64 Native Tools Command Prompt":

```
meson setup build-release --buildtype=release -Ddefault_library=static
ninja -C build-release aegidub.exe
```

All other dependencies, including FFmpeg, wxWidgets, Eigen and the C++ htdemucs port, are downloaded and built by Meson.

To build the installer, install [Inno Setup 6](https://jrsoftware.org/isinfo.php) and run:

```
powershell tools\build-aegidub-installer.ps1
```

### Releasing

Push a version tag and GitHub Actions does the rest:

```
git tag v1.0.0
git push origin v1.0.0
```

The *Release* workflow builds aegidub from the tag (so the program knows its version), makes the installer and publishes a GitHub release with it attached. Installed copies of aegidub then offer the update.

The program and installer are signed through [SignPath](https://signpath.io) once the repository variable `SIGNPATH_ORGANIZATION_ID` and the secret `SIGNPATH_API_TOKEN` are set; each release then waits for its signing request to be approved in SignPath. The SignPath project is `aegidub`, with the signing policy `release-signing` and [`.signpath/artifact-configuration.xml`](.signpath/artifact-configuration.xml) as its artifact configuration. Alternatively, sign with your own certificate by adding the secrets `WINDOWS_SIGN_PFX_BASE64` (a base64-encoded `.pfx`) and `WINDOWS_SIGN_PFX_PASSWORD`; locally, set `SIGN_PFX` and `SIGN_PFX_PASSWORD` before running the installer script. Without either, everything still builds, unsigned.

## Code signing policy

Free code signing provided by [SignPath.io](https://signpath.io), certificate by [SignPath Foundation](https://signpath.org).

Only builds made by this repository's GitHub Actions from its source are signed, and every release is approved by hand before signing.

- Committers and reviewers: [@tuvshinorg](https://github.com/tuvshinorg)
- Approvers: [@tuvshinorg](https://github.com/tuvshinorg)

## Privacy policy

aegidub sends data only to services you configure or ask it to use:

- Subtitle text to the AI model endpoint you set (by default `api.openai.com`), when you run an AI command.
- Lines and voice settings to ElevenLabs (`api.elevenlabs.io`), when you cast voices or generate speech.
- A request to the GitHub API for this repository's releases, when it checks for updates.
- A download of the voice separation model from Hugging Face, once, the first time it is needed.

Nothing else about you or your computer is collected or sent.

To change the logo, replace `docs/art-sources/aegidubLogo.png` and run `python tools/generate_logo_assets.py`.

## Licence and credits

aegidub is based on [Aegisub](https://github.com/TypesettingTools/Aegisub), © 2005–2026 Rodrigo Braz Monteiro, Niels Martin Hansen, Thomas Goyne and the Aegisub team, under the BSD licence in [LICENCE](LICENCE).

The built-in voice separator is [demucs.cpp](https://github.com/sevagh/demucs.cpp) (MIT) running Meta's [htdemucs](https://github.com/facebookresearch/demucs) model. Speakers are told apart with [pyannote.audio](https://github.com/pyannote/pyannote-audio) (MIT) and its [speaker-diarization-community-1](https://huggingface.co/pyannote/speaker-diarization-community-1) model by pyannoteAI, included under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Speech is generated by [ElevenLabs](https://elevenlabs.io). See *Help → About* for the other libraries.

Bugs and ideas: [github.com/tuvshinorg/aegidub/issues](https://github.com/tuvshinorg/aegidub/issues).
