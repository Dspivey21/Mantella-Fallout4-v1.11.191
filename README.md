# Mantella-Fallout4-v1.11.191
Contains updates to the Mantella source files and updates to F4 DLL's for use with the F4AE 1.11.191

# Mantella for Fallout 4 Anniversary Edition

Mantella v0.14 Preview 1, ported to Fallout 4 Anniversary Edition (v1.11.191).

Mantella v0.13 was built for Fallout 4 pre-AE (v1.10.163). The Anniversary Edition update broke both F4SE plugins (F4SE_HTTP and TopicInfoPatcher) due to changed memory addresses, and the compiled Mantella.exe had a latent Piper TTS deadlock bug. Every component — DLLs, Papyrus scripts, Python backend, config — required fixes. As of this writing, this may be the only working Mantella installation on Fallout 4 Anniversary Edition.

**Pipeline:** Player speaks → Moonshine STT → LLM (KoboldCpp/Mistral-Nemo) → Piper TTS → NPC speaks in-game

---

## Quick Start

### Requirements

- **Windows 10/11**
- **Python 3.11+** — required (Mantella uses `StrEnum` which was added in 3.11; Python 3.10 will NOT work)
  - Download: https://www.python.org/downloads/release/python-3119/ (Windows installer 64-bit)
- **Fallout 4 Anniversary Edition** (v1.11.191)
- **F4SE** installed for AE — https://f4se.silverlock.org/
- **Address Library for F4SE Plugins** — required for F4SE to work on AE. Install from Nexus Mods: https://www.nexusmods.com/fallout4/mods/47327

### Step 1: Install Mantella from Source

Clone or download the official Mantella repository:

```
git clone https://github.com/art-from-the-machine/Mantella.git
```

Or download the ZIP from https://github.com/art-from-the-machine/Mantella and extract it to a folder (e.g. `C:\Mantella-src`).

### Step 2: Set Up the Python Environment

Open a terminal in the Mantella source folder:

```
cd C:\Mantella-src
py -3.11 -m venv MantellaEnv
.\MantellaEnv\Scripts\Activate.ps1
```

Install dependencies (two steps — setuptools must be installed first):

```
pip install --upgrade pip
pip install "setuptools==70.3.0" wheel
pip install --no-build-isolation -r requirements.txt
```

> **Why two steps?** Setuptools v71+ removed the `pkg_resources` module that Moonshine's setup.py depends on. It must be pinned to v70.3.0 *before* installing the rest of the requirements. The requirements file pins all other versions including `onnxruntime==1.19.2` (v1.24.3 fails with `DLL load failed` on Python 3.11).

### Step 3: Apply the AE Changes

This repository contains three folders. Each one goes to a different place:

