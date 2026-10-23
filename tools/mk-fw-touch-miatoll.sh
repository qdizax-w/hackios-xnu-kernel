#!/bin/bash
# The firmware of miatoll's touch screen (the Huaxing panel's FocalTech FT8756), which the chip keeps none of:
# the kernel loads it at every boot (bsd/dev/miatoll_touch.c, boot-arg ft8756=1). It is Xiaomi's, from the
# kernel source Xiaomi published for the phone, where it is a C array the vendor driver is compiled with
# (MiCode/Xiaomi_Kernel_OpenSource, branch curtana-q-oss, drivers/input/touchscreen/ft8756_spi/include/
# firmware/fw_huaxing_v0e.i -- the one its focaltech_config.h names). Not in the repository: fetched here.
#
# usage: tools/mk-fw-touch-miatoll.sh     -> fw-touch/ft8756.bin, which userland/build.sh puts in fw/
set -e -o pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
COMMIT=231fa425e8f42661f74339de63c3ad472de6077d   # curtana-q-oss
URL=https://raw.githubusercontent.com/MiCode/Xiaomi_Kernel_OpenSource/$COMMIT/drivers/input/touchscreen/ft8756_spi/include/firmware/fw_huaxing_v0e.i
mkdir -p fw-touch
curl -sfL "$URL" -o fw-touch/fw_huaxing_v0e.i || { echo "mk-fw-touch-miatoll: cannot fetch $URL" >&2; exit 1; }
python3 - fw-touch/fw_huaxing_v0e.i fw-touch/ft8756.bin <<'PY'
import re, sys
text = open(sys.argv[1]).read()
data = bytes(int(v, 16) for v in re.findall(r'0[xX]([0-9a-fA-F]{1,2})\b', text))


def length_at(offset):    # the driver's check (bsd/dev/miatoll_touch.c, fw_length_at)
    n, complement = data[offset] << 8 | data[offset + 1], data[offset + 2] << 8 | data[offset + 3]
    return n if (n + complement) & 0xffff == 0xffff else 0


code, constants = length_at(0x100) * 2, length_at(0x108) * 2
if len(data) < 0x120 or code == 0 or code + constants > len(data):
    sys.exit(f'mk-fw-touch-miatoll: {sys.argv[1]} is not an FT8756 firmware image ({len(data)} bytes)')
open(sys.argv[2], 'wb').write(data)
print(f'{sys.argv[2]}: {len(data)} bytes, {code} of code and {constants} of constants')
PY
rm -f fw-touch/fw_huaxing_v0e.i
