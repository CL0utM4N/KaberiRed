# Test machine

## HP laptop, AMD A4-5000 (Kabini)

| | |
|---|---|
| CPU | A4-5000: 4 × Jaguar, 1.5 GHz. **No AVX2/BMI2/FMA**, so macOS uses the plain `x86_64` dyld shared cache, not `x86_64h` |
| GPU | Radeon HD 8330: 1002:9830 (expected), GCN 2 gfx703, 2 CUs, DCE 8.3, UMA carve-out |
| Model, BIOS version | _to fill in_ |
| Display | internal eDP panel (resolution _to fill in_), external ports _to fill in_ |
| UMA frame buffer size | _to fill in_ (many HP BIOSes don't offer a setting) |

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
