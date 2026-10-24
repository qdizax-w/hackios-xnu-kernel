# HackiOS on miatoll (Redmi Note 9S): booting, USB internet, Sileo

This is how to start iOS 16.5 on a Redmi Note 9S (curtana, Snapdragon 720G) from the iOS volume on its
own flash, with internet over the USB cable. It assumes the phone has been set up once already: an
unlocked bootloader and the `darwin` partition holding the iOS volume (see *Setting up the phone*
at the end).

Nothing is flashed to the phone to boot it. Each boot is sent from the computer over USB, from
fastboot, and ends when the phone goes back to fastboot.

## What works

| | |
|---|---|
| Display, GPU (Adreno 618), blur, wallpaper, widgets | yes |
| Touch, power and volume buttons | yes |
| Every stock Apple app (root on the flash) | yes |
| Internet over the USB cable (`usbnet=1`) | yes |
| Sileo (package manager) | yes, and what it installs is kept |
| Settings, home screen, app data kept between boots | yes (`var_store=1`) |
| SSH into the phone | yes (`ssh miatoll`) |
| Wi-Fi, sound, cellular | no |

Animations run at about 30 fps: the kernel scales every frame from iOS's 1125x2436 down to the
panel's 1080x2400 on the CPU, and that takes about 16 ms a frame.

## Before the first boot: the computer (once)

