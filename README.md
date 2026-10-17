# KaberiRed

A [Lilu](https://github.com/acidanthera/Lilu) plugin that aims to bring macOS graphics (display, then Metal/OpenGL
acceleration) to AMD's GCN 2 APUs by adapting **Apple's own GCN 2 drivers**: the ones macOS Monterey ships for the
Bonaire/Hawaii GPUs of the 2013 Mac Pro and 2014 iMac. It's the sister project of
[CarrizoRed](https://github.com/CL0utM4N/CarrizoRed), which does the same for Carrizo/Stoney with Apple's Polaris
drivers, and reuses its infrastructure.

**Status: Phase 0 (research and skeleton).** Nothing works yet. See [docs/ROADMAP.md](docs/ROADMAP.md).

## Targets

| Family | Products | PCI IDs | GPU | Display |
|---|---|---|---|---|
| Kabini ("Kalindi" in AMD's CAIL) | A4-5000, A6-5200, E1/E2-2xxx, AM1 Athlon/Sempron | 1002:9830–983F | GCN 2 gfx703, 2 CUs | DCE 8.3 |
| Beema/Mullins ("Godavari") | A4-6210, A6-6310, E1-6010, E2-6110, A10 Micro-6700T | 1002:9850–985F | gfx703, 2 CUs | DCE 8.3 |
| Kaveri ("Spectre"), later | A10-7850K, A8-7600, FX-7600P | 1002:1304–131D | gfx700, up to 8 CUs | DCE 8.1 |

Kabini/Mullins first, on macOS Monterey 12.7.6, the last release with Apple's GCN 2 drivers. Ventura and later
(through OpenCore Legacy Patcher's legacy-GCN root patches) come after.

## How

As in CarrizoRed: the iGPU's `device-id` is spoofed (OpenCore `DeviceProperties`) to a GCN 2 dGPU Apple supports, so
Apple's `AMD8000Controller` (DCE 8 display) and `AMDRadeonX4000` (accelerator) match it. Every difference between that
dGPU and the APU is then patched with Lilu routes: display limits (2 CRTCs, PPLL1/PPLL2 only), "VRAM" being a carve-out
of system RAM, PowerPlay, shader engine/CU counts, tiling. AMD's CAIL, inside Apple's HWLibs, reads the real device ID
from config space and should pick its own Kalindi/Spectre paths, with the microcode it embeds.

- [docs/APPLE_KEXTS.md](docs/APPLE_KEXTS.md): what's in Monterey's AMD drivers, and how to get them on Windows
- [docs/KABINI_VS_BONAIRE.md](docs/KABINI_VS_BONAIRE.md): the hardware differences, from Linux's drivers
- [docs/TEST_MACHINE.md](docs/TEST_MACHINE.md): the test laptop and what to collect from it

## Building

There's no Mac in this project: GitHub Actions builds the kext on a macOS runner (`.github/workflows/build.yml`), and
the artifact contains debug and release builds plus a debug Lilu. On a Mac:

```sh
./scripts/bootstrap.sh   # fetches Lilu (debug) and MacKernelSDK
make DEBUG=1             # build/Debug/KaberiRed.kext
```

## Tools (Windows-friendly, Python 3)

| Script | What it does |
|---|---|
| `scripts/fetch_macos.py` | Extracts files from Apple's macOS installer over HTTP range requests: no Mac, no 12 GB download, no KDK needed |
| `scripts/extract_kdk.py` | Extracts files from a Kernel Debug Kit `.dmg`/`.pkg` (needs 7-Zip) |
| `scripts/macho.py` | Lists symbols of, and disassembles functions in, Apple binaries (`pip install capstone`) |
| `scripts/match_functions.py` | Names functions in a stripped binary (Monterey's HWLibs) from a symbolised build of the same code |
| `scripts/dmpstore.py` | Decodes an NVRAM dump: KaberiRed's `KR-progress` and the panic report in `AAPL,PanicInfo` |
| `scripts/build_efi_hp.py` | Builds the OpenCore EFI (plus Monterey Recovery) for the HP A4-5000 test laptop |
| `scripts/collect_linux_info.sh` | Collects reference data (VBIOS, registers, ACPI) on the test machine under Linux |

Apple's binaries are never committed: keep them outside the repository (we use `Desktop/KaberiRed-RE/`).

## Boot arguments

| Argument | Effect |
|---|---|
| `-kroff` / `-krdbg` / `-krbeta` | Lilu's: disable, debug logging, run on unsupported macOS |
| `krpanicat=N` | Panic on purpose at N seconds after start with KaberiRed's report (read it back from NVRAM) |
| `krstopat=<stage>[#n]` | Stop the n-th call of a traced stage and panic with the report |
| `-krnoaccel` | Keep Apple's accelerator off, so the display controller comes up on its own |
| `-krnotrace` | Don't trace Apple's start-up functions |
| `-krnowatch`, `-krnowatchdog`, `-krnobreadcrumb` | Turn off the watcher's console output, its hung-call panic, the `KR-progress` NVRAM breadcrumb |
| `-krnovbios` | Don't copy the VBIOS from ACPI VFCT into `ATY,bin_image` |
| `-krnommio` | Don't read the GPU's registers at start-up |
| `-krnomodel` | Don't set the GPU's `model` name |
| `-krpagehook` | Install the dyld-shared-cache page hook even without patches (its page counter proves it works) |

## Licence

BSD 3-Clause, see [LICENSE](LICENSE). Register offsets and hardware facts come from Linux's `amdgpu`/`radeon`
drivers (MIT). No code from NootedRed or LegacyRed is used.
