#!/bin/bash
# Make ../../Mu-Silicium-3.6: Mu-Silicium at release 3.6 with the miatoll port in it (docs/PORTING.md, "miatoll").
# Releases after 3.6 do not boot on these phones; the profile (common/devices/miatoll.conf, MU_SILICIUM) takes
# the UEFI from this tree. Build it with clang 18 -- clang 23 makes images that do not boot either:
#   cd Mu-Silicium-3.6 && CLANG_BIN=<llvm-18>/bin/ python3 build_uefi.py -d miatoll -m N -r RELEASE
#
# What goes in: mu-silicium-3.6.patch (the boot shim copying the payload, the boot manager starting the XNU
# loader, the "XNU Payload" region of miatollPkg's memory map) and the loader hook, XnuLoader.[ch], which is
# redwood's (../files): in 3.6 the boot manager library is PlatformBootManagerLib.
set -e
cd "$(dirname "$(readlink -f "$0")")"
MU=../../Mu-Silicium-3.6
[ -e $MU ] && { echo "uefi/miatoll/apply.sh: $MU already exists"; exit 1; }
git clone https://github.com/Project-Silicium/Mu-Silicium.git $MU
git -C $MU checkout -q "$(cat BASE)"
git -C $MU submodule update --init --recursive
git -C $MU apply --whitespace=nowarn "$PWD/mu-silicium-3.6.patch"
cp ../files/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.[ch] \
	$MU/Silicon/Silicium/SiliciumPkg/Library/PlatformBootManagerLib/
echo "uefi/miatoll/apply.sh: done; next: build it with clang 18 (above)"
