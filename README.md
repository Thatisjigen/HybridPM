# HybridPM

XE-PM, JulioNectar's CPU performance and idle-power profile for hybrid Alder Lake / Raptor Lake under XCPM, as a
standalone Lilu plugin. Written for an Acer Nitro ANV15-52 (i9-13900H) whose internal panel is not driven by
NootedGreen, so the NootedGreen build of XE-PM could not be used.

## Origin and license

`kern_hybridpm.cpp` is a modified copy of `src/NootedGreen/kern_xepm.cpp` from
[JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe)
at commit `99631db2` (NootedGreen fork by JulioNectar; NootedGreen by StezzaPilot). It is licensed under the
**Thou Shalt Not Profit License 1.0** (`LICENSE-TSNPL-1.0.txt`), and so is this repository: use, modification and
distribution are allowed for non-profit purposes under the same license, with attribution. The first commit of this
repository is the unmodified file; the second is what HybridPM changes.

## What it does

20 s after Lilu's patcher starts and again at 60 s:

1. HWP units on the hybrid P-cores (XCPM writes `IA32_HWP_REQUEST` in 100 MHz units; a P-core unit is about 78.7 MHz).
   A lower max from XCPM, as in Low Power Mode, is converted as well; E-core requests are left as XCPM writes them.
2. EPP 64 instead of 0x80 (`hpmepp=N` for another value). With HWP choosing the clock, 96 rose too slowly for the short, periodic work of
   composing 144 Hz displays, which dropped frames; 64 is the highest value that kept them smooth, with the lowest idle draw of 0 to 96.
   Under an all-core load lower values lose clock: on an i9-13900H, 96 held 3.6 GHz on 20 threads where EPP 0 held 3.0.
3. Energy-efficient turbo off (MSR `0x1FC` bit 19).
4. XCPM C-state table: C8 and C10 on, C3 off, only when the table holds the expected values (`hpmcst=0` leaves it;
   `sysctl debug.hybridpm.cst=0|1` switches it live).
5. D3hot for driverless PCI devices (IPU camera, GNA, ISH, DPTF, Thunderbolt DMA): **opt-in with `hpmd3=1`**.
6. Sustained PL1 (`hpmpl1=W`, or `hpm=W`). Nothing changes without the boot-arg.

With `-nohpm` the kext only registers the read-only `debug.hybridpm.sensors`.

## USB idle suspend

Internal USB devices whose drivers never set an IOUSBHost idle policy stay powered, and the xHCI keeps polling them,
which holds the package in shallow C-states: the Bluetooth controller (bluetoothd sets none, a Mac's Bluetooth is on
UART or PCIe) and the camera (neither does the UVC driver). `kern_usbidle.cpp` routes `IOUSBHostInterface::copyPipe`
and sets the policy:

- on the Bluetooth interfaces (class E0/01/01) whose pipes bluetoothd takes, and on the IN pipes of interface 0. The
  controller suspends after 2 s idle and wakes itself with remote wakeup. Its firmware loader (SimpleMtkBtFw,
  IntelBluetoothFirmware) is then taken out of the device's power tree, where it would keep idle suspend off;
- on the video interfaces of a camera on an internal port, and on the interrupt IN pipe of its control interface (not the
  isochronous one), also from a scan 10 s after a USB device appears.

`hpmbtidle=ms` and `hpmcamidle=ms` choose the idle time (default 2000, 0 = off), `-hpmusboff` leaves USB alone. Sysctls
`debug.hybridpm.btidle` (with the next Bluetooth transfer) and `debug.hybridpm.camidle` (at once). Counters: the
`HybridPMUsbIdle` property of the IORegistry root. Based on `G15PM` (`kern_btidle.cpp`) by Rodrigo Rangel,
[rodgod-rr/Dell-G15-5530-Hackintosh@df44c68](https://github.com/rodgod-rr/Dell-G15-5530-Hackintosh/commit/df44c68),
under the same license.

Boot-args: `-nohpm`, `hpm=W`, `hpmepp=N`, `hpmcst=0`, `hpmd3=1`, `hpmpl1=W`, `-hpmusboff`, `hpmbtidle=ms`,
`hpmcamidle=ms`. Sysctls: `debug.hybridpm.*`.

## Non-profit declaration

Section 18 of the Thou Shalt Not Profit License 1.0 allows distribution only with the declaration of its section 17. So: the maintainer of this repository, an individual acting on a not-for-profit basis, represents and declares that they derive no revenue whatsoever from the distribution of this work or of derivative works, or from support or services relating to them.

## Build

```sh
bash build.sh
```

Command Line Tools only: MacKernelSDK `3f75008` and the Lilu 1.7.2 DEBUG SDK are fetched into `build/deps`
(set `LILU_ZIP` to reuse a local `Lilu-1.7.2-DEBUG.zip`). `SOURCE_DATE_EPOCH` is fixed, so two builds give the
same binary. Output: `build/out/HybridPM.kext`; add it to OpenCore after Lilu.

## Status

Tested on the Acer Nitro ANV15-52 (i9-13900H, macOS 26.7.x): one core reaches 5.0-5.1 GHz
instead of 3.1-3.5; idle package power not yet settled. The XCPM routine it hooks is checked byte by byte on the
running kernel (25G241, 25G332); on another kernel the hook stays off. Results from XE-PM itself are the author's,
on a Dell XPS 13 Plus 9320 (i7-1360P).
