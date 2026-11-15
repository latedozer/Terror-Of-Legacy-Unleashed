#!/bin/bash
# Runs the tests:
#   1. the D2RMM mod script under Node, against an extract of the game's vanilla data;
#   2. the plugin DLL under Wine, in a stand-in loader that maps a dump of the game image.
# usage: test/run.sh <vanilla data folder> <game image dump>
#   The data folder needs global/excel (monstats, pettype), hd/global/excel/desecratedzones.json,
#   hd/character/monsters.json and local/lng/strings/ui.json.
#   The dump is the unpacked D2R.exe image (stamp 0x6ab3782c, file offset = RVA).
set -e
cd "$(dirname "$0")/.."
DATA="$1"; DUMP="$2"
[ -d "$DATA" ] && [ -f "$DUMP" ] || { echo "usage: test/run.sh <vanilla data> <image dump>"; exit 2; }
SDK="${SDK:-../../PluginSDK-master/include}"
./build.sh > /dev/null
x86_64-w64-mingw32-g++ -std=c++20 -O1 -Wall -Wno-missing-field-initializers -static -static-libgcc -static-libstdc++ -I "$SDK" -o build/fake_host.exe test/fake_host.cpp
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/mod/TestMod.mpq/data"
node test/run_mod.js ../d2rmm-mod/LegacyOfTerror/mod.js "$DATA" "$WORK/mod/TestMod.mpq/data"
# the stand-in has no game to run the Cow Portal against
awk 'BEGIN{s=""} /^\[/{s=$0} {if(s=="[cow_portal]" && $0=="enabled = true") print "enabled = false"; else print}' legacy-of-terror.toml > "$WORK/test.toml"
cd build
WINEDEBUG=-all timeout 300 wine fake_host.exe "$(winepath -w "$PWD/d2rl-legacy-of-terror.dll")" "$(winepath -w "$DUMP")" "$(winepath -w "$WORK/test.toml")" "$(winepath -w "$WORK/mod")" TestMod | grep -E '^(ok|FAIL|ALL|FAILURES)'
