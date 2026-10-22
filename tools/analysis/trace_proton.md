# Spike 0.2 runtime API trace (Proton)

Script: `tools/analysis/trace_proton.sh`. Everything it produces lands in `private/` (gitignored).

## What it does

1. Writes Proton's own `user_settings.py` in the tool directory Steam maps Dishonored to (here `Proton - Experimental`),
   setting `PROTON_LOG=1`, `PROTON_LOG_DIR`, `WINEDEBUG` and `PROTON_USE_XALIA=0`. It restores or removes the file on exit.
2. (relay mode) Sets `HKCU\Software\Wine\Debug\RelayFromInclude` / `RelayExclude` in the game's prefix
   (`compatdata/205100/pfx`), removed again on exit.
   - `RelayFromInclude` limits `+relay` output to calls whose caller is a game module (exe, binkw32, PhysX*, APEX*, vorbis,
     msvcr90, steam_api, libcurl). Wine's own kernel32 -> kernelbase -> ntdll chatter is dropped inside Wine.
     Note: Wine appends `.dll` to the module name before comparing, so the list contains `Dishonored.exe.dll`.
   - `RelayExclude` drops a handful of ultra-hot functions (memcpy, _wcsicmp, TlsGetValue, critical sections ...).
     They are known to be used from the import table; their counts from an unfiltered run are in `research/binary/api_surface.md`.
3. Launches the game through Steam (`flatpak run com.valvesoftware.Steam -applaunch 205100 -nostartupmovies`).
4. Streams Proton's raw log (`~/.var/app/com.valvesoftware.Steam/.cache/dishonored-trace/raw/steam-205100.log`, can reach
   GBs) through a filter (`relay_filter.py` or `com_filter.py`) and writes reduced outputs to `private/trace/logs/`.
5. After N seconds (or when the game exits if N=0) kills the game and Proton processes and runs `trace_post.py`, which
   writes the sorted unique list `private/trace/api_called-<tag>.txt` (`dll!Function  count  callers`).

## Why it launches through Steam

Steam is the Flatpak build. Its `/dev/shm` is private to the sandbox, and the game needs the live Steam client
(`steam_api`). Starting Proton by hand, from the host or inside the sandbox, fails with
`steamclient ... fatal stalled cross-thread pipe`. Letting Steam launch the game works, so the script drives Steam.
Do not edit `trace_proton.sh` while it is running (bash reads scripts incrementally).

## Usage

```
tools/analysis/trace_proton.sh relay 480      # 8 minutes, then kill. Win32 calls from game modules.
tools/analysis/trace_proton.sh relay 0        # run until you quit the game yourself (play through a mission, then exit)
tools/analysis/trace_proton.sh com 300        # COM method/CLSID trace (Wine's d3d9 via PROTON_USE_WINED3D=1, dsound, dinput, xinput, ole)
tools/analysis/trace_proton.sh smoke 60       # WINEDEBUG=err+all,+loaddll only
tools/analysis/trace_proton.sh post           # re-run post-processing on the newest relay log
tools/analysis/trace_proton.sh cleanup        # remove user_settings.py + registry values (if a run was interrupted)
```

Environment: `TRACE_TAG` (output name), `DISH_ARGS` (default `-nostartupmovies`), `TOOL` (Proton directory).
Steam must be running and logged in.

## Notes and limits

- Only calls into *Wine builtin* DLLs appear in `+relay`. Native DLLs shipped with the game (PhysX, binkw32, ...) are not
  traced themselves; their calls out to Win32 are. COM method calls (d3d9, dsound, dinput8) go through vtables and never
  show in `+relay`, hence the separate `com` mode which uses Wine's own d3d9/dsound traces. In `com` mode d3d9 is Wine's wined3d
  rather than DXVK, because DXVK is not instrumented with Wine's debug channels.
- The relay log counts are *call counts from one run*, not a proof that a function is never used.
  Gameplay-level coverage (combat, saves, level streaming, cutscenes, options menu, controller hot-plug) needs a human
  to play with `relay 0`.
- The game writes its config/saves into the Proton prefix (`compatdata/205100/pfx/.../Documents/My Games/Dishonored`),
  not into the install directory. Steam itself touched `installscript.vdf` once on first launch (install script run).
- Proton-provided native PE modules (DXVK d3d9/dxgi, steamclient, nvapi) are not counted as "game code" by `relay_filter.py`.
- Do not take desktop screenshots to check progress: they capture the whole desktop, not just the game window.
