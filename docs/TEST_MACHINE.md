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

1. Monterey 12.7.6 with OpenCore and AMD_Vanilla, **no KaberiRed**: does it reach the desktop on the firmware
   framebuffer? Note the boot-args and SMBIOS used.
2. KaberiRed (debug) + Lilu (debug), **no spoof**, boot-args `-krdbg krpanicat=60 debug=0x100 keepsyms=1`: at 60 s
   the machine panics on purpose with KaberiRed's report (`debug=0x100` keeps it on screen). After rebooting, dump
   NVRAM from OpenShell (`dmpstore -all -s fs0:\nvramNNN.bin`) and send the file; `scripts/dmpstore.py` decodes it.
3. Only then the `device-id` spoof, with `-krnoaccel` so the display controller comes up on its own.

Keep a USB stick with the unspoofed config to recover.
