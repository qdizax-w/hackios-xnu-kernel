# Porting HackiOS to another device

HackiOS was brought up on the POCO X5 Pro 5G (Snapdragon 778G). A Qualcomm phone with an unlockable bootloader
is the realistic next target: the boot chain (Mu-Silicium UEFI started with `fastboot boot`) and most of the kernel's
platform code carry over. Start with:

```sh
common/new-device.sh <codename> path/to/device.dtb
```

It writes `common/devices/<codename>.conf` (the profile `common/build.sh` and `common/run.sh` read) and
`common/devices/<codename>.platform.c` (the loader's description of the SoC), filling in what the device tree says.
Get the device tree from the phone's `vendor_boot` image, or from a running Android: `adb pull /sys/firmware/fdt`.

## 1. The UEFI

The phone needs a UEFI that our loader can run under. Port [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
the way `uefi/` ports redwood (it follows `lisa`, the same SoC package):

- `Platforms/<Vendor>/<codename>Pkg`: memory map and driver list (copy the closest device of the same SoC);
- `Binaries/<codename>`: the UEFI drivers of the phone's own firmware, extracted with
  [UEFIReader](https://github.com/WOA-Project/UEFIReader) from its `xbl.img`;
- apply our two changes from `uefi/mu-silicium.patch`: the boot shim copies the "XNUP" container after the UEFI
  image to `PAYLOAD_BASE`, and `BootManagerLib` starts the XNU loader from it instead of the menu.

Put the addresses in the profile: `FD_BASE`, `FD_SIZE` (where the shim puts the UEFI image) and `PAYLOAD_BASE`
(free RAM for the kernel and images). The prebuilt `BootShim.bin` is built for redwood's addresses: build the
shim from your Mu-Silicium tree (`mkxnuimg.py` does it when `Mu-Silicium/` is there).

## 2. The loader

`xnuloader/loader.c` describes each device with a `struct platform` and picks it by the framebuffer base the UEFI
reports. Paste `common/devices/<codename>.platform.c` next to `platform_redwood` and add it to `platforms[]`:

| Field | Where it comes from |
|---|---|
| `io_base`, `io_size` | one window that covers the SoC's peripherals; it must not start at 0 (XNU then skips clock setup) |
| `gic_dist`, `gic_redist` | the `arm,gic-v3` node's `reg` (filled in by `new-device.sh`) |
| `cores`, `core_shift` | the `cpu@` nodes; Qualcomm numbers cores in MPIDR's second affinity field (shift 8) |
| `small_cores` | a bit per core of the little cluster (lowest `capacity-dmips-mhz`) |
| `psci_hvc` | 1 if the firmware's PSCI is called with HVC (QEMU), 0 for SMC (phones) |
| `fb_base` | the framebuffer address; the loader prints it at start (`display W x H at ...`) |

Rebuild with `xnuloader/build.sh`; QEMU must still pick `qemu-virt` (no match = ramfb).

## 3. The kernel

What is generic: GICv3, the timer, PSCI SMP, the display (the UEFI's framebuffer), APFS and ramfs, the driver
replacements, the return-to-loader path. What is redwood-specific and needs looking at:

- **touch**: `bsd/dev/redwood_goodix.c` (Goodix GT9916 over bit-banged SPI) — another panel needs its own driver
  feeding `/dev/redwood-touch` (`bsd/dev/redwood_input.c`);
- **buttons, CPU frequency, watchdog**: GPIO/PMIC numbers and the EPSS block in `redwood_input.c`,
  `redwood_cpufreq.c`, `osfmk/arm64/loader_return.c`;
- **GPU / Wi-Fi / sound**: `redwood_adreno.c` (Adreno 642L), `redwood_wifi.c` (WCN6750 + ath11k), `redwood_audio.c`
  — the same Qualcomm subsystems (SMEM, SMP2P, GLINK, QRTR, QMI) are reused, the addresses and firmware differ.

Start with no extras: the system boots to the lock screen with only the framebuffer, touch and the CPU.

## 4. Firmware

Vendor firmware never goes into the repository; it comes from the phone's own Android (`FIRMWARE` in the profile
lists what to pull, see README "On the phone"). Without `fw-gpu/` the kernel does not start the GPU and says so only
deep in the log (`adreno: THE GPU IS NOT STARTED`); `userland/build.sh` prints which firmware went into the image.

## 5. Run

```sh
common/build.sh <codename>
common/run.sh <codename>
```

The first useful milestone is the loader's own screen and log over USB (`./xl log`), the next one is the kernel's
verbose boot text (`-v`), then SpringBoard. Read [checks.md](checks.md) before trusting a "it is not in the log".

## miatoll (Snapdragon 720G, SM7125)

Redmi Note 9S / 9 Pro / 9 Pro Max, Redmi Note 10 Lite, POCO M2 Pro: `common/devices/miatoll.conf`. **Reaches the home
screen** on a 4 GB curtana (Redmi Note 9S), all eight cores, **with touch** (the Huaxing panel's FT8756). What it took:

- **UEFI: Mu-Silicium 3.6, built with clang 18.** Later Mu-Silicium releases do not boot on these phones (Mi logo, a
  reset after a second, again), and no release built with clang 23 does either -- the official 3.6 image boots, the same
  source built with clang 23 does not, built with clang 18 it does. LLVM 18 can sit beside the system's compiler (its
  release for Ubuntu 18.04 needs that distribution's `libtinfo.so.5` in its `lib/`): `CLANG_BIN=<llvm-18>/bin/
  python3 build_uefi.py -d miatoll -m N -r RELEASE` in the 3.6 tree `uefi/miatoll/apply.sh` makes
  (`Mu-Silicium-3.6`, which the profile names: `MU_SILICIUM`; release 3.6 calls the image `MIATOLL_UEFI.fd`,
  `mkxnuimg.py` takes either name). Our changes there are redwood's with 3.6's names
  (`uefi/miatoll/mu-silicium-3.6.patch`: the boot shim, `XnuLoaderMain ()` at the start of
  `PlatformBootManagerUnableToBoot ()`, the hook's libraries and protocols in `PlatformBootManagerLib.inf`, the memory
  map's `{"XNU Payload", 0xB0000000, 0x04000000, ...}` line), and redwood's `XnuLoader.[ch]`.
- **The payload at `0xB0000000`, not `0xF0000000`.** On a 4 GB phone the only RAM long enough for the kernel and the
  system image is `0xC0000000-0x17E000000`; redwood's address splits it. The loader takes the address from the
  platform (`payload_base`), the UEFI hook from the memory map's "XNU Payload" region.
- **The USB driver's buffers below `0xC0000000`.** It takes the last megabyte below 4 GB, which splits the same stretch;
  the loader holds that RAM while the driver starts (`usb_keep_below`). And the loader's window search now joins free
  ranges that touch. Window: 3040 MB.
- **A smaller system image**: `SLIM=slim.miatoll` (userland/build.sh) leaves out what nothing on the way to the home
  screen uses -- the applications iOS installs on its first boot but four, a few more, the applications' fonts, four
  frameworks only those use: 2040 MB against the 2144 MB the window allows, about 870 MB left for the kernel and iOS.
  The dyld shared cache alone is 980 MB of it.
- **The kernel's timer interrupt.** SM7325 (redwood) has the EL1 virtual timer on INTID 27, SM7125 on 19 (PPI 3, as
  SDM845 and SC7180). With 27 the kernel hangs at its first wait for the time, right after
  `skip attaching fsw to lo0 using legacy TX model`. The loader now says which (`vtimer_intid`, the gic node's
  `vtimer-intid`) and `gic_v3_irq.c` takes it, 27 when it is not given. Without a kernel toolchain,
  `tools/patch-kernel-miatoll.py` makes the same change in the prebuilt kernel (`KERNEL=out/mach.miatoll`).
- **Touch**: `bsd/dev/miatoll_touch.c`, boot-arg `ft8756=1`. The FT8756 is in the panel and powered with it; it keeps
  no firmware, so the kernel loads Xiaomi's at every boot into its PRAM and DRAM, ECC-checked, without a reset, as
  Xiaomi's driver does (`fts_fw_recovery`; MiCode `curtana-q-oss`, `drivers/input/touchscreen/ft8756_spi`), over SPI
  driven by hand on GPIO 53-56; reports at register 0x01 in pixels. The firmware is `tools/mk-fw-touch-miatoll.sh`'s
  (`fw-touch/ft8756.bin`, into the small image's `fw/`). The Tianma panel's Novatek NT36675 has no driver yet.
- **Building the kernel here**: theos with its Linux toolchain and `iPhoneOS16.5.sdk` (xnu/README.md); and the
  builds now run in the C locale (`xnu-env.sh`): in tr_TR, `[a-zA-Z]` leaves out `i`, and tcsh's build -- and with it
  the kernel's -- broke on it.
- **Loader**: `platform_miatoll` -- GIC as on redwood, cores 0-5 Cortex-A55 (`small_cores = 0x3f`), 6-7 Cortex-A76,
  framebuffer `0x9C000000`, `io_base` `0x00100000` (not redwood's: the kernel's Wi-Fi, audio DSP and GPU drivers refuse
  to start on another base). Drivers that do not compare are listed in `redwood_driver_args`; the loader leaves their
  boot-args out and says so. No hypervisor watchdog (QHEE, not Haven): a hung run ends with the power key.

To run: `common/build.sh miatoll`, then `common/run.sh miatoll` (the phone in fastboot). The first `fastboot boot`
after the phone enters fastboot is sometimes not taken, or resets at once; `xl enter` again.

Not done, each a driver to write against the phone's own hardware (the phone's device tree has the addresses):

- touch on the Tianma panel (Novatek NT36675, same pins: Xiaomi's `nt36xxx_spi` driver);
- power and volume keys: PM6150's power-on block and PM6150L GPIO 2 (`redwood_buttons.c` reads PMK8350 and PM7325);
- GPU: Adreno 618, with its own GMU, SQE and zap firmware, not the 642L of `redwood_adreno.c`;
- Wi-Fi: WCN3990 is ath10k over SNOC (`qcom,icnss@18800000`), not ath11k;
- sound, camera, haptics, flashlight, CPU frequency (SM7125 has OSM, not EPSS), battery.

The loader writes its log to the raw `logdump` partition, which curtana has.
