#!/bin/sh
# Reproduce config/sdk_symbols.txt. Usage: sh tools/sdkid/run.sh REFDIR   (run from project root)
# REFDIR: empty scratch directory for downloaded reference decomp asm (untrusted data, never executed).
set -e
R="$1"; P=$(pwd); B=$P/build/sdkid; mkdir -p "$R" "$B"
clone() { [ -d "$R/$1" ] || git clone -q --filter=blob:none --no-checkout "https://github.com/$2.git" "$R/$1"; }
snap() { # name repo-dir commit paths...
  n=$1; d=$2; c=$3; shift 3
  [ -d "$R/snap_$n" ] && return
  (cd "$R/$d" && git sparse-checkout set --no-cone "$@" && git checkout -q "$c" && mkdir -p "$R/snap_$n" && cp -r asm "$R/snap_$n/")
}
clone prime PrimeDecomp/prime; clone melee doldecomp/melee; clone tp zeldaret/tp
snap prime_8492e9a prime 8492e9ab261a614acf2507fe48f56a93a326b233 asm/Dolphin/ asm/musyx/ asm/Runtime/ asm/os/
snap prime_3bf5eba prime 3bf5eba181a506bf30aa9ba69ab996ba5e21c44c asm/Dolphin/ asm/musyx/ asm/Runtime/ asm/os/
snap prime_4196041 prime 419604163a7f772b0919982d0602b6173755a04a asm/Dolphin/ asm/musyx/ asm/Runtime/ asm/os/
snap melee_2d0592e melee 2d0592ead7e70010cea3835b9528597c58f7fb6a asm/dolphin/ asm/MSL/ asm/Runtime/ asm/init.s asm/MetroTRK/
snap melee_9d3d029 melee 9d3d0295fc7b2df493aa8ea27baa00e276b5f31d asm/dolphin/ asm/MSL/ asm/Runtime/ asm/init.s asm/MetroTRK/
# melee flat 2020 asm: library files only (HSD/game files excluded)
FLAT="AX AXDriver AmcExi2Stubs CARD DebuggerDriver GXFifo GXInit Gecko_setjmp NMWException PPCArch Padclamp THPDec __ppc_eabi_init __va_arg abort_exit ai ansi_fp ar arq buffer_io code_80391580 code_803A949C console_io cstring ctors ctype db devcom direct_io dispatch dolphin_sdk_1 dolphin_sdk_2 dolphin_trk dolphin_trk_glue dsp dsp_debug dsp_task dtors dvderror dvdlow dvdqueue fio flush_cache fstload hio init jpegdec main_TRK math mcc mem_TRK mem_funcs mpc_7xx_603e msg msgbuf msghndlr mtx mtxvec nubevent nubinit pad perf printf runtime serpoll sislib targcont targimpl vi wchar_io"
snap melee_964759d melee 964759dfa2a0bbd9a7fe128c1b45a49c873c23e8 $(for f in $FLAT; do printf '/asm/%s.s ' $f; done)
snap tp tp f25c24699bf57f8cfee8e959e998cc82ccb32b36 asm/dolphin/ asm/MSL_C/ asm/Runtime.PPCEABI.H/ asm/init/
mkdir -p "$R/syms"
curl -sfL -o "$R/syms/melee.txt" https://raw.githubusercontent.com/doldecomp/melee/HEAD/config/GALE01/symbols.txt
curl -sfL -o "$R/syms/prime.txt" https://raw.githubusercontent.com/PrimeDecomp/prime/HEAD/config/GM8E01_00/symbols.txt
curl -sfL -o "$R/syms/tp.txt" https://raw.githubusercontent.com/zeldaret/tp/HEAD/config/GZ2E01/symbols.txt
T=$P/tools/sdkid
python -I $T/parse_asm.py --perfile --syms "$R/syms/tp.txt" $B/tp.json tp "$R/snap_tp/asm"
for s in 2d0592e 9d3d029 964759d; do python -I $T/parse_asm.py --syms "$R/syms/melee.txt" $B/melee_$s.json melee_$s "$R/snap_melee_$s/asm"; done
for s in 8492e9a 3bf5eba 4196041; do python -I $T/parse_asm.py --syms "$R/syms/prime.txt" $B/prime_$s.json prime_$s "$R/snap_prime_$s/asm"; done
python -I $T/resolve.py --manual config/sdk_symbols_manual.txt $B/melee_9d3d029.json $B/melee_2d0592e.json \
  $B/melee_964759d.json $B/prime_4196041.json $B/prime_3bf5eba.json $B/prime_8492e9a.json $B/tp.json
