#!/bin/bash
# HybridPM.kext, built with the Command Line Tools only: MacKernelSDK 3f75008 and the Lilu SDK from Lilu-1.7.2-DEBUG.zip
# (headers of Lilu 1.7.3). Reproducible: SOURCE_DATE_EPOCH fixes Lilu's kextVersion (__DATE__/__TIME__).
#   bash build.sh                   -> build/out/HybridPM.kext   (deps fetched into build/deps)
#   LILU_ZIP=/path/Lilu-1.7.2-DEBUG.zip bash build.sh   reuses a local copy of the Lilu package
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); W=$HERE/build; D=$W/deps; B=$W/obj; O=$W/out
MKSDK_COMMIT=3f750085caa17ec3a7880f11c11bf4f48cd6a164
LILU_URL=https://github.com/acidanthera/Lilu/releases/download/1.7.2/Lilu-1.7.2-DEBUG.zip
LILU_SHA=e95df95d82e8b151047abf359ecd9c853755c59293e19dfcc3907da8fbd8cf89
VER=1.0.0
export SOURCE_DATE_EPOCH=1793102400   # 27 Oct 2026 10:00 UTC
mkdir -p "$D"
[ -d "$D/MacKernelSDK" ] || git clone -q https://github.com/acidanthera/MacKernelSDK.git "$D/MacKernelSDK"
git -C "$D/MacKernelSDK" cat-file -e "$MKSDK_COMMIT^{commit}" || { echo "STOP: MacKernelSDK lacks $MKSDK_COMMIT"; exit 1; }
Z=${LILU_ZIP:-$D/Lilu-1.7.2-DEBUG.zip}; [ -f "$Z" ] || curl -fsSL -o "$Z" "$LILU_URL"
[ "$(shasum -a 256 "$Z" | cut -d' ' -f1)" = $LILU_SHA ] || { echo "STOP: $Z checksum"; exit 1; }
rm -rf "$B" "$O" "$W/sdk" && mkdir -p "$B" "$O/HybridPM.kext/Contents/MacOS" "$W/sdk/MacKernelSDK"
git -C "$D/MacKernelSDK" archive $MKSDK_COMMIT | tar -x -C "$W/sdk/MacKernelSDK"
unzip -q "$Z" -d "$W/sdk/lilu"
SDK="$W/sdk/lilu/Lilu.kext/Contents/Resources"; K="$W/sdk/MacKernelSDK/Headers"; S="$HERE"
[ -f "$SDK/Headers/plugin_start.hpp" ] || { echo "STOP: no Lilu SDK"; exit 1; }
FLAGS=(-arch x86_64 -mmacosx-version-min=10.6 -std=c++14 -mkernel -fapple-kext -nostdinc -I"$K" -I"$SDK" -I"$S"
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DMODULE_VERSION=$VER -DPRODUCT_NAME=HybridPM
  -O3 -flto -fvisibility=hidden -fno-rtti -fno-exceptions -fno-builtin -fno-common -Wall -Wno-unused-parameter)
: > "$B/compile.log"
for f in "$SDK/Library/plugin_start.cpp" "$S/kern_start.cpp" "$S/kern_hybridpm.cpp"; do
  xcrun clang++ "${FLAGS[@]}" -c "$f" -o "$B/$(basename "${f%.cpp}").o" 2>> "$B/compile.log" || { tail -30 "$B/compile.log"; exit 1; }
done
cat > "$B/HybridPM_info.c" <<EOC
#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
__private_extern__ kern_return_t HybridPM_kern_start(kmod_info_t *ki, void *data);
__private_extern__ kern_return_t HybridPM_kern_stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(org.anv1552.HybridPM, "$VER", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = HybridPM_kern_start;
__private_extern__ kmod_stop_func_t *_antimain = HybridPM_kern_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__ ;
EOC
xcrun clang -arch x86_64 -mmacosx-version-min=10.6 -mkernel -nostdinc -I"$K" -DKERNEL -DKERNEL_PRIVATE -O3 -flto -fvisibility=hidden \
  -c "$B/HybridPM_info.c" -o "$B/HybridPM_info.o"
BIN="$O/HybridPM.kext/Contents/MacOS/HybridPM"
xcrun clang++ -arch x86_64 -mmacosx-version-min=10.6 -mkernel -fapple-kext -nostdlib -flto -Xlinker -kext -Xlinker -dead_strip -static \
  -L"$W/sdk/MacKernelSDK/Library/universal" "$B"/{plugin_start,kern_start,kern_hybridpm,HybridPM_info}.o -lkmodc++ -lkmod -lcc_kext -o "$BIN"
strip -S "$BIN"; strip -x -T "$BIN" 2>/dev/null || strip -x "$BIN"
cp "$S/Info.plist" "$O/HybridPM.kext/Contents/Info.plist"
mkdir -p "$O/HybridPM.kext/Contents/Resources" && cp "$S/LICENSE-TSNPL-1.0.txt" "$O/HybridPM.kext/Contents/Resources/"
echo "compile warnings: $(grep -c 'warning:' "$B/compile.log" || true)"
echo "HybridPM $(shasum -a 256 "$BIN" | cut -c1-8) $(stat -f %z "$BIN") B -> $O/HybridPM.kext"