| Folder | What's Inside | Where to Copy It |
|--------|--------------|-----------------|
| `Mantella-SRC- Changes\` | Modified Python files (`piper.py`, `game_manager.py`) | Copy into your Mantella source folder, overwriting the originals. For example, `Mantella-SRC- Changes\src\tts\piper.py` → `C:\Mantella-src\src\tts\piper.py` |
| `Data\` | Rewritten DLLs, compiled Papyrus scripts (`.pex`), Papyrus source (`.psc`) | Copy the entire `Data` folder into your Fallout 4 game directory (e.g. `C:\Steam\steamapps\common\Fallout 4\`), merging with the existing `Data` folder. This overwrites the old DLLs and scripts. |
| `DLL-Source-Code\` | C++ source code for the three rewritten DLLs | **You don't need to copy this anywhere.** This is the source code for the DLLs in case you want to review or rebuild them. |

**What gets overwritten:**

In your Mantella source folder:
- `src\tts\piper.py` — Piper TTS stderr deadlock fix
- `src\game_manager.py` — Actions format compatibility fix

In your Fallout 4 `Data` folder:
- `F4SE\Plugins\F4SE_HTTP.dll` — Rewritten for AE
- `F4SE\Plugins\TopicInfoPatcher.dll` — Rewritten for AE
- `Scripts\F4SE_HTTP.pex` — Updated compiled script
- `Scripts\MantellaConversation.pex` — Bug fixes
- `Scripts\MantellaListenerScript.pex` — AE version detection
- `Scripts\TopicInfoPatcher.pex` — Updated compiled script
- `Scripts\Source\User\*.psc` — Updated source scripts

**Also included:**
- `F4SE\Plugins\F4MantellaLauncher.dll.bak` — The auto-launcher DLL, pre-renamed to `.bak` so it won't interfere with running from source
- `Scripts\Source\User\OLD SRC SCRPTS\` — Backup of the original unmodified Papyrus scripts in case you need to revert

### Step 4: Set Up Piper TTS

Mantella needs Piper and its voice models. If you haven't already set these up, follow the Mantella documentation for Piper TTS setup. The Piper directory should end up at `Data\F4SE\Plugins\MantellaSoftware\piper\` in your Fallout 4 game folder.

### Step 5: Run

1. Activate the venv: `.\MantellaEnv\Scripts\Activate.ps1`
2. Start Mantella: `python main.py`
3. Verify "Waiting for player to select an NPC..." appears
4. Launch Fallout 4
5. Talk to an NPC in-game

### Reverting to Compiled Mantella.exe

1. Stop `python main.py`
2. Rename `F4MantellaLauncher.dll.bak` back to `F4MantellaLauncher.dll` in `Data\F4SE\Plugins\`
3. Launch Fallout 4 normally (the DLL will auto-start Mantella.exe)
4. Note: the compiled exe is v0.13 and will not have any of these fixes

---

## Config

**File:** `%USERPROFILE%\Documents\My Games\Mantella\config.ini`

| Setting | Value | Note |
|---------|-------|------|
| game | Fallout4 | |
| tts_service | Piper | Switched from XTTS (too slow on shared VRAM) |
| stt_service | Moonshine | Local STT, no API needed |
| llm_api | KoboldCpp | Local LLM via Mistral-Nemo |
| pause_threshold | 0.5+ | Increase if NPC cuts you off mid-sentence |
| port | 4999 | Default Mantella port |

---

## File Locations

All paths relative to your Fallout 4 game directory unless marked with `%USERPROFILE%`.

| What | Path |
|------|------|
| Mantella source | wherever you cloned/extracted the Mantella repo |
| Mantella venv | `<mantella-src>\MantellaEnv\` |
| Mantella config | `%USERPROFILE%\Documents\My Games\Mantella\config.ini` |
| Mantella log | `%USERPROFILE%\Documents\My Games\Mantella\logging.log` |
| Piper directory | `Data\F4SE\Plugins\MantellaSoftware\piper\` |
| Piper voice models | `Data\F4SE\Plugins\MantellaSoftware\piper\models\fallout4\low\` (~107 models, ~63MB each) |
| Papyrus scripts | `Data\Scripts\Source\User\` |
| F4SE plugins | `Data\F4SE\Plugins\` |
| F4SE logs | `%USERPROFILE%\Documents\My Games\Fallout4\F4SE\` |
| Papyrus log | `%USERPROFILE%\Documents\My Games\Fallout4\Logs\Script\Papyrus.0.log` |

### Files Still Required in Game Folder

| Path | Size | Purpose |
|------|------|---------|
| `MantellaSoftware\piper\` | 6.4 GB | piper.exe + voice models — Python code launches this as subprocess |
| `MantellaSoftware\data\Fallout4\` | ~8 MB | Character CSV, placeholder.lip, voice folder mappings |
| `MantellaSoftware\data\actions\` | small | Action definitions (follow, inventory, offend/forgive) |
| `MantellaSoftware\data\*.wav` | small | Audio cues (ready sound, no mic detected) |

### Files No Longer Needed

| Path | Size | Reason |
|------|------|--------|
| `MantellaSoftware\Mantella.exe` | 382 MB | Running from source now; this is the compiled v0.13 binary |
| `MantellaSoftware\Mantella.ico` | tiny | Icon for the compiled exe |
| `MantellaSoftware\data\Skyrim\` | small | Not playing Skyrim |

---

## What Was Changed and Why

### Python Fixes (2 files)

**Piper TTS Stderr Pipe Deadlock** — `src\tts\piper.py`

Piper.exe writes status messages to stderr. The Python subprocess captured stderr but never read from it. When the OS pipe buffer filled (~4-64KB on Windows), piper.exe blocked on its next write, freezing completely. This caused 20 synthesis timeouts (all ~5.6 seconds) in a single session. After Mantella restarted Piper, it worked briefly until the buffer filled again. This is a well-known Python subprocess pitfall.

We added a background thread that continuously reads and logs Piper's stderr, preventing the buffer from ever filling. We also added structured logging so each synthesis attempt can be traced through the log, and increased the timeout from 5s to 15s as a safety margin.

Result: zero timeouts after fix. Piper synthesis typically completes in under 0.1 seconds.

**Actions Format Incompatibility** — `src\game_manager.py`

Saying "goodbye" triggered Mantella's end sequence correctly (NPC said "Safe travels"), but the conversation never actually closed in-game. The player was stuck and couldn't talk to any other NPC without restarting Mantella.

The v0.14 source sends actions as a list of dicts (`[{"identifier": "mantella_end_conversation"}]`) but the v0.13 Papyrus scripts expect a list of strings (`["mantella_end_conversation"]`). The Papyrus `F4SE_HTTP.getStringArray()` received dicts, got nothing back, and never triggered the end conversation logic.

We added a flattening step in `sentence_to_json()` that converts the v0.14 dict format back to the v0.13 string format before sending to the game. This applies to all actions (end conversation, follow, offend, forgive, etc.), not just goodbye.

### Papyrus Fixes (4 scripts)

**Off-By-One in CauseReassignmentOfParticipantAlias()** — `MantellaConversation.psc`

The loop started at `i = Participants.GetSize()` and checked `While i > 0`, which meant the first iteration accessed an index past the end of the array. Changed to `i = GetSize() - 1` and `While i >= 0`, and added a null check before calling `EvaluatePackage()`.

**Null Reference in ClearAllFunctionTargets()** — `MantellaConversation.psc`

The function assumed the actor was always valid when clearing faction data. When it wasn't, calling `SetFactionRank()` and `RemoveFromFaction()` on a None reference caused crashes. Wrapped both calls in a null check.

**AE Version Detection** — `MantellaListenerScript.psc`

Added recognition for the AE version string (`"1.11.191.0"`) so the startup notification displays correctly instead of showing nothing.

**Element-Wise Array Access** — `F4SE_HTTP.psc`

The original declared `getStringArray` as a single native function that retrieved an entire array in one DLL call. This crashed on AE. Replaced it with two new natives (`getStringArraySize` + `getStringArrayElement`) that read one element at a time, plus a Papyrus wrapper that rebuilds the full array. Transparent to all callers — same function signature, same behavior.

**Form Pointer to Integer ID** — `TopicInfoPatcher.psc`

The original natives accepted form pointers directly, which crashed on AE due to a different calling convention. Replaced with integer form ID natives plus Papyrus wrappers that convert forms to IDs before passing them to the DLL. Also split `GetLastActorCoords()` (which returned a vector — broken on AE) into three individual coordinate natives.

### Config Fix

**Bare "Follow:" LLM Responses** — `config.ini`

The LLM sometimes returned just `Follow:` with no dialogue text after it, causing empty TTS voicelines. Added prompt instructions in `fallout4_prompt` and `fallout4_multi_npc_prompt` requiring the LLM to always include at least one full sentence of spoken dialogue after any action prefix. This is a config change you'll need to make yourself in your own `config.ini`.

### Binary Fix

**Placeholder LIP File** — `placeholder.lip`

The existing placeholder.lip had a garbage header that caused game crashes during dialogue playback. Replaced with a valid empty header so the game gracefully skips lip sync animation. This file is included in the `Data` folder.

---

## DLL Rewrites

Both F4SE plugins were rewritten from scratch for AE v1.11.191. The originals crashed on AE because CommonLibF4's address library IDs changed, meaning every game function lookup returned garbage pointers. The DLL source code is included in the `DLL-Source-Code` folder for reference.

### F4SE_HTTP.dll

Provides Papyrus native functions for HTTP GET/POST so in-game scripts can communicate with Mantella's Python backend over localhost.

**Original:** [Leidtier/F4SE_HTTP](https://github.com/Leidtier/F4SE_HTTP) (~364 lines)
**Rewrite:** ~764 lines — same 25 Papyrus native functions, completely different internals

What changed:

- **String handling:** The original's BSFixedString operations crashed when processing string literals stored in read-only memory (.rdata). We installed a VectoredExceptionHandler (VEH) that catches the ACCESS_VIOLATION, makes the page writable, and resumes execution. Scoped to our DLL only.
- **Dictionary storage:** Replaced the external `SKSE_HTTP_TypedDictionary` library with a self-contained storage system. No external dependencies.
- **Array access:** The original's bulk `getStringArray` native crashed on AE. Replaced with element-wise natives that read one element at a time, with a Papyrus wrapper that rebuilds the full array — transparent to all callers.
- **Response notification:** Replaced the broken `SendPapyrusEvent` callback with polling + keypress injection (scancode 0x97) to wake the Papyrus VM when HTTP responses arrive.
- **Game API:** Uses F4SE's own plugin API instead of CommonLibF4's address-library-dependent wrappers.
- **Dependencies:** Only CPR (HTTP client) and nlohmann/json (JSON parsing).

### TopicInfoPatcher.dll

Hooks the game's dialogue system to replace NPC response text with Mantella-generated text in real time (so NPCs "speak" the LLM-generated lines).

**Original:** [peteben/TopicInfoPatcher](https://github.com/peteben/TopicInfoPatcher) (~324 lines)
**Rewrite:** ~841 lines — direct RVA calls replacing all CommonLibF4 wrappers

What changed:

- **Form lookup:** Bypassed CommonLibF4 entirely. We found the real `GetFormByID` function in Fallout4.exe AE at RVA `0x311850` and call it directly. No address library needed.
- **Calling convention:** Switched from form pointers and vector return types (broken on AE) to integer form IDs with no vector returns.
- **Logging:** Every successful text patch is logged with form ID and content for easy verification.

### F4MantellaLauncher.dll

Auto-launches Mantella.exe when Fallout 4 starts. Included as `.dll.bak` (disabled) since running from source means you start Mantella manually. If the Mantella author eventually compiles v0.14 to an exe, this DLL can be renamed back to `.dll` to re-enable auto-launch.

---

## Papyrus Script Summary

4 of Mantella's 10 Papyrus scripts were modified. The rest were used as-is from v0.13.

| Script | What Changed |
|--------|-------------|
| `F4SE_HTTP.psc` | Element-wise array access replacing the broken bulk native |
| `TopicInfoPatcher.psc` | Integer form ID natives replacing broken form pointer natives |
| `MantellaConversation.psc` | Off-by-one fix and null reference fix |
| `MantellaListenerScript.psc` | AE version string recognition |

**Unmodified:** `MantellaConstants.psc`, `MantellaEffectScript.psc`, `MantellaRepository.psc`, `MantellaLauncher.psc`, `MantellaIsTalkingEffectScript.psc`, `MantellaNPCIsUsingItemEffectScript.psc`

### How the HTTP Communication Works

- **Non-VR (default):** The script registers for key `0x97`. When the DLL receives an HTTP response from Python, it injects a keypress (scancode 0x97) to wake the Papyrus VM. The `OnKeyDown` event retrieves the response data.
- **VR mode:** Uses timer-based polling since VR can't reliably receive injected keypresses.
- The original used `SendPapyrusEvent` callbacks which broke on AE — the polling + keypress injection model is the replacement.

