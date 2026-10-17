# Roadmap

Each phase ends with a testable milestone. Test boots cost the tester real time: a visual test is only requested once
a counter in the report proves the patch in question landed, and one test EFI is rebuilt in place.

## Phase 0: environment and research (current)

- [x] Repository skeleton from CarrizoRed: Makefile, Info.plist template, Lilu entry, CI, black box
- [x] Monterey's AMD binaries on Windows without a KDK (`scripts/fetch_macos.py`, Apple's installer, 12.7.6 21H1320)
- [ ] Inventory of `AMD8000Controller`, `AMDSupport`, `AMDRadeonX4000`, HWServices/HWLibs: GCN 2 classes, device-ID
      tables, CAIL Kalindi/Spectre paths ([APPLE_KEXTS.md](APPLE_KEXTS.md))
- [x] Linux differences Bonaire vs Kabini/Kaveri ([KABINI_VS_BONAIRE.md](KABINI_VS_BONAIRE.md))
- [ ] Spoof target chosen (a Bonaire ID that `AMD8000Controller` and `AMDRadeonX4000` both match)
- [ ] Does Monterey's Metal driver have a GFX7 code generator for these IDs? (dyld shared cache, `x86_64` not
      `x86_64h`: Jaguar has no AVX2)
- [ ] Reference data from the HP A4-5000 ([TEST_MACHINE.md](TEST_MACHINE.md)): Linux `lspci`, VBIOS, register dumps

## Phase 1: observe

- [ ] macOS Monterey boots on the A4-5000 with the firmware framebuffer (OpenCore, no KaberiRed)
- [ ] KaberiRed loaded, no spoof: report (`krpanicat=60`) shows the iGPU, carve-out, `GB_ADDR_CONFIG`, VBIOS injected
- [ ] Spoof `device-id` with `-krnoaccel`: which of Apple's drivers attach, and where the controller stops

## Phase 2: display (`AMD8000Controller` + `AMDSupport`)

- [ ] Controller accepts the APU: ASIC caps, family/revision checks, VBIOS from VFCT, VRAM from `CONFIG_MEMSIZE`
- [ ] DCE 8.3 limits: 2 CRTCs, PPLL1/PPLL2 only, line buffers, eDP link
- [ ] **Milestone:** native resolution on the internal panel, brightness, then HDMI/VGA

## Phase 3: acceleration (`AMDRadeonX4000`)

- [ ] CAIL runs its own Kalindi path (it reads the real device ID) and loads its embedded microcode
- [ ] VRAM = carve-out: FB window, VM page-table entries, interrupt ring placement
- [ ] Shader engine / CU / RB / tiling configuration for 1 SE, 2 CUs, 1 RB, 2 pipes
- [ ] PowerPlay Dummy back end
- [ ] **Milestone:** Metal device present, WindowServer composites with acceleration
- [ ] 16-bank LDS hazard (`v_interp_p1_f32` writing its own I source): check what Apple's GFX7 compiler emits; if
      needed, port CarrizoRed's interpolation fixer with a GFX7 decoder, an offline harness and a fuzzer first

## Phase 4: Kaveri, then newer macOS

- [ ] Kaveri (DCE 8.1, 4 CRTCs, PPLL0-2, up to 8 CUs, 2 MECs)
- [ ] Ventura and later with OpenCore Legacy Patcher's legacy-GCN root patches
