#!/bin/bash
# CMake fordítóindító (CXX_COMPILER_LAUNCHER) az LSP x86 SIMD forrásaihoz macOS x86_64-re.
#
# Az LSP kézi assembly-sablonjai "0x40 + %[mem]" alakúak. Ha a fordító a változót épp a verem tetejére
# teszi, ebből "0x40 + (%rsp)" lesz, amit az LLVM assembler (macOS-en csak ez van) elutasít
# ("expected relocatable expression"). Linuxon a GNU as elfogadja, és ott a veremkiosztás is más.
# Ezért ezeket a fájlokat először assemblyre fordítjuk, a "+ (%reg)" alakot "+ 0(%reg)"-re írjuk
# (ugyanaz a cím), majd a javított assemblyből készül a tárgykód. Más forrásnál a parancs változatlanul fut.
set -euo pipefail

args=("$@")
src=""
obj=""
for ((i = 0; i < ${#args[@]}; i++)); do
    case "${args[$i]}" in
        -c) src="${args[$((i + 1))]}" ;;
        -o) obj="${args[$((i + 1))]}" ;;
    esac
done
case "$src" in
    */x86/*.cpp) ;;
    *) exec "${args[@]}" ;;
esac
[ -n "$obj" ] || exec "${args[@]}"

asm="$obj.s"
# 1. assembly (a függőségfájl -MD/-MF ebben a lépésben készül, a valódi forrás alapján)
compile=()
for ((i = 0; i < ${#args[@]}; i++)); do
    case "${args[$i]}" in
        -c) compile+=(-S) ;;
        -o) compile+=(-o "$asm"); i=$((i + 1)) ;;
        *) compile+=("${args[$i]}") ;;
    esac
done
"${compile[@]}"

# 2. "<kifejezés> + (%reg)" -> "<kifejezés> + 0(%reg)"
sed -E 's/\+[[:space:]]*\(%/+ 0(%/g' "$asm" > "$asm.tmp"
mv "$asm.tmp" "$asm"

# 3. tárgykód a javított assemblyből (a függőségfájlt nem írjuk felül)
assemble=()
skip=0
for ((i = 0; i < ${#args[@]}; i++)); do
    a="${args[$i]}"
    if [ $skip = 1 ]; then skip=0; continue; fi
    case "$a" in
        -MD|-MMD) ;;
        -MT|-MF|-MQ) skip=1 ;;
        -c) assemble+=(-c "$asm"); skip=1 ;;
        -o) assemble+=(-o "$obj"); skip=1 ;;
        *) assemble+=("$a") ;;
    esac
done
"${assemble[@]}" -Wno-unused-command-line-argument
rm -f "$asm"
