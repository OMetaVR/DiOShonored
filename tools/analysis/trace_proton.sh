#!/usr/bin/env bash
# Spike 0.2: trace which Win32 / COM APIs Dishonored.exe (and the DLLs we would recompile) really call, under Proton.
# Background and findings: research/binary/api_surface.md (section "Runtime trace") and tools/analysis/trace_proton.md.
#
# Design (why it is not a plain `proton run`):
#  * Steam here is the Flatpak build. Its /dev/shm is private to the sandbox and the game needs a live Steam client for
#    steam_api. Starting Proton by hand (host or inside the sandbox) ends in "steamclient ... fatal stalled cross-thread
#    pipe". Letting Steam itself launch the game (-applaunch 205100) works, so the script drives Steam and injects
#    its settings through Proton's own supported hook, <Proton tool>/user_settings.py (removed again on exit).
#  * Relay is restricted to calls whose *caller* is a game module via the Wine registry value
#    HKCU\Software\Wine\Debug\RelayFromInclude (set in the game's own prefix, removed again on exit).
#  * The game directory is not touched by this script. (Steam/the game may write logs/config inside the install; compare
#    mtimes with `find <game> -newer <marker>` if that matters.)
#
# usage: trace_proton.sh <mode> [seconds]       seconds=0 -> wait until the game exits (play manually, then quit)
#   relay   +relay (caller-filtered) + +loaddll  -> private/trace/logs/relay-<tag>.* and private/trace/api_called-<tag>.txt
#   com     PROTON_USE_WINED3D=1 and d3d9/dsound/dinput/xinput/ole channels -> private/trace/logs/com-<tag>.*
#   smoke   WINEDEBUG=err+all,+loaddll (does it start / reach Steam init?)
#   post    re-run the post-processing for the newest relay log
#   cleanup remove user_settings.py + registry values (safe to run any time the game is not running)
# env: TRACE_TAG, DISH_ARGS (default -nostartupmovies), TOOL (Proton dir; default = the tool Steam maps the game to)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HOST_STEAM="$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"
GAME="$HOST_STEAM/steamapps/common/Dishonored"
TOOL="${TOOL:-$HOST_STEAM/steamapps/common/Proton - Experimental}"
PFX="$HOST_STEAM/steamapps/compatdata/205100/pfx"
MODE="${1:-relay}"; SECS="${2:-240}"
TAG="${TRACE_TAG:-$(date +%Y%m%d-%H%M%S)}"
LOGDIR="$ROOT/private/trace/logs"; mkdir -p "$LOGDIR"
# Proton writes its raw log here (path as seen from inside the Flatpak/pressure-vessel; host path below)
RAW_HOST="$HOME/.var/app/com.valvesoftware.Steam/.cache/dishonored-trace/raw"; RAW_SBX="$HOME/.cache/dishonored-trace/raw"
mkdir -p "$RAW_HOST"
PY="$ROOT/tools/.venv/bin/python -I"
DISH_ARGS="${DISH_ARGS:--nostartupmovies}"
US="$TOOL/user_settings.py"
WINE="$TOOL/files/bin/wine"

reg() { WINEPREFIX="$PFX" WINEDEBUG=-all "$WINE" reg "$@" >/dev/null 2>&1; }
cleanup() {
  [ -f "$US.dishonored-trace.bak" ] && mv -f "$US.dishonored-trace.bak" "$US" || rm -f "$US"
  reg delete 'HKCU\Software\Wine\Debug' /f
  WINEPREFIX="$PFX" "$TOOL/files/bin/wineserver" -w 2>/dev/null
}
game_pids() { pgrep -f '^[A-Za-z]:\\.*Dishonored\.exe' ; }
stop_game() {
  local p; p="$(game_pids)"
  [ -n "$p" ] && kill $p 2>/dev/null; sleep 4
  p="$(game_pids)"; [ -n "$p" ] && kill -9 $p 2>/dev/null
  # whatever Proton/Steam left in the game's prefix
  pgrep -f 'steamapps/compatdata/205100' | while read -r q; do kill "$q" 2>/dev/null; done
}
if [ "$MODE" = cleanup ]; then cleanup; echo cleaned; exit 0; fi
if [ "$MODE" = post ]; then
  f="$(ls -t "$LOGDIR"/relay-*.calls.tsv | head -1)"; b="${f%.calls.tsv}"
  $PY "$ROOT/tools/analysis/trace_post.py" relay "$b" "$ROOT/private/trace/api_called-$(basename "$b").txt"; exit 0
fi
if [ -n "$(game_pids)" ]; then echo "Dishonored is already running; quit it first"; exit 1; fi
trap 'stop_game; cleanup' EXIT