These steps are for a Linux computer with NetworkManager. The phone shows up as a USB Ethernet
adapter (CDC-ECM, driven by Linux's own `cdc_ether`). The computer gives it an address by DHCP and
shares its own connection with it.

1. **Make a shared connection** that starts by itself whenever the phone's adapter appears. It is
   picked out by the address of the computer's end, `02:00:5E:77:EE:01`:

   ```sh
   nmcli connection add type ethernet con-name hackios-usb ifname '*' \
       ethernet.mac-address 02:00:5E:77:EE:01 \
       ipv4.method shared ipv6.method disabled connection.autoconnect yes
   ```

   NetworkManager then runs dnsmasq for DHCP and DNS on `10.42.0.1`, and NATs the phone's traffic.
   `dnsmasq` must be installed.

2. **Open the firewall** if one is running. With ufw, the phone's DHCP requests are dropped until it
   is allowed in, and its traffic is not forwarded until it is allowed to route:

   ```sh
   sudo ufw allow in on enp2s0f0u2
   sudo ufw route allow in on enp2s0f0u2
   ```

   `enp2s0f0u2` is the adapter's name on the computer this was written on. It depends on the USB
   port. Plug the phone in and boot it once, then use the name that shows up:

   ```sh
   ip -br link | grep -i 02:00:5e:77:ee:01
   ```

   To undo all of this: `nmcli connection delete hackios-usb`, and `sudo ufw status numbered` then
   `sudo ufw delete N` for the two rules.

## Booting

The short way, once the phone is in fastboot (hold Power + Volume Down with the cable plugged in):

```sh
DEVICE=miatoll ./xl ios
```

It starts the loader, uploads the image (about 40 seconds) and starts iOS with the settings of
`IOS_ARGS` in `common/devices/miatoll.conf`, in the background. Its output goes to
`logs/ios-run.log`, and the terminal is free again. The step by step way below is what it does.

1. **Build** the kernel and the image (only after changing them):

   ```sh
   ./xnu-build.sh
   cd userland && FLASHBOOT=1 GL=1 REST=1 RC_JOBS=rc.jobs.unlock ./build.sh && cd ..
   ```

2. **Put the phone in fastboot**: hold Power + Volume Down with the cable plugged in.

3. **Start the loader and send it the image**: about 40 seconds for 1 GB.

   ```sh
   DEVICE=miatoll ./xl enter
   DEVICE=miatoll ./xl ramdisk userland/flashboot.apfs
   ```

4. **Run iOS** with the USB network:

   ```sh
   DEVICE=miatoll XL_SECS=86400 \
     XL_CMDLINE="bootcpu=7 cpus=8 ft8756=1 gpu=1 fb_scan=1 ufs=2 root_apfs=ufs_darwin usbnet=1 ufs_write=1 var_store=1 var_save_secs=60" \
     ./xl run
   ```

   `xl run` stays in the foreground. Run it in a second terminal, or end the command line with `&`.
   `XL_SECS` is how long the phone may run before the kernel gives control back to the loader. With
   `usbnet=1` the loader cannot take the USB port back, so make it long.

The Apple logo appears after a few seconds, the home screen within about a minute. At the same
time the computer sees a new USB device, `1209:0001 HackiOS (USB Ethernet)`. About 30 seconds
after the home screen the phone asks for an address and gets `10.42.0.100`.

```sh
ping 10.42.0.100                 # the phone answers
journalctl -f | grep dnsmasq     # DHCPDISCOVER ... DHCPACK 10.42.0.100 when it asks
```

Safari, the App Store and Sileo then have internet.

### Without the USB network

Leave out `usbnet=1` and the loader keeps the USB port: `xl stop` works and `XL_SECS` can be short.
There is no internet then.

## While it runs

With `usbnet=1` the loader cannot be reached, so the kernel answers `xl` over the network instead
(TCP port 5140 on the phone):

```sh
DEVICE=miatoll ./xl screenshot          # logs/screen.png (a second or less)
DEVICE=miatoll ./xl log                 # the kernel's console log so far, into logs/loader.log
```

`xl` picks the network on its own when it finds the phone at its address.

`xl stop` does not work in this mode. To end a session, hold **Power + Volume Down** until the
phone is in fastboot. That is also how to start the next boot.

### SSH

`sshd` starts on every boot, for root, with a key only (no passwords):

```sh
ssh miatoll
```

That needs, once, on the computer:

- a key pair for the phone, `~/.ssh/hackios_ed25519`;
- its public half in `ssh/authorized_keys` of this repository;
- the phone's own host key, `ssh/ssh_host_ed25519_key`.

`userland/build.sh` puts all three into the image. `ssh/` is in `.gitignore` and never goes into git.
The `miatoll` entry in `~/.ssh/config` is:

```
Host miatoll
	HostName 10.42.0.100
	User root
	IdentityFile ~/.ssh/hackios_ed25519
	IdentitiesOnly yes
	StrictHostKeyChecking accept-new
```

### What is kept between boots

`/private/var` is all of iOS's own data:

- settings
- the home screen's layout
- what apps keep
- what Sileo installed (`/var/jb`)

It lives in memory. With `var_store=1` it is put back at boot from the `darwin` partition, past the
iOS volume. The saving is done for you:

- every minute (`var_save_secs=60`), writing only what changed (a few seconds);
- when you hold Power + Volume Down to leave, before the phone resets.

Nothing needs saving by hand. To save at a moment of your own, over SSH:
`/redwood/bin/var_store save`. To start afresh: `/redwood/bin/var_store erase`, then reboot.

Left out: temporary files, logs, and the packages apt downloaded.

## Sileo

Sileo is on the home screen a few seconds after SpringBoard comes up: `/var/jb/prep.sh` registers
it, and any app a package brought, on every boot (`userland/rc.jobs.unlock`). The Procursus
repository is set up, and packages install over the USB network.

What Sileo installs is kept with the rest of `/private/var`. It is gone only after
`var_store erase`, or when booted without `var_store=1`.

## When something is wrong

| What you see | What to do |
|---|---|
| `xl: a run is in progress` | The `xl run` from the last boot is still waiting. Stop it (Ctrl-C, or kill it) and try again. |
| `fastboot boot` hangs or `xl enter` fails | Xiaomi's fastboot sometimes stops answering. Hold Power + Volume Down again and repeat. |
| The computer sees the adapter, but the phone gets no address | Check the firewall (step 2). `journalctl -f \| grep dnsmasq` should show DHCPDISCOVER. |
| The phone has an address, but no internet | `nmcli connection show --active` should list `hackios-usb`. Check `ufw route allow` as well. |
| A new adapter name after changing USB ports | Add the two ufw rules for the new name. |
| Stuck at the Apple logo | Boot again from fastboot. Booting a second time without going back to fastboot first does not work. |
| The upload stops partway | Hold Power + Volume Down again and repeat `xl ios`. |
| A change from the last minute is gone | It was made after the last save, and the phone reset before the next one. |

## Setting up the phone (once)

Already done on the phone this was written on. For another one:

- **Bootloader:** unlocked (Xiaomi's unlock tool).
- **`darwin` partition:** entry 19 on logical unit 0 (`sda19`), the end of what was `userdata`, made
  with `parted` from a recovery. It needs about 20 GB (5242875 blocks of 4096 bytes); the rest stays
  Android's `userdata`.
- **iOS volume:** written into `darwin` from recovery with `tools/darwin-write.py`. The loader
  refuses to write into a partition other than the one in `xnuloader/loader.c` (`darwin_miatoll`),
  so another phone's partition table goes there first.
- **The rest:** the boot settings are in `common/devices/miatoll.conf`.
