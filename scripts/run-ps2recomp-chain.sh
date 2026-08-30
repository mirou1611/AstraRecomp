#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ps2recomp_build="${PS2RECOMP_BUILD:-/home/mirou/astrarecomp-ps2recomp-spike-local/out-host-localdeps}"
analyzer="$ps2recomp_build/ps2xAnalyzer/ps2_analyzer"
recompiler="$ps2recomp_build/ps2xRecomp/ps2_recomp"
elf="$project_dir/build-host/aot-chain.elf"
config="$project_dir/build-host/aot-chain.toml"
generated_dir="$project_dir/build-host/chain-output"

test -x "$analyzer"
test -x "$recompiler"
mkdir -p "$generated_dir"

python3 "$project_dir/tools/make_aot_chain_elf.py" "$elf"
"$analyzer" "$elf" "$config"
# The analyzer defaults every sibling config to build-host/output. Keep this
# corpus separate from T0 so both emitted results remain inspectable.
sed -i 's#build-host/output/#build-host/chain-output/#' "$config"
"$recompiler" "$config"
test -s "$generated_dir/sub_00003000_0x3000.cpp"
test -s "$generated_dir/sub_00003020_0x3020.cpp"

printf 'PS2Recomp call-chain output: %s\n' "$generated_dir"
