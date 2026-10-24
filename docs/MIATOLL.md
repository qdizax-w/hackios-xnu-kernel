# HackiOS on miatoll: the full guide

This guide runs iOS 16.5 on a Redmi Note 9S (curtana) from its own flash, with internet over the USB
cable. It starts from a phone running Android and ends with the everyday routine, which is one
command.

Other miatoll phones (Redmi Note 9 Pro / 9 Pro Max, Redmi Note 10 Lite, POCO M2 Pro) share the SoC.
This guide was made on a 64 GB Redmi Note 9S with the Huaxing panel, and the partition numbers below
are that phone's.

## Contents

1. [What works](#what-works)
2. [What you need](#what-you-need)
3. [How it fits together](#how-it-fits-together)
4. [Setting up the computer](#setting-up-the-computer)
5. [Building](#building)
6. [Setting up the phone (once)](#setting-up-the-phone-once)
7. [Internet over USB (once, on the computer)](#internet-over-usb-once-on-the-computer)
8. [Booting iOS](#booting-ios)
9. [While it runs: SSH, screenshots, the log](#while-it-runs-ssh-screenshots-the-log)
10. [Sileo](#sileo)
11. [What is kept between boots](#what-is-kept-between-boots)
12. [Ending a session, and going back to Android](#ending-a-session-and-going-back-to-android)
13. [Troubleshooting](#troubleshooting)
14. [Reference](#reference)

## What works

| | |
|---|---|
| Display, GPU (Adreno 618): blur, wallpaper, widgets | yes |
| Touch (Huaxing panel), power and volume buttons | yes |
| Every stock Apple app (root on the flash) | yes |
| Internet over the USB cable | yes |
| SSH into the phone | yes |
| Sileo, with what it installs kept | yes |
| Settings, home screen, app data kept between boots | yes |
| Wi-Fi, Bluetooth, sound, cellular, camera | no |
| Touch on the Tianma panel (Novatek NT36675) | no |

Animations run at about 30 fps. The kernel scales every frame from iOS's 1125x2436 down to the
panel's 1080x2400 on the CPU, which takes about 16 ms a frame.

## What you need

- **A miatoll phone** with an **unlocked bootloader** (Xiaomi's Mi Unlock tool) and a custom recovery
  that has `parted` (OrangeFox was used here). The phone is not flashed with anything else.
- **A Linux computer** (this was made on Arch/CachyOS) with:
  - `adb` and `fastboot`
  - `python3`, `clang`, `lld`, `perl`, `7z` (7-Zip 23 or newer: it reads APFS), `cc`
  - NetworkManager and `dnsmasq` (for USB internet)
  - about 20 GB of free disk
- **A USB cable** that carries data.
- **Time:** the first setup takes an hour or two. Every boot after it takes about a minute and a half.

## How it fits together

Nothing of this replaces Android's boot. Every session goes like this:

1. **Fastboot:** the phone is in Xiaomi's fastboot (Power + Volume Down).
2. **Loader:** `xl` boots Mu-Silicium's UEFI with this project's loader inside it into RAM
   (`fastboot boot`). The loader talks to the computer over USB.
3. **Image:** `xl` uploads a 1 GB image into RAM. It holds the boot volume, iOS's OS cryptex with the
   dyld shared cache, and the Sileo prefix.
4. **Kernel:** `xl` uploads the XNU kernel. The loader starts it on a big core and keeps serving the
   flash to it from core 0.
5. **iOS:** the kernel mounts iOS's own root volume from the `darwin` partition on the flash, and iOS
   starts. With `usbnet=1` the kernel takes the USB port over as a network adapter.
6. **End:** the session ends when you hold Power + Volume Down. The phone resets into fastboot,
   ready for the next one.

## Setting up the computer

Clone the `miatoll` branches and set them up:

```sh
git clone -b miatoll https://github.com/qdizax-w/hackios-xnu-kernel.git hackios && cd hackios
USERLAND_URL=https://github.com/qdizax-w/hackios-xnu-userland.git USERLAND_REF=miatoll common/setup.sh
```

**The kernel toolchain:** [theos](https://theos.dev/docs/installation-linux) in `~/theos`, with its
Linux toolchain (cctools) and `iPhoneOS16.5.sdk`. Details are in `xnu/README.md`.

```sh
xnu/apply.sh && xnu-deps/setup.sh
```

**The UEFI:** Mu-Silicium 3.6, built with clang 18. Newer releases, or any build made with clang 23,
reset at the Mi logo. See `docs/PORTING.md` ("miatoll") and `uefi/README.md`.

```sh
uefi/miatoll/apply.sh
# then, in the Mu-Silicium-3.6 tree it made:
CLANG_BIN=<llvm-18>/bin/ python3 build_uefi.py -d miatoll -m 0 -r RELEASE
```

`-m 0` is the Redmi Note 9S. 1 is the Note 9 Pro (India), 2 the Note 10 Lite, 3 joyeuse, 4 excalibur,
5 gram.

**iOS 16.5 (iPhone X, 20F66)** comes straight from Apple's servers. It is about 8 GB while it runs
and 5.3 GB afterwards, and nothing of it goes into git.

```sh
cd ios165 && ./fetch.sh && cd ..
```

**The phone's own firmware** isn't in the repository. Take it from the phone while it runs Android:

```sh
mkdir -p fw-gpu
adb pull /vendor/firmware/a618_gmu.bin fw-gpu/
adb pull /vendor/firmware/a630_sqe.fw fw-gpu/
adb pull /vendor/firmware/a615_zap.mdt fw-gpu/
adb pull /vendor/firmware/a615_zap.b00 fw-gpu/
adb pull /vendor/firmware/a615_zap.b01 fw-gpu/
adb pull /vendor/firmware/a615_zap.b02 fw-gpu/
tools/mk-fw-touch-miatoll.sh            # touch firmware (Xiaomi's, from their kernel source) -> fw-touch/
```

## Building

Rebuild after changing something, or after pulling new commits.

| What | Command |
|---|---|
| Kernel | `./xnu-build.sh` |
| Loader | `xnuloader/build.sh` (`xl enter` packs it with the UEFI by itself) |
| iOS image | `cd userland && FLASHBOOT=1 GL=1 REST=1 RC_JOBS=rc.jobs.unlock ./build.sh && cd ..` |

The kernel build ends with an error about an "exported name" in a symbol set. That error is expected:
the kernel is built anyway. Check the time on
`xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`.

The iOS image is `userland/flashboot.apfs`. It is about 1 GB and holds what isn't on the flash. If
`ssh/` holds keys (see [SSH](#ssh)), they go into it, and the build says so.

## Setting up the phone (once)

iOS's root volume lives in a partition of its own, `darwin`, carved from the end of `userdata`.
**This wipes Android's data:** back it up first.

All of this runs from the recovery (OrangeFox), over `adb`.

1. **Look at the partition table** (it only reads):

   ```sh
   adb shell parted /dev/block/sda unit s print
   ```

   On the 64 GB Redmi Note 9S, `userdata` is entry 18 and the unit ends at sector 14499840 (blocks of
   4096 bytes).

2. **Shrink `userdata`** so that 20 GiB (5242875 blocks) are left after it. Note the three dashes:

   ```sh
   adb shell 'parted ---pretend-input-tty /dev/block/sda unit s resizepart 18 9256959s Yes'
   ```

3. **Make `darwin`** in the space that freed:

   ```sh
   adb shell 'parted /dev/block/sda unit s mkpart darwin 9256960s 14499834s'
   ```

4. **Format `userdata`** again from the recovery (Wipe → Format Data), and check that Android still
   boots.

5. **Write iOS's root volume** to the start of `darwin`. It is 5.6 GB, so it goes through `/data`:

   ```sh
   adb push ios165/rootfs.apfs /data/local/tmp/rootfs.apfs
   adb shell 'D=/dev/block/by-name/darwin; dd if=/data/local/tmp/rootfs.apfs of=$D bs=4096 conv=fsync && sync &&
              dd if=$D bs=4096 count=1374208 2>/dev/null | sha256sum'
   cat ios165/rootfs.apfs.sha256          # must be the same
   adb shell rm /data/local/tmp/rootfs.apfs
   ```

**Another phone:** the partition's place and GUIDs are written into three places, and the loader and
kernel refuse to write anywhere else. Put your phone's values (from `parted ... print`) into all
three:

- `darwin_miatoll` in `xnuloader/loader.c`
- `UFS_DARWIN_FIRST` / `UFS_DARWIN_BLOCKS` in `xnu/files/bsd/dev/redwood_ufs.c`
- `DEVICES['miatoll']` in `tools/darwin-write.py`

`DEVICE=miatoll tools/darwin-write.py check` compares them with the phone.

## Internet over USB (once, on the computer)

While iOS runs, the phone is a **USB Ethernet adapter** (CDC-ECM) to the computer. Its name is
`HackiOS (USB Ethernet)` and its USB ID is `1209:0001`. The computer's end has the address
`02:00:5e:77:ee:01` and the phone's end `02:00:5e:77:ee:02`. The computer gives the phone an address by
DHCP and forwards its traffic to the internet (NAT), the way a phone's hotspot does the other way
round.

### Linux with NetworkManager

1. **A shared connection.** It starts by itself whenever the phone's adapter appears, and is picked
   out by the address of the computer's end:

   ```sh
   nmcli connection add type ethernet con-name hackios-usb ifname '*' \
       ethernet.mac-address 02:00:5E:77:EE:01 \
       ipv4.method shared ipv6.method disabled connection.autoconnect yes
   ```

   NetworkManager then runs `dnsmasq` on `10.42.0.1` for DHCP and DNS, turns on forwarding, and NATs
   the phone's traffic. The phone gets `10.42.0.100`, and keeps it from one boot to the next.

2. **The firewall.** If one runs, it drops the phone's DHCP requests, and with them everything else.
   For ufw, boot the phone once (below) and find the adapter's name:

   ```sh
   ip -br link | grep -i 02:00:5e:77:ee:01        # e.g. enp2s0f0u2
   ```

   Then let it in and let it through:

   ```sh
   sudo ufw allow in on enp2s0f0u2
   sudo ufw route allow in on enp2s0f0u2
   ```

   The name depends on the USB port. If you use another port, add the same two rules for the name that
   shows up there. With firewalld, put the adapter in the `trusted` zone instead:
   `sudo firewall-cmd --permanent --zone=trusted --add-interface=NAME && sudo firewall-cmd --reload`.

3. **Check it** while iOS runs:

   ```sh
   journalctl -f | grep dnsmasq     # DHCPDISCOVER, DHCPOFFER, DHCPREQUEST, DHCPACK 10.42.0.100
   ping 10.42.0.100                 # the phone answers
   ```

   Then open Safari on the phone.

**Undo:** `nmcli connection delete hackios-usb`. For ufw, `sudo ufw status numbered`, then
`sudo ufw delete N` for each of the two rules.

### Linux without NetworkManager

The same thing by hand, with `NAME` the adapter and `WAN` the interface that has the internet:

```sh
sudo ip addr add 10.42.0.1/24 dev NAME && sudo ip link set NAME up
sudo dnsmasq --interface=NAME --bind-interfaces --dhcp-range=10.42.0.100,10.42.0.150,1h --no-daemon &
sudo sysctl -w net.ipv4.ip_forward=1
sudo nft add table ip hackios
sudo nft 'add chain ip hackios post { type nat hook postrouting priority 100; }'
sudo nft add rule ip hackios post ip saddr 10.42.0.0/24 oifname WAN masquerade
```

### macOS and Windows

- **macOS** drives CDC-ECM itself. Use System Settings → General → Sharing → Internet Sharing, from
  your connection to `HackiOS (USB Ethernet)`. The phone then gets an address in `192.168.2.x`, so put
  that into `~/.ssh/config`. Not tested.
- **Windows** has no CDC-ECM driver of its own. It isn't supported.

## Booting iOS

### The everyday way

Put the phone in **fastboot**: hold **Power + Volume Down** with the cable plugged in, until the
fastboot screen. Then:

```sh
DEVICE=miatoll ./xl ios
```

It takes about 40 seconds:

- It starts the loader and uploads `userland/flashboot.apfs`.
- It starts iOS in the background with `BOOTARGS` and `IOS_ARGS` from `common/devices/miatoll.conf`.
- The terminal is free again. The run's own output goes to `logs/ios-run.log`.

What you see:

| After | What |
|---|---|
| A few seconds | The Apple logo |
| About a minute | The home screen |
| Within seconds of the home screen | The computer finds `HackiOS (USB Ethernet)` |
| About 30 s after the home screen | The phone has `10.42.0.100`, and internet |

### The same, step by step

```sh
DEVICE=miatoll ./xl enter
DEVICE=miatoll ./xl ramdisk userland/flashboot.apfs
DEVICE=miatoll XL_SECS=86400 \
  XL_CMDLINE="bootcpu=7 cpus=8 ft8756=1 gpu=1 fb_scan=1 ufs=2 root_apfs=ufs_darwin usbnet=1 ufs_write=1 var_store=1 var_save_secs=60" \
  ./xl run &
```

What the settings mean:

| Setting | What it does |
|---|---|
| `bootcpu=7 cpus=8` | The kernel on a big core (Cortex-A76), all eight cores in use |
| `ft8756=1` | Touch (and with it the buttons) |
| `gpu=1` | The Adreno 618 for the window server |
| `fb_scan=1` | Frames are put on the panel in step with its scan, so there is no tearing |
| `ufs=2 root_apfs=ufs_darwin` | The root is the iOS volume in `darwin`, read through the loader |
| `usbnet=1` | The USB port is a network adapter while iOS runs |
| `ufs_write=1 var_store=1` | `/private/var` is kept in `darwin` between boots |
| `var_save_secs=60` | ... and saved every minute |
| `XL_SECS` | How long before the kernel hands control back to the loader. With `usbnet=1` the loader cannot take the port back, so it is a day. |

### Without the USB network

Leave out `usbnet=1` and the loader keeps the USB port. `xl stop` then works and `XL_SECS` can be short,
but there is no internet.

## While it runs: SSH, screenshots, the log

With `usbnet=1` the loader can't be reached. The kernel answers `xl` over the network instead, on TCP
port 5140, and `xl` finds the phone by itself:

```sh
DEVICE=miatoll ./xl screenshot          # logs/screen.png, in under a second
DEVICE=miatoll ./xl log                 # the kernel's console log so far -> logs/loader.log
```

### SSH

`sshd` starts on every boot, for root, with a key only (no passwords):

```sh
ssh miatoll
```

One-time setup on the computer:

```sh
ssh-keygen -t ed25519 -N "" -f ~/.ssh/hackios_ed25519                   # your key for the phone
mkdir -p ssh && cp ~/.ssh/hackios_ed25519.pub ssh/authorized_keys
ssh-keygen -t ed25519 -N "" -f ssh/ssh_host_ed25519_key                 # the phone's own host key
cat >> ~/.ssh/config <<'EOF'

Host miatoll
	HostName 10.42.0.100
	User root
	IdentityFile ~/.ssh/hackios_ed25519
	IdentitiesOnly yes
	StrictHostKeyChecking accept-new
EOF
```

Then rebuild the iOS image, which puts the keys on the phone. `ssh/` is in `.gitignore`: keys never go
into git. The shell is Procursus' (`/var/jb`), with `apt`, `dpkg`, coreutils and the rest.

## Sileo

Sileo is on the home screen a few seconds after SpringBoard comes up. Every boot, `/var/jb/prep.sh`
(from `userland/rc.jobs.unlock`) registers it, and any other app a package brought. The Procursus
repository is set up, and packages install over the USB internet.

This is a "rootless" setup: everything lives under `/var/jb`. What Sileo installs is kept between
boots (next section).

## What is kept between boots

`/private/var` holds all of iOS's own data:

- your settings
- the home screen's layout, widgets and folders
- what apps store
- Sileo and its packages (`/var/jb`)

While iOS runs it is in memory. With `var_store=1` it's put back at boot from `darwin` (two slots of
2 GiB, 8 GiB into the partition, after the iOS volume), so it comes back as you left it.

**You never save by hand.** It happens:

- **Every minute** (`var_save_secs=60`). Only what changed is written, in a few seconds.
- **When you hold Power + Volume Down** to leave. The kernel sees both keys and asks for a save at
  once, before the phone resets.

A change made in the last moments before an unexpected reset can be lost. The slot being written is
never the newest good one, so a cut-short save only loses itself, never the one before.

Over SSH:

```sh
/redwood/bin/var_store save      # save now
/redwood/bin/var_store show      # what the two slots hold
/redwood/bin/var_store erase     # start afresh at the next boot
```

Not kept: temporary files, logs, and the `.deb` files apt downloaded.

## Ending a session, and going back to Android

- **End a session:** hold **Power + Volume Down** until fastboot. Your data is saved on the way, and
  the phone is ready for the next `xl ios`.
- **Back to Android:** in fastboot, `fastboot reboot`, or hold Power alone. Android and its data are
  untouched; it never sees `darwin`.

`xl stop` doesn't work during a `usbnet=1` session, because the loader has handed the port to iOS.

## Troubleshooting

| What you see | What to do |
|---|---|
| `fastboot boot` hangs, or `xl enter` / the upload stops partway | Xiaomi's fastboot sometimes stops answering. Hold Power + Volume Down again, then run `xl ios` again. |
| `xl: a run is in progress` | An `xl run` from before is still waiting. `xl ios` clears its own; for one started by hand, stop it (Ctrl-C, or `kill`). |
| Stuck at the Apple logo | Boot again from fastboot. Booting a second time without going back to fastboot first doesn't work. |
| The computer sees no new USB device | Check `journalctl -k -f` while iOS boots. A charge-only cable shows nothing. |
| The adapter is there, but the phone gets no address | The firewall. `journalctl -f \| grep dnsmasq` must show DHCPDISCOVER; if it doesn't, see [the firewall](#linux-with-networkmanager). |
| The phone has an address, but no internet | `nmcli connection show --active` must list `hackios-usb`, and ufw needs the `route allow` rule. |
| Internet by address works, but no names: Safari can't open Google, Sileo can't add repos | Seen once, cause not found; the computer side was fine. On the phone (`ssh miatoll`), `/redwood/bin/net_test store State:/Network/Global/DNS resolve google.com` must show `ServerAddresses = ( "10.42.0.1" )` and an address. If it doesn't, boot again from fastboot. Don't restart `hackios-usb` on the computer while iOS runs: the phone doesn't ask for its address again. (A bash `/dev/tcp/NAME/...` test fails even when names work; use `net_test`.) |
| A new adapter name after changing USB ports | Add the two ufw rules for the new name. |
| `ssh miatoll` is refused | The keys weren't in `ssh/` when the image was built: rebuild it and boot again. |
| A change from the last minute is gone | It came after the last save, and the phone reset before the next one. |
| Animations feel slow | A known limit: the CPU scales every frame (about 30 fps). |

## Reference

| | |
|---|---|
| Phone's address | `10.42.0.100` (computer `10.42.0.1`) |
| Phone's adapter | USB `1209:0001`, `02:00:5e:77:ee:02` (computer's end `...:01`) |
| Log and screenshot server | TCP 5140 on the phone (`log`, `screen`) |
| SSH | port 22, root, key only |
| `darwin` | entry 19 of unit 0 (`sda19`), LBA 9256960..14499834; iOS volume at 0, the saved `/var` from 8 GiB |
| Serial in Settings → About | `MIATOLL9S` |
| Profile | `common/devices/miatoll.conf` (`BOOTARGS`, `IOS_ARGS`) |
| USB network driver | `xnu/files/bsd/dev/redwood_usbnet.c` |
| Saving `/var` | `userland/src/var_store.c`, `xnu/files/bsd/dev/redwood_ufs.c` |
| Jobs every boot | `userland/rc.jobs.unlock` |
| Porting notes | `docs/PORTING.md`, "miatoll" |
