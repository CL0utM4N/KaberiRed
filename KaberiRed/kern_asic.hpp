//
//  kern_asic.hpp
//  KaberiRed
//
//  Hardware constants for the GCN 2 (Sea Islands / CIK) APUs: Kabini, Mullins/Beema and Kaveri.
//  Register offsets are dword indices into the MMIO BAR (BAR5), as in Linux's
//  drivers/gpu/drm/amd/include/asic_reg headers and amdgpu/cikd.h (MIT licensed).
//

#ifndef kern_asic_hpp
#define kern_asic_hpp

#include <stdint.h>
#include <stddef.h>

enum class ChipFamily {
	Unknown,
	Kabini,   // Kabini ("Kalindi" in AMD's CAIL): gfx703, 2 CUs, DCE 8.3
	Mullins,  // Mullins/Beema ("Godavari"): gfx703, 2 CUs, DCE 8.3
	Kaveri,   // Kaveri ("Spectre"): gfx700, up to 8 CUs, DCE 8.1
};

struct DeviceRange {
	uint16_t first, last;
	ChipFamily family;
};

// From amdgpu_drv.c (CHIP_KABINI, CHIP_MULLINS, CHIP_KAVERI; all AMD_IS_APU). Kaveri skips 0x1308, 0x1314, 0x1319-0x131A.
static constexpr DeviceRange SupportedDevices[] {
	{0x9830, 0x983F, ChipFamily::Kabini},
	{0x9850, 0x985F, ChipFamily::Mullins},
	{0x1304, 0x1307, ChipFamily::Kaveri},
	{0x1309, 0x1313, ChipFamily::Kaveri},
	{0x1315, 0x1318, ChipFamily::Kaveri},
	{0x131B, 0x131D, ChipFamily::Kaveri},
};

inline ChipFamily familyForDevice(uint16_t deviceId) {
	for (auto &range : SupportedDevices)
		if (deviceId >= range.first && deviceId <= range.last)
			return range.family;
	return ChipFamily::Unknown;
}

inline const char *familyName(ChipFamily family) {
	switch (family) {
		case ChipFamily::Kabini:
			return "Kabini";
		case ChipFamily::Mullins:
			return "Mullins/Beema";
		case ChipFamily::Kaveri:
			return "Kaveri";
		default:
			return "Unknown";
	}
}

// Display pipes (CRTCs) per family, from Linux dce_v8_0.c (num_crtc): Kaveri 4, Kabini/Mullins 2; Bonaire/Hawaii 6.
inline uint8_t displayPipes(ChipFamily family) {
	switch (family) {
		case ChipFamily::Kabini:
		case ChipFamily::Mullins:
			return 2;
		case ChipFamily::Kaveri:
			return 4;
		default:
			return 0;
	}
}

// DCE 8 per-pipe register blocks: CRTC0 at 0x1B7C, CRTC1 +0x300, CRTC2-5 from 0x417C, 0x300 apart (cikd.h
// CRTCn_REGISTER_OFFSET). The GRPH/LB/SCL blocks of each pipe sit at the same offsets from their pipe's base.
inline uint32_t pipeRegisterBase(uint32_t pipe) { return pipe < 2 ? 0x300 * pipe : 0x2600 + 0x300 * (pipe - 2); }

namespace Reg {
	// bif_4_1_d.h
	static constexpr uint32_t CONFIG_MEMSIZE = 0x150A;  // Carve-out ("VRAM") size in MB
	// gmc_7_1_d.h
	static constexpr uint32_t MC_SHARED_CHMAP = 0x801;
	static constexpr uint32_t MC_VM_FB_LOCATION = 0x809;  // [15:0] FB base >> 24, [31:16] FB top >> 24
	static constexpr uint32_t MC_VM_SYSTEM_APERTURE_LOW_ADDR = 0x80D;
	static constexpr uint32_t MC_VM_SYSTEM_APERTURE_HIGH_ADDR = 0x80E;
	static constexpr uint32_t MC_VM_FB_OFFSET = 0x81A;  // System physical address of carve-out >> 22 (APU only)
	static constexpr uint32_t MC_ARB_RAMCFG = 0x9D8;
	// oss_2_0_d.h
	static constexpr uint32_t SRBM_STATUS = 0x394;
	static constexpr uint32_t HDP_NONSURFACE_BASE = 0xB01;
	static constexpr uint32_t HDP_NONSURFACE_INFO = 0xB02;
	static constexpr uint32_t HDP_NONSURFACE_SIZE = 0xB03;
	static constexpr uint32_t IH_RB_CNTL = 0xF80;
	static constexpr uint32_t IH_RB_BASE = 0xF81;
	static constexpr uint32_t IH_RB_RPTR = 0xF82;
	static constexpr uint32_t IH_RB_WPTR = 0xF83;
	static constexpr uint32_t IH_CNTL = 0xF86;
	// gfx_7_2_d.h
	static constexpr uint32_t GRBM_STATUS = 0x2004;
	static constexpr uint32_t CP_RB0_RPTR = 0x21C0;
	static constexpr uint32_t CC_GC_SHADER_ARRAY_CONFIG = 0x226F;
	static constexpr uint32_t CC_RB_BACKEND_DISABLE = 0x263D;
	static constexpr uint32_t GB_ADDR_CONFIG = 0x263E;
	static constexpr uint32_t CP_RB0_WPTR = 0x3045;
	static constexpr uint32_t RLC_CNTL = 0x30C0;
	static constexpr uint32_t SDMA0_STATUS_REG = 0x340D;
	static constexpr uint32_t PA_SC_RASTER_CONFIG = 0xA0D4;
	// dce_8_0_d.h, pipe 0 (add pipeRegisterBase(pipe))
	static constexpr uint32_t GRPH_ENABLE = 0x1A00;
	static constexpr uint32_t GRPH_CONTROL = 0x1A01;
	static constexpr uint32_t GRPH_PRIMARY_SURFACE_ADDRESS = 0x1A04;
	static constexpr uint32_t GRPH_PITCH = 0x1A06;
	static constexpr uint32_t GRPH_PRIMARY_SURFACE_ADDRESS_HIGH = 0x1A07;
	static constexpr uint32_t GRPH_X_END = 0x1A0D;
	static constexpr uint32_t GRPH_Y_END = 0x1A0E;
	static constexpr uint32_t LB_MEMORY_CTRL = 0x1AC1;
	static constexpr uint32_t CRTC_H_TOTAL = 0x1B80;
	static constexpr uint32_t CRTC_V_TOTAL = 0x1B87;
	static constexpr uint32_t CRTC_CONTROL = 0x1B9C;
	static constexpr uint32_t CRTC_BLANK_CONTROL = 0x1B9D;
	static constexpr uint32_t CRTC_STATUS = 0x1BA3;
}

#endif /* kern_asic_hpp */