# ---- configuration for this mode
FROM_MODS="Dishonored binkw32 PhysXLoader PhysXCore PhysXExtensions PhysXCooking NxCharacter APEX_release APEX_BasicIOS APEX_Clothing APEX_Destructible APEX_Emitter APEX_IOFX APEX_Particles APEX_RRB ogg vorbis vorbisfile msvcr90 msvcp90 steam_api libcurl"
EXCL="msvcr90.memcpy;msvcr90.memmove;msvcr90.memset;msvcr90._wcsicmp;msvcr90._wcsnicmp;msvcr90._stricmp;msvcr90.wcsstr;msvcr90.wcsrchr;msvcr90.strncpy_s;msvcr90.strcpy_s;kernel32.lstrlenW;kernel32.WideCharToMultiByte;kernel32.Sleep;ntdll.RtlEnterCriticalSection;ntdll.RtlLeaveCriticalSection;ntdll.RtlTryEnterCriticalSection;kernel32.TlsGetValue;kernel32.TlsSetValue;kernel32.GetLastError;kernel32.SetLastError;kernel32.InterlockedIncrement;kernel32.InterlockedDecrement;kernel32.InterlockedExchange;kernel32.InterlockedCompareExchange;kernel32.InterlockedExchangeAdd;kernel32.EnterCriticalSection;kernel32.LeaveCriticalSection;kernel32.QueryPerformanceCounter;kernel32.GetTickCount;kernel32.GetCurrentThreadId;kernel32.FlsGetValue;kernel32.FlsSetValue;kernel32.HeapAlloc;kernel32.HeapFree;kernel32.HeapReAlloc;kernel32.HeapSize"
EXTRA=""
case "$MODE" in
  relay) WD="-all,+relay,+loaddll"
         FROM=""; for m in $FROM_MODS; do FROM="$FROM$m.dll;$m;$m.exe;$m.exe.dll;"; done
         reg add 'HKCU\Software\Wine\Debug' /v RelayFromInclude /t REG_SZ /d "$FROM" /f
         reg add 'HKCU\Software\Wine\Debug' /v RelayExclude /t REG_SZ /d "$EXCL" /f
         FILTER="$PY '$ROOT/tools/analysis/relay_filter.py' '$GAME/Binaries/Win32' '$LOGDIR/relay-$TAG' --raw-cap-mb 512";;
  com)   WD="-all,+loaddll,+d3d9,+dsound,+dinput,+xinput,+ole"; EXTRA='"PROTON_USE_WINED3D": "1",'
         FILTER="$PY '$ROOT/tools/analysis/com_filter.py' '$LOGDIR/com-$TAG' --raw-cap-mb 256";;
  smoke) WD="-all,err+all,+loaddll"; FILTER="cat > '$LOGDIR/smoke-$TAG.log'";;
  *) echo "unknown mode $MODE"; exit 2;;
esac
[ -f "$US" ] && cp -f "$US" "$US.dishonored-trace.bak"
cat > "$US" <<PYEOF
user_settings = {
    "PROTON_LOG": "1",
    "PROTON_LOG_DIR": "$RAW_SBX",
    "WINEDEBUG": "$WD",
    "PROTON_USE_XALIA": "0",
    $EXTRA
}
PYEOF
RAWLOG="$RAW_HOST/steam-205100.log"; rm -f "$RAWLOG"

# ---- launch via Steam and stream the raw log through the filter
( until [ -f "$RAWLOG" ]; do sleep 1; done; tail -n +1 -F "$RAWLOG" 2>/dev/null | eval "$FILTER" ) &
FPID=$!
flatpak run com.valvesoftware.Steam -applaunch 205100 $DISH_ARGS >/dev/null 2>&1 &
echo "launched via Steam; mode=$MODE tag=$TAG seconds=$SECS (0 = until the game exits)"
for _ in $(seq 1 90); do [ -n "$(game_pids)" ] && break; sleep 1; done
if [ "$SECS" -gt 0 ]; then sleep "$SECS"; stop_game; else while [ -n "$(game_pids)" ]; do sleep 2; done; fi
sleep 3
pkill -P $FPID tail 2>/dev/null; pgrep -f "tail -n \+1 -F $RAWLOG" | xargs -r kill 2>/dev/null
wait $FPID 2>/dev/null
if [ "$MODE" = relay ]; then $PY "$ROOT/tools/analysis/trace_post.py" relay "$LOGDIR/relay-$TAG" "$ROOT/private/trace/api_called-$TAG.txt"; fi
ls -la "$RAWLOG" 2>/dev/null
