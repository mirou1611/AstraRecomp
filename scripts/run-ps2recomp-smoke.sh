#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ps2recomp_build="${PS2RECOMP_BUILD:-/home/mirou/astrarecomp-ps2recomp-spike-local/out-host-localdeps}"
analyzer="$ps2recomp_build/ps2xAnalyzer/ps2_analyzer"
recompiler="$ps2recomp_build/ps2xRecomp/ps2_recomp"
elf="$project_dir/build-host/astrarecomp-smoke.elf"
config="$project_dir/build-host/astrarecomp-smoke.toml"
generated="$project_dir/build-host/output/sub_00001000_0x1000.cpp"

test -x "$analyzer"
test -x "$recompiler"
mkdir -p "$project_dir/build-host/output"

python3 "$project_dir/tools/make_smoke_elf.py" "$elf"
"$analyzer" "$elf" "$config"
"$recompiler" "$config"
test -s "$generated"

printf 'PS2Recomp Phase-0 smoke output: %s\n' "$generated"
