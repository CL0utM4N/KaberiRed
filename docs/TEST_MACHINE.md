# Test machine

## HP laptop, AMD A4-5000 (Kabini)

| | |
|---|---|
| CPU | A4-5000: 4 × Jaguar, 1.5 GHz (family 16h model 0 stepping 1). AVX, BMI1, MOVBE, F16C; **no AVX2/BMI2/FMA**, so macOS uses the plain `x86_64` dyld shared cache, not `x86_64h` |
| GPU | Radeon HD 8330: **1002:9832 rev 0x00**, subsystem 103C:8133; GCN 2 gfx703, 2 CUs, DCE 8.3 |
| Model, firmware | HP Notebook, board 8133, Insyde BIOS **F.12 (2015-07-30)**, KBC 98.0C |
| Memory | 2 × 4 GB DDR3-1333 (dual channel, 128-bit) |
| UMA carve-out | **512 MB**; BAR0 256 MB at `0xE0000000` (smaller than the carve-out), MMIO (BAR5) 256 KB at `0xF2900000`, doorbells 8 MB at `0xF2000000`. E820 reserves `0xBFC00000`-`0xDFFFFFFF`, which presumably holds the carve-out |
| Display | eDP panel 1366×768 (HPD1), HDMI (HPD2), VGA (DAC1) |
| VBIOS | "AMD Fusion Kalindi Generic VBIOS", 2015-03-18, 59,904 bytes; SetPixelClock rev 6 (Apple's `AtiSetPixelClock_V1_V6`), PowerPlayInfo rev 6.1, IntegratedSystemInfo rev 1.8 |
| **No ACPI VFCT table** | the VBIOS isn't published through ACPI. KaberiRed falls back to the legacy ROM shadow at `0xC0000`; the dumped copy (`KaberiRed-RE/…/gpu/pci-rom.bin`) can be injected as `ATY,bin_image` through DeviceProperties if that fails |
| Other devices | FCH xHCI 1022:7814, EHCI/OHCI 1022:7808/7807, SATA AHCI 1022:7804, SD 1022:7813, HD Audio 1022:780D, HDMI audio 1002:9840, Realtek RTL810xE Ethernet 10EC:8136, Intel Wireless 7260 8086:08B1 |

Under Linux (`radeon`, kernel 6.17) the GPU comes up completely: kabini microcode, all 8 rings and IB tests pass, UVD
1.64 and VCE 40.2.2, DPM on, MSI interrupts.

Jaguar is a family 16h CPU: macOS needs the AMD_Vanilla kernel patches for 15h/16h, as on CarrizoRed's machines.
On CarrizoRed's APUs the FCH's USB 3 controller never delivered interrupts to macOS (see `FCHXHCIFix`); Kabini's
on-die FCH is a different generation, so check USB on the first boot before blaming anything else.

## Phase 0: reference data from Linux

Boot a Linux live USB (Ubuntu or Fedora) with Secure Boot off, ideally with the kernel parameters
`radeon.cik_support=0 amdgpu.cik_support=1` (so `amdgpu` drives the GPU and exposes its registers), then:

```sh
sudo bash scripts/collect_linux_info.sh
```

It's read-only and writes `kaberired-dump-<host>-<date>.tar.gz` next to itself: PCI config space, ACPI tables
(including VFCT, the VBIOS), the GPU driver's log, CPU flags, and the GPU registers that matter here (carve-out
location, `GB_ADDR_CONFIG`, tiling, raster config, both display pipes, panel power sequencer). Copy it to the Windows
PC into `Desktop\KaberiRed-RE\`.

## First macOS boots

The EFI comes from `scripts/build_efi_hp.py` (OpenCore 1.0.8, AMD_Vanilla with 4 cores, iMac19,1, SSDT-EC-USBX,
VoodooPS2 with the Elantech trackpad, AirportItlwm for the Intel 7260, Monterey Recovery from `macrecovery.py`).

**USB stick:** format it FAT32 (GPT or MBR) and copy the contents of `KaberiRed-RE/efi-hp/usb/` (the `EFI` and
`com.apple.recovery.boot` folders) to its root. **BIOS (F10):** Secure Boot off, legacy/CSM boot off if offered,
USB boot on. Boot it with F9 (boot menu), **from a USB 2.0 port**: those are on the EHCI controllers, which macOS
supports natively; the FCH xHCI (1022:7814) is untested under macOS. In OpenCore's picker choose the Recovery entry;
join Wi-Fi in Recovery (no Ethernet driver), erase the internal disk as APFS in Disk Utility, then Reinstall macOS
Monterey.

### Install log
- **2026-10-17, first EFI:** black screen, no picker; the OpenCore file log was 256 KB of zeros. With `--debugoc
  --diag` (log on screen through the firmware's text console) it stopped at `Driver HfsPlus.efi at 1 is being
  loaded`: Apple's `HfsPlus.efi` executes **RDRAND, which Jaguar lacks**. `HfsPlusLegacy.efi` (no RDRAND) fixed it:
  the picker appears and Recovery boots.
- **Recovery, 4 cores:** stops at `dlil_init: Waiting for all the create dlil kernel threads…`: threads queued on the
  other cores never run. Not yet understood (the MADT is normal: LAPIC IDs 0-3, two I/O APICs). `cpus=1` gets past it.
- **Recovery, 1 core (with and without AirportItlwm):** the screen stops right after `rooting via boot-uuid`. With
  XHCIProbe (observe-only) it printed its 2 s report once and never again, so the kernel itself stopped, not just the
  root mount: the stick was seen (xHCI port 4, high speed, link in U0), but the 1 ms sampler got only 149 of ~2000
  samples in its first 2 s, as if the CPU was nearly saturated by interrupts. FCHXHCIFix (matched to 7814) never
  started. Hypothesis: an interrupt storm from the FCH's EHCI/OHCI controllers (shared legacy IRQs 17/18; Linux applies
  AMD-specific EHCI quirks to them). Next test: `--nousb2` keeps Apple's EHCI/OHCI drivers off them.
- **`--nousb2`:** same freeze (138 samples in the first 2 s window), so not EHCI/OHCI. Caps Lock dead at the freeze:
  the CPU is stuck, not just the screen. The OpenCore log (DEBUG) is clean: FID 0x14 DID 1, TSC 1497 MHz, **FSB 99.8
  MHz** passed to the kernel (`OCAK: BusFreq = 99812867Hz`), MSR 35h 4/4, every AMD_Vanilla patch applied, every kext
  injected, AppleIntelCPUPowerManagement dummied, ExitBootServices reached. Next: a baseline with only Lilu and
  VirtualSMC (`--minimal`), to separate the add-on kexts (battery/EC, PS/2) from macOS itself.

1. Monterey 12.7.6 with OpenCore and AMD_Vanilla, **no KaberiRed**: does it reach the desktop on the firmware
   framebuffer? Note the boot-args and SMBIOS used.
2. KaberiRed (debug) + Lilu (debug), **no spoof**, boot-args `-krdbg krpanicat=60 debug=0x100 keepsyms=1`: at 60 s
   the machine panics on purpose with KaberiRed's report (`debug=0x100` keeps it on screen). After rebooting, dump
   NVRAM from OpenShell (`dmpstore -all -s fs0:\nvramNNN.bin`) and send the file; `scripts/dmpstore.py` decodes it.
3. Only then the `device-id` spoof, with `-krnoaccel` so the display controller comes up on its own.

Keep a USB stick with the unspoofed config to recover.
