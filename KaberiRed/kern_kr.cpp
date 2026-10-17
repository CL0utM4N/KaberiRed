//
//  kern_kr.cpp
//  KaberiRed
//
//  Phase 1: observe. Finds the GCN 2 iGPU by its config-space ID, records what the hardware reports, injects the
//  VBIOS, and traces Apple's GCN 2 kexts (AMD8000Controller, AMDRadeonX4000) when they load. The black box: a
//  watcher thread prints traced calls, writes an NVRAM breadcrumb, runs a watchdog, and on request (krpanicat=N)
//  panics with a report, which macOS keeps in NVRAM (AAPL,PanicInfo) for scripts/dmpstore.py.
//
//  The routing, tracing, page-patching and black-box code comes from CarrizoRed (same author, BSD-3).
//

#include "kern_kr.hpp"
#include "kern_vbios.hpp"

#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>
#include <Headers/kern_iokit.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IODeviceTreeSupport.h>
#include <kern/thread.h>
#include <stdarg.h>

// --- Routing ------------------------------------------------------------------------------------------------------
//
// Lilu's createTrampoline copies a wrapped function's first instructions verbatim and jumps back after them; nothing
// is relocated, so a RIP-relative operand or relative branch in the copied bytes breaks (CarrizoRed spoof boot 19: a
// stack-protector prologue's `mov rax, [rip + __stack_chk_guard@GOT]` read the wrong address). Lilu also builds every
// plugin's trampolines in one shared 4 KB buffer; once it is full, createTrampoline returns 0, routeFunctionInternal
// returns EINVAL, and routeMultiple takes that for a trampoline address and reports success with the function left
// untouched. So routeChecked refuses such prologues and functions shorter than the 14-byte jump, builds the
// trampoline in a pool of its own, has Lilu write only the jump, and reads the jump back.
static constexpr size_t RoutePoolSize = 16384;
static uint8_t routePool[RoutePoolSize] __attribute__((section("__TEXT,__text"), aligned(16)));
static size_t routePoolUsed;
static uint32_t routesOk, routesFailed, routesUnverified;
static const char *routeLastFailed;

static bool routeFail(const char *symbol) {
	routesFailed++;
	routeLastFailed = symbol;
	return false;
}

static bool routeChecked(KernelPatcher &patcher, size_t index, KernelPatcher::RouteRequest &request,
	mach_vm_address_t slide, size_t size) {
	if (!request.org)
		return patcher.routeMultiple(index, &request, 1, slide, size);
	auto func = patcher.solveSymbol(index, request.symbol, slide, size);
	if (!func) {
		SYSLOG("kr", "route: %s not found (error %d)", request.symbol, patcher.getError());
		return routeFail(request.symbol);
	}
	size_t copied = 0;
	while (copied < 14) {
		hde64s hs {};
		size_t len = Disassembler::hdeDisasm(func + copied, &hs);
		if (!len || (hs.flags & F_ERROR)) {
			SYSLOG("kr", "route: REFUSED %s: cannot decode +0x%lx", request.symbol, copied);
			return routeFail(request.symbol);
		}
		bool ends = hs.opcode == 0xC3 || hs.opcode == 0xC2 || (hs.opcode == 0xFF && (hs.flags & F_MODRM) &&
			(hs.modrm_reg == 4 || hs.modrm_reg == 5));
		if (ends && copied + len < 14) {
			SYSLOG("kr", "route: REFUSED %s: function ends at +0x%lx, inside the patch", request.symbol, copied + len);
			return routeFail(request.symbol);
		}
		bool ripRelative = (hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5;
		if (ripRelative || (hs.flags & F_RELATIVE)) {
			SYSLOG("kr", "route: REFUSED %s: %s instruction at +0x%lx would be copied unrelocated", request.symbol,
				ripRelative ? "RIP-relative" : "relative-branch", copied);
			return routeFail(request.symbol);
		}
		copied += len;
	}
	if (routePoolUsed + copied + 14 > RoutePoolSize) {
		SYSLOG("kr", "route: %s: trampoline pool full (%lu bytes used)", request.symbol, routePoolUsed);
		return routeFail(request.symbol);
	}
	auto *tramp = routePool + routePoolUsed;
	if (MachInfo::setKernelWriting(true, nullptr) != KERN_SUCCESS) {
		SYSLOG("kr", "route: %s: cannot write the trampoline", request.symbol);
		return routeFail(request.symbol);
	}
	// The copied instructions (checked above), then jmp [rip+0] back to func + copied.
	memcpy(tramp, reinterpret_cast<const void *>(func), copied);
	static const uint8_t jumpBack[6] {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
	memcpy(tramp + copied, jumpBack, sizeof(jumpBack));
	mach_vm_address_t back = func + copied;
	memcpy(tramp + copied + sizeof(jumpBack), &back, sizeof(back));
	MachInfo::setKernelWriting(false, nullptr);
	routePoolUsed += (copied + 14 + 15) & ~static_cast<size_t>(15);
	*request.org = reinterpret_cast<mach_vm_address_t>(tramp);
	KernelPatcher::RouteRequest jump {request.symbol, request.to};
	if (!patcher.routeMultipleLong(index, &jump, 1, slide, size)) {
		SYSLOG("kr", "route: %s: failed to write the jump (error %d)", request.symbol, patcher.getError());
		*request.org = 0;
		return routeFail(request.symbol);
	}
	// Lilu's long route: jmp [rip+0] (FF 25 00000000) followed by the destination.
	auto *entry = reinterpret_cast<const uint8_t *>(func);
	mach_vm_address_t dest;
	memcpy(&dest, entry + 6, sizeof(dest));
	if (entry[0] != 0xFF || entry[1] != 0x25 || dest != request.to) {
		SYSLOG("kr", "route: %s: the jump didn't land (%02x %02x)", request.symbol, entry[0], entry[1]);
		routesUnverified++;
		routeLastFailed = request.symbol;
		return false;
	}
	routesOk++;
	return true;
}

// --- Trace ring ---------------------------------------------------------------------------------------------------
//
// The wrappers run in whatever context Apple's code calls them from (possibly with a spinlock held or from an
// interrupt), where IOLog can deadlock, so they only append to this lock-free ring; the watcher thread prints new
// events and runs a watchdog over calls that never return.

static uint64_t uptimeMs() {
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000;
}

// Short summary of the bring-up (start-up stages and their results), repeated by the watcher and in the report.
static char traceSummary[2048];

static void traceNote(const char *fmt, ...) {
	size_t len = strlen(traceSummary);
	if (len + 2 >= sizeof(traceSummary))
		return;
	va_list va;
	va_start(va, fmt);
	vsnprintf(traceSummary + len, sizeof(traceSummary) - len, fmt, va);
	va_end(va);
}

struct TraceEvent {
	uint64_t ms;
	const char *name;
	uint32_t value;
	bool exit;
};

static constexpr size_t TraceRingSize = 512;
static constexpr uint64_t WatchdogMs = 60000;
static TraceEvent traceRing[TraceRingSize];
static uint32_t traceHead;

static void traceEvent(const char *name, bool exit, uint32_t value) {
	uint32_t index = __atomic_fetch_add(&traceHead, 1, __ATOMIC_RELAXED);
	auto &event = traceRing[index % TraceRingSize];
	event.ms = uptimeMs();
	event.name = name;
	event.value = value;
	event.exit = exit;
}

// krstopat=<stage>[#n]: on entering that traced stage (the n-th time, default the first), block the calling thread
// and have the watcher panic with the report. Walks a stop point forward without a freeze swallowing the report.
static char stopAtName[64];
static uint32_t stopAtCount = 1, stopAtHits;
static const char *stoppedStage;

static void parseStopAt() {
	if (!PE_parse_boot_argn("krstopat", stopAtName, sizeof(stopAtName)))
		stopAtName[0] = '\0';
	if (char *hash = strchr(stopAtName, '#')) {
		*hash = '\0';
		stopAtCount = 0;
		for (char *c = hash + 1; *c >= '0' && *c <= '9'; c++)
			stopAtCount = stopAtCount * 10 + static_cast<uint32_t>(*c - '0');
		if (!stopAtCount)
			stopAtCount = 1;
	}
}

// --- Apple's kexts ------------------------------------------------------------------------------------------------
//
// Monterey (12.x) still ships Apple's GCN 2 drivers on disk: AMD8000Controller (DCE 8 display), the GFX7 layer of
// AMDRadeonX4000, and CAIL with Kalindi/Spectre support in the HWLibs plug-ins. See docs/APPLE_KEXTS.md.

static const char *pathController[] {"/System/Library/Extensions/AMD8000Controller.kext/Contents/MacOS/AMD8000Controller"};
static const char *pathSupport[] {"/System/Library/Extensions/AMDSupport.kext/Contents/MacOS/AMDSupport"};
static const char *pathFramebuffer[] {"/System/Library/Extensions/AMDFramebuffer.kext/Contents/MacOS/AMDFramebuffer"};
static const char *pathX4000[] {"/System/Library/Extensions/AMDRadeonX4000.kext/Contents/MacOS/AMDRadeonX4000"};
static const char *pathHWServices[] {
	"/System/Library/Extensions/AMDRadeonX4000HWServices.kext/Contents/MacOS/AMDRadeonX4000HWServices"};
static const char *pathHWLibs[] {"/System/Library/Extensions/AMDRadeonX4000HWServices.kext/Contents/PlugIns/"
								 "AMDRadeonX4000HWLibs.kext/Contents/MacOS/AMDRadeonX4000HWLibs"};

enum KextIndex : size_t {
	KextController,
	KextSupport,
	KextFramebuffer,
	KextX4000,
	KextHWServices,
	KextHWLibs,
	KextCount,
};

static const char *const kextShortNames[KextCount] {"8000Controller", "Support", "Framebuffer", "X4000", "HWServices",
	"HWLibs"};
static uint32_t kextLoads[KextCount];
static mach_vm_address_t kextSlides[KextCount];

static KernelPatcher::KextInfo kexts[KextCount] {
	[KextController] = {"com.apple.kext.AMD8000Controller", pathController, arrsize(pathController), {}, {},
		KernelPatcher::KextInfo::Unloaded},
	[KextSupport] = {"com.apple.kext.AMDSupport", pathSupport, arrsize(pathSupport), {}, {},
		KernelPatcher::KextInfo::Unloaded},
	[KextFramebuffer] = {"com.apple.kext.AMDFramebuffer", pathFramebuffer, arrsize(pathFramebuffer), {}, {},
		KernelPatcher::KextInfo::Unloaded},
	[KextX4000] = {"com.apple.kext.AMDRadeonX4000", pathX4000, arrsize(pathX4000), {}, {},
		KernelPatcher::KextInfo::Unloaded},
	[KextHWServices] = {"com.apple.kext.AMDRadeonX4000HWServices", pathHWServices, arrsize(pathHWServices), {}, {},
		KernelPatcher::KextInfo::Unloaded},
	[KextHWLibs] = {"com.apple.kext.AMDRadeonX4000HWLibs", pathHWLibs, arrsize(pathHWLibs), {}, {},
		KernelPatcher::KextInfo::Unloaded},
};

// --- Traced stages ------------------------------------------------------------------------------------------------
//
// Each stage wraps one of Apple's functions: entry and exit go to the trace ring, the watchdog panics naming any call
// that hasn't returned after WatchdogMs, and "sync" stages (thread context only) also print and pause on entry, so an
// instant hard hang still shows where it was.

namespace {
	struct TracedStage {
		const char *symbol;
		const char *name;
		KextIndex kext;
		bool summary;  // note the result in the trace summary (start-up stages)
		bool sync;
		mach_vm_address_t org;
	};

	TracedStage stages[] {
		// AMD8000Controller start-up (ATIController::start).
		{"__ZN17AMD8000Controller21initializeProjectInfoEv", "initializeProjectInfo", KextController, true, true, 0},
		{"__ZN17AMD8000Controller19initializeResourcesEv", "initializeResources", KextController, true, true, 0},
		{"__ZN17AMD8000Controller35initializeProjectDependentResourcesEv", "initializeProjectDependentResources",
			KextController, true, true, 0},
		{"__ZN17AMD8000Controller18initializeHardwareEv", "initializeHardware", KextController, true, true, 0},
		{"__ZN17AMD8000Controller19initializePowerPlayEv", "initializePowerPlay", KextController, true, true, 0},
		{"__ZN17AMD8000Controller24initializeConnectionInfoEv", "initializeConnectionInfo", KextController, true, true, 0},
		{"__ZN17AMD8000Controller15powerUpHardwareEv", "powerUpHardware", KextController, true, true, 0},
		{"__ZN13AtomBiosProxy14initializeAsicEv", "initializeAsic", KextSupport, true, true, 0},
		{"__ZN11AtiAsicInfo18initWithControllerEP13ATIController", "AsicInfo.initWithController", KextSupport, true,
			false, 0},
		// Display path driven by WindowServer.
		{"__ZN14AMDFramebuffer4openEv", "fb.open", KextFramebuffer, false, true, 0},
		{"__ZN14AMDFramebuffer14setDisplayModeEii", "fb.setDisplayMode", KextFramebuffer, false, true, 0},
		// Acceleration.
		{"__ZN37AMDRadeonX4000_AMDGraphicsAccelerator5startEP9IOService", "accel.start", KextX4000, true, true, 0},
		{"__ZN42AMDRadeonX4000_AMDRadeonHWServicesAbstract5startEP9IOService", "hws.start", KextHWServices, true, true, 0},
		{"_CAILInitialize", "CAILInitialize", KextHWLibs, true, true, 0},
	};

	constexpr size_t StageCount = arrsize(stages);
	uint32_t stageOpen[StageCount], stageCalls[StageCount];
	uint64_t stageEnterMs[StageCount];
	bool stageRouted[StageCount];

	// Six integer/pointer arguments cover every traced function.
	template <size_t N>
	uint64_t wrapStage(void *a, void *b, void *c, void *d, void *e, void *f) {
		auto &stage = stages[N];
		stageEnterMs[N] = uptimeMs();
		__atomic_fetch_add(&stageCalls[N], 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&stageOpen[N], 1, __ATOMIC_RELAXED);
		traceEvent(stage.name, false, 0);
		if (stopAtName[0] && !strcmp(stopAtName, stage.name) &&
			__atomic_add_fetch(&stopAtHits, 1, __ATOMIC_RELAXED) == stopAtCount) {
			__atomic_store_n(&stoppedStage, stage.name, __ATOMIC_RELAXED);
			for (;;)
				IOSleep(1000);
		}
		if (stage.sync) {
			IOLog("KaberiRed: => %s\n", stage.name);
			IOSleep(30);
		}
		auto ret = FunctionCast(wrapStage<N>, stage.org)(a, b, c, d, e, f);
		traceEvent(stage.name, true, static_cast<uint32_t>(ret));
		__atomic_fetch_sub(&stageOpen[N], 1, __ATOMIC_RELAXED);
		if (stage.summary)
			traceNote("%s=%x ", stage.name, static_cast<uint32_t>(ret));
		return ret;
	}

	// wrappers[i] = wrapStage<i>, without listing them by hand.
	template <size_t N>
	struct WrapperTable {
		static void fill(mach_vm_address_t *table) {
			WrapperTable<N - 1>::fill(table);
			table[N - 1] = reinterpret_cast<mach_vm_address_t>(wrapStage<N - 1>);
		}
	};
	template <>
	struct WrapperTable<0> {
		static void fill(mach_vm_address_t *) {}
	};

	void routeStages(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size, KextIndex kext) {
		mach_vm_address_t wrappers[StageCount];
		WrapperTable<StageCount>::fill(wrappers);
		for (size_t i = 0; i < StageCount; i++) {
			if (stages[i].kext != kext)
				continue;
			KernelPatcher::RouteRequest request {stages[i].symbol, wrappers[i], stages[i].org};
			stageRouted[i] = routeChecked(patcher, index, request, slide, size);
			if (!stageRouted[i]) {
				traceNote("noroute:%s(%d) ", stages[i].name, patcher.getError());
				patcher.clearError();
			}
		}
	}
}

// --- User-space page patches --------------------------------------------------------------------------------------
//
// Lilu's user patcher is switched off on Big Sur and later, and it finds patch sites by reading the library from disk,
// where cache-only dylibs (the Metal driver, the shader compiler) don't exist. Instead, wrap cs_validate_page and
// patch pages of the dyld shared cache in place as they are validated; the patched page is then shared by every
// process mapping the cache. Each patch counts its hits for the report. Note that a Kabini/Mullins CPU (Jaguar/Puma:
// no AVX2) runs the plain x86_64 cache, not x86_64h, so patterns must come from dyld_shared_cache_x86_64*.
// No patches yet; -krpagehook installs the hook anyway, to prove it sees the cache's pages on this machine.

struct PagePatch {
	const char *name;
	const char *arg;  // boot-arg that turns it off
	uint32_t pageOffset;
	const uint8_t *find;
	uint32_t size;
	uint32_t at;
	const uint8_t *replace;
	uint32_t replaceSize;
	bool enabled;
	uint32_t hits;
};

static PagePatch *const pagePatches = nullptr;
static constexpr size_t PagePatchCount = 0;
static mach_vm_address_t orgCsValidatePage;
static bool pageHooked;
static uint32_t pagesValidated;

static void wrapCsValidatePage(void *vnode, void *pager, uint64_t offset, const void *data, int *validated,
	int *tainted, int *nx) {
	FunctionCast(wrapCsValidatePage, orgCsValidatePage)(vnode, pager, offset, data, validated, tainted, nx);
	__atomic_fetch_add(&pagesValidated, 1, __ATOMIC_RELAXED);
	if (!data || !validated || !*validated || (offset & 0xFFF))
		return;
	auto *page = const_cast<uint8_t *>(static_cast<const uint8_t *>(data));
	for (size_t i = 0; i < PagePatchCount; i++) {
		auto &patch = pagePatches[i];
		auto *at = page + patch.pageOffset;
		if (patch.enabled && at[0] == patch.find[0] && !memcmp(at, patch.find, patch.size)) {
			memcpy(at + patch.at, patch.replace, patch.replaceSize);
			__atomic_fetch_add(&patch.hits, 1, __ATOMIC_RELAXED);
		}
	}
}

// --- Start-up -----------------------------------------------------------------------------------------------------

KRDriver *KRDriver::callback = nullptr;

void KRDriver::init() {
	callback = this;
	SYSLOG("kr", "KaberiRed starting on kernel %d.%d", getKernelVersion(), getKernelMinorVersion());
	lilu.onPatcherLoadForce(
		[](void *user, KernelPatcher &patcher) { static_cast<KRDriver *>(user)->processPatcher(patcher); }, this);
	lilu.onKextLoadForce(
		kexts, arrsize(kexts),
		[](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
			static_cast<KRDriver *>(user)->processKext(patcher, index, slide, size);
		},
		this);
}

void KRDriver::processPatcher(KernelPatcher &patcher) {
	auto *devInfo = DeviceInfo::create();
	if (devInfo) {
		// Lilu classifies the iGPU by its device-id property, so once that is spoofed to a dGPU's ID through
		// DeviceProperties it lands in videoExternal instead. Match on config space.
		IORegistryEntry *candidates[1 + 8] {devInfo->videoBuiltin};
		size_t candidateNum = 1;
		for (size_t i = 0; i < devInfo->videoExternal.size() && candidateNum < arrsize(candidates); i++)
			candidates[candidateNum++] = devInfo->videoExternal[i].video;
		for (size_t i = 0; i < candidateNum && !iGPU; i++) {
			auto *pci = OSDynamicCast(IOPCIDevice, candidates[i]);
			if (!pci)
				continue;
			auto vendor = WIOKit::readPCIConfigValue(pci, WIOKit::kIOPCIConfigVendorID);
			auto device = static_cast<uint16_t>(WIOKit::readPCIConfigValue(pci, WIOKit::kIOPCIConfigDeviceID));
			if (vendor != WIOKit::VendorID::ATIAMD || familyForDevice(device) == ChipFamily::Unknown)
				continue;
			iGPU = pci;
			iGPU->retain();
			family = familyForDevice(device);
			deviceId = device;
		}
		DeviceInfo::deleter(devInfo);
	}
	// DeviceInfo skips devices whose vendor-id property isn't a known GPU vendor (an EFI may override it to keep
	// Apple's kexts off the iGPU). Fall back to every IOPCIDevice's config space.
	if (!iGPU)
		findByConfigSpace();
	if (!iGPU) {
		SYSLOG("kr", "no supported GCN 2 iGPU found, disabling");
		disableKextCallbacks();
		return;
	}

	parseStopAt();
	logPCIInfo();
	setGpuModel();
	if (!checkKernelArgument("-krnommio"))
		probeMMIO();
	// AMDSupport reads the VBIOS from "ATY,bin_image" before trying the PCI ROM, which an APU doesn't have.
	if (!checkKernelArgument("-krnovbios"))
		iGPU->setProperty("KaberiRed,vbios-injected", VBIOS::inject(iGPU, WIOKit::VendorID::ATIAMD, deviceId));

	bool anyPagePatch = checkKernelArgument("-krpagehook");
	for (size_t i = 0; i < PagePatchCount; i++) {
		auto &patch = pagePatches[i];
		patch.enabled = !checkKernelArgument(patch.arg) && patch.pageOffset + patch.size <= 0x1000 &&
			patch.at + patch.replaceSize <= patch.size;
		anyPagePatch |= patch.enabled;
	}
	if (anyPagePatch) {
		// Long route, chained with any other wrapper of the same function (RestrictEvents has one).
		KernelPatcher::RouteRequest request {"_cs_validate_page", wrapCsValidatePage, orgCsValidatePage};
		pageHooked = patcher.routeMultipleLong(KernelPatcher::KernelID, &request, 1) && orgCsValidatePage &&
			orgCsValidatePage != EINVAL;
		if (!pageHooked) {
			SYSLOG("kr", "failed to hook cs_validate_page (error %d)", patcher.getError());
			orgCsValidatePage = 0;
			patcher.clearError();
		}
	}
	startWatcher();
}

void KRDriver::findByConfigSpace() {
	// Walk the device tree as DeviceInfo does: this early the IOPCIDevices exist but may not be registered yet.
	auto *iterator = IORegistryIterator::iterateOver(gIODTPlane, kIORegistryIterateRecursively);
	if (!iterator)
		return;
	while (auto *entry = iterator->getNextObject()) {
		auto *pci = OSDynamicCast(IOPCIDevice, entry);
		if (!pci)
			continue;
		auto vendor = WIOKit::readPCIConfigValue(pci, WIOKit::kIOPCIConfigVendorID);
		auto device = static_cast<uint16_t>(WIOKit::readPCIConfigValue(pci, WIOKit::kIOPCIConfigDeviceID));
		if (vendor != WIOKit::VendorID::ATIAMD || familyForDevice(device) == ChipFamily::Unknown)
			continue;
		SYSLOG("kr", "found iGPU %s by config space (not listed by DeviceInfo)", safeString(pci->getName()));
		iGPU = pci;
		iGPU->retain();
		family = familyForDevice(device);
		deviceId = device;
		break;
	}
	iterator->release();
}

static uint16_t subVendor, subDevice;
static uint32_t propDeviceId;
static uint64_t bar0Base, bar0Size, bar5Base, bar5Size;

void KRDriver::logPCIInfo() {
	revision = static_cast<uint8_t>(WIOKit::readPCIConfigValue(iGPU, WIOKit::kIOPCIConfigRevisionID));
	subVendor = static_cast<uint16_t>(WIOKit::readPCIConfigValue(iGPU, WIOKit::kIOPCIConfigSubSystemVendorID, 0, 16));
	subDevice = static_cast<uint16_t>(WIOKit::readPCIConfigValue(iGPU, WIOKit::kIOPCIConfigSubSystemID, 0, 16));
	WIOKit::getOSDataValue(iGPU, "device-id", propDeviceId);
	SYSLOG("kr", "found %s iGPU at %s: 1002:%04X rev 0x%02X subsys %04X:%04X (device-id property 0x%04X)",
		familyName(family), safeString(iGPU->getName()), deviceId, revision, subVendor, subDevice, propDeviceId & 0xFFFF);
	if (auto *bar0 = iGPU->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0)) {
		bar0Base = bar0->getPhysicalAddress();
		bar0Size = bar0->getLength();
	}
	if (auto *bar5 = iGPU->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5)) {
		bar5Base = bar5->getPhysicalAddress();
		bar5Size = bar5->getLength();
	}
	SYSLOG("kr", "BAR0 (FB aperture) 0x%llX size 0x%llX, BAR5 (MMIO) 0x%llX size 0x%llX", bar0Base, bar0Size, bar5Base,
		bar5Size);
	iGPU->setProperty("KaberiRed,family", familyName(family));
	iGPU->setProperty("KaberiRed,revision", revision, 8);
}

// What the firmware left behind, read once at start-up, for the report: carve-out size and location, memory
// controller and GFX configuration, and each pipe's scanout (the GOP's framebuffer).
static volatile uint32_t *gpuMmio;
static struct {
	uint32_t memSize, fbLocation, fbOffset, grbmStatus, srbmStatus, gbAddrConfig, shaderArrayConfig, rbDisable,
		ramcfg, chmap, rasterConfig;
	struct {
		uint32_t crtcControl, grphEnable, grphLow, grphHigh, pitch, xEnd, yEnd, lbMemoryCtrl;
	} pipes[4];
	bool valid;
} boot;

void KRDriver::probeMMIO() {
	auto *map = iGPU->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
	if (!map) {
		SYSLOG("kr", "failed to map the MMIO BAR, skipping the register probe");
		return;
	}
	auto *mmio = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
	gpuMmio = mmio;
	boot.memSize = mmio[Reg::CONFIG_MEMSIZE];
	// Same sanity rule as gmc_v7_0_mc_init(): some firmware leaves garbage in the upper half.
	if ((boot.memSize & 0xFFFF0000) && (boot.memSize & 0xFFFF))
		boot.memSize &= 0xFFFF;
	boot.fbLocation = mmio[Reg::MC_VM_FB_LOCATION];
	boot.fbOffset = mmio[Reg::MC_VM_FB_OFFSET];
	boot.grbmStatus = mmio[Reg::GRBM_STATUS];
	boot.srbmStatus = mmio[Reg::SRBM_STATUS];
	boot.gbAddrConfig = mmio[Reg::GB_ADDR_CONFIG];
	boot.shaderArrayConfig = mmio[Reg::CC_GC_SHADER_ARRAY_CONFIG];
	boot.rbDisable = mmio[Reg::CC_RB_BACKEND_DISABLE];
	boot.ramcfg = mmio[Reg::MC_ARB_RAMCFG];
	boot.chmap = mmio[Reg::MC_SHARED_CHMAP];
	boot.rasterConfig = mmio[Reg::PA_SC_RASTER_CONFIG];
	for (uint32_t pipe = 0; pipe < displayPipes(family) && pipe < arrsize(boot.pipes); pipe++) {
		uint32_t r = pipeRegisterBase(pipe);
		auto &p = boot.pipes[pipe];
		p.crtcControl = mmio[Reg::CRTC_CONTROL + r];
		p.grphEnable = mmio[Reg::GRPH_ENABLE + r];
		p.grphLow = mmio[Reg::GRPH_PRIMARY_SURFACE_ADDRESS + r];
		p.grphHigh = mmio[Reg::GRPH_PRIMARY_SURFACE_ADDRESS_HIGH + r];
		p.pitch = mmio[Reg::GRPH_PITCH + r];
		p.xEnd = mmio[Reg::GRPH_X_END + r];
		p.yEnd = mmio[Reg::GRPH_Y_END + r];
		p.lbMemoryCtrl = mmio[Reg::LB_MEMORY_CTRL + r];
	}
	boot.valid = true;

	uint64_t fbBase = static_cast<uint64_t>(boot.fbLocation & 0xFFFF) << 24;
	uint64_t fbTop = (static_cast<uint64_t>(boot.fbLocation >> 16) << 24) | 0xFFFFFF;
	uint64_t carveout = static_cast<uint64_t>(boot.fbOffset) << 22;
	SYSLOG("kr", "UMA carve-out %u MB at system 0x%llX, GPU FB range 0x%llX-0x%llX", boot.memSize, carveout, fbBase,
		fbTop);
	SYSLOG("kr", "GRBM_STATUS 0x%08X GB_ADDR_CONFIG 0x%08X CC_GC_SHADER_ARRAY_CONFIG 0x%08X CC_RB_BACKEND_DISABLE "
		"0x%08X MC_ARB_RAMCFG 0x%08X", boot.grbmStatus, boot.gbAddrConfig, boot.shaderArrayConfig, boot.rbDisable,
		boot.ramcfg);
	iGPU->setProperty("KaberiRed,vram-mb", boot.memSize, 32);
	iGPU->setProperty("KaberiRed,carveout-phys", carveout, 64);
	iGPU->setProperty("KaberiRed,gb-addr-config", boot.gbAddrConfig, 32);
}

// The GPU's name in About This Mac: the iGPU's "model". A "model" from DeviceProperties wins; otherwise the APU's
// graphics name from the CPU brand string where it has one (Beema/Mullins: "... RADEON R4, 4 COMPUTE CORES"), else
// a generic name. Re-checked by the watcher, since Apple's controller sets its own later. -krnomodel leaves it alone.
void KRDriver::setGpuModel() {
	static char model[64];
	static bool disabled;
	if (disabled || !iGPU)
		return;
	if (!model[0]) {
		if (checkKernelArgument("-krnomodel")) {
			disabled = true;
			return;
		}
		auto *given = OSDynamicCast(OSData, iGPU->getProperty("model"));
		if (given && given->getLength() > 1 && given->getLength() < sizeof(model) &&
			!strnstr(static_cast<char *>(const_cast<void *>(given->getBytesNoCopy())), "Bonaire", given->getLength())) {
			lilu_os_strlcpy(model, static_cast<const char *>(given->getBytesNoCopy()), given->getLength());
		} else {
			uint32_t brand[13] {};
			for (uint32_t i = 0; i < 3; i++) {
				uint32_t a = 0x80000002 + i, b, c, d;
				asm volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
				brand[i * 4] = a;
				brand[i * 4 + 1] = b;
				brand[i * 4 + 2] = c;
				brand[i * 4 + 3] = d;
			}
			auto *radeon = strnstr(reinterpret_cast<char *>(brand), "RADEON R", 48);
			if (radeon && radeon[8] >= '0' && radeon[8] <= '9')
				snprintf(model, sizeof(model), "AMD Radeon R%c Graphics", radeon[8]);
			else
				lilu_os_strlcpy(model, family == ChipFamily::Kaveri ? "AMD Radeon R7 Graphics" : "AMD Radeon HD Graphics",
					sizeof(model));
		}
		SYSLOG("kr", "GPU model: %s", model);
	}
	auto *current = OSDynamicCast(OSData, iGPU->getProperty("model"));
	size_t len = strlen(model) + 1;
	if (!current || current->getLength() != len || memcmp(current->getBytesNoCopy(), model, len))
		iGPU->setProperty("model", model, static_cast<unsigned>(len));
}

void KRDriver::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
	for (size_t i = 0; i < KextCount; i++) {
		if (kexts[i].loadIndex != index)
			continue;
		SYSLOG("kr", "%s loaded: slide 0x%llX size 0x%lX", kexts[i].id, static_cast<uint64_t>(slide),
			static_cast<unsigned long>(size));
		kextLoads[i]++;
		kextSlides[i] = slide;
		traceNote("kext:%s ", kextShortNames[i]);
		bool accel = i == KextX4000 || i == KextHWServices || i == KextHWLibs;
		if (accel && checkKernelArgument("-krnoaccel")) {
			// Keep acceleration off so the display controller comes up on its own: its probes never match.
			static const char *const probes[] {"__ZN37AMDRadeonX4000_AMDGraphicsAccelerator5probeEP9IOServicePi",
				"__ZN42AMDRadeonX4000_AMDRadeonHWServicesAbstract5probeEP9IOServicePi"};
			if (i != KextHWLibs) {
				KernelPatcher::RouteRequest request {probes[i == KextX4000 ? 0 : 1],
					reinterpret_cast<mach_vm_address_t>(+[](IOService *, IOService *, SInt32 *) -> IOService * {
						return nullptr;
					})};
				if (!routeChecked(patcher, index, request, slide, size)) {
					traceNote("noroute:probe(%d) ", patcher.getError());
					patcher.clearError();
				}
			}
			return;
		}
		if (!checkKernelArgument("-krnotrace"))
			routeStages(patcher, index, slide, size, static_cast<KextIndex>(i));
		return;
	}
}

void KRDriver::disableKextCallbacks() {
	for (auto &kext : kexts)
		kext.switchOff();
}

// --- Black box ----------------------------------------------------------------------------------------------------

// One line per child of the iGPU, two levels deep: which of Apple's drivers attached.
static void describeChildren(IORegistryEntry *entry, int depth, char *buf, size_t size) {
	auto *iterator = entry->getChildIterator(gIOServicePlane);
	if (!iterator)
		return;
	while (auto *child = OSDynamicCast(IORegistryEntry, iterator->getNextObject())) {
		size_t len = strlen(buf);
		if (len + 1 >= size)
			break;
		snprintf(buf + len, size - len, "%*s%s (%s)\n", depth * 2, "", child->getMetaClass()->getClassName(),
			safeString(child->getName()));
		if (depth < 1)
			describeChildren(child, depth + 1, buf, size);
	}
	iterator->release();
}

// NVRAM breadcrumb: the black box only reaches NVRAM reliably through a panic (a kernel NVRAM write outside one is
// only kept if macOS shuts down cleanly), but a forced sync sometimes survives a reset. While trace events keep
// arriving, write the latest progress to KR-progress (Apple's GUID): at most every 2 s and 160 times per boot.
static uint32_t breadcrumbWrites, breadcrumbRefused;

static void writeBreadcrumb(uint32_t t) {
	static IORegistryEntry *options;
	static uint32_t lastHead = ~0U, lastT;
	static char text[800];
	uint32_t head = __atomic_load_n(&traceHead, __ATOMIC_RELAXED);
	if (breadcrumbWrites >= 160 || head == lastHead || (breadcrumbWrites && t - lastT < 2))
		return;
	if (!options && !(options = IORegistryEntry::fromPath("/options", gIODTPlane)))
		return;
	lastT = t;
	lastHead = head;
	size_t len = static_cast<size_t>(snprintf(text, sizeof(text), "t=%u w=%u ev=%u last:", t, breadcrumbWrites, head));
	for (uint32_t i = head > 10 ? head - 10 : 0; i != head && len < sizeof(text); i++) {
		auto &event = traceRing[i % TraceRingSize];
		len += static_cast<size_t>(snprintf(text + len, sizeof(text) - len, " %s%s=%x", event.exit ? "" : ">",
			event.name ? event.name : "?", event.value));
	}
	if (len + 8 < sizeof(text)) {
		size_t summary = strlen(traceSummary), room = sizeof(text) - len - 8;
		len += static_cast<size_t>(snprintf(text + len, sizeof(text) - len, " | %s",
			traceSummary + (summary > room ? summary - room : 0)));
	}
	if (len >= sizeof(text))
		len = sizeof(text) - 1;
	auto *data = OSData::withBytes(text, static_cast<unsigned>(len));
	if (!data)
		return;
	if (!options->setProperty("KR-progress", data))
		breadcrumbRefused++;
	data->release();
	options->setProperty("IONVRAM-FORCESYNCNOW-PROPERTY", "KR-progress");
	options->setProperty("IONVRAM-SYNCNOW-PROPERTY", "KR-progress");
	breadcrumbWrites++;
}

static char treeNow[1024];

void KRDriver::startWatcher() {
	thread_t thread;
	if (kernel_thread_start(watcherThread, this, &thread) == KERN_SUCCESS)
		thread_deallocate(thread);
	else
		SYSLOG("kr", "failed to start the watcher");
}

void KRDriver::watcherThread(void *arg, wait_result_t) {
	auto *self = static_cast<KRDriver *>(arg);
	static char treeLast[sizeof(treeNow)];
	bool print = !checkKernelArgument("-krnowatch");
	bool watchdog = !checkKernelArgument("-krnowatchdog");
	bool breadcrumb = !checkKernelArgument("-krnobreadcrumb");
	// krpanicat=N: panic on purpose at t=N s with the report. A panic screen (debug=0x100) and the report in NVRAM
	// prove the kernel was still running; no panic means a real hang.
	uint32_t panicAt = 0;
	if (!PE_parse_boot_argn("krpanicat", &panicAt, sizeof(panicAt)))
		panicAt = 0;
	uint32_t printed = 0;
	// Tick every 500 ms for 30 minutes.
	for (uint32_t tick = 0, t = 0; t <= 1800; tick++, t = tick / 2) {
		uint32_t head = __atomic_load_n(&traceHead, __ATOMIC_RELAXED);
		if (head - printed > TraceRingSize) {
			IOLog("KaberiRed: (%u trace events dropped)\n", head - printed - static_cast<uint32_t>(TraceRingSize));
			printed = head - static_cast<uint32_t>(TraceRingSize);
		}
		for (; printed != head; printed++) {
			auto &event = traceRing[printed % TraceRingSize];
			if (event.exit)
				IOLog("KaberiRed: <- %s = 0x%x\n", event.name, event.value);
			else
				IOLog("KaberiRed: -> %s\n", event.name);
		}
		if (breadcrumb)
			writeBreadcrumb(t);
		if (panicAt && t >= panicAt)
			self->panicReport(t, "KaberiRed: deliberate panic (krpanicat), the kernel was still running.");
		if (auto *stage = __atomic_load_n(&stoppedStage, __ATOMIC_RELAXED)) {
			static char reason[160];
			snprintf(reason, sizeof(reason), "KaberiRed: stopped on entering %s (#%u, krstopat).", stage, stopAtCount);
			self->panicReport(t, reason);
		}
		// Watchdog: a traced call that never returns (a hard hang with this CPU still alive) becomes a panic naming it.
		if (watchdog) {
			uint64_t ms = uptimeMs();
			static char reason[512];
			size_t len = 0;
			for (size_t i = 0; i < StageCount; i++) {
				if (__atomic_load_n(&stageOpen[i], __ATOMIC_RELAXED) && ms - stageEnterMs[i] > WatchdogMs) {
					if (!len)
						len = static_cast<size_t>(snprintf(reason, sizeof(reason), "KaberiRed watchdog: open stages:"));
					if (len < sizeof(reason))
						len += static_cast<size_t>(snprintf(reason + len, sizeof(reason) - len, " %s(%llu ms)",
							stages[i].name, ms - stageEnterMs[i]));
				}
			}
			if (len)
				self->panicReport(t, reason);
		}
		self->setGpuModel();
		if (tick % 2) {
			IOSleep(500);
			continue;
		}
		// Which of Apple's drivers attached: every second, printed when it changes or every 15 s.
		treeNow[0] = '\0';
		describeChildren(self->iGPU, 0, treeNow, sizeof(treeNow));
		if (print && (strcmp(treeNow, treeLast) || t % 15 == 0)) {
			strlcpy(treeLast, treeNow, sizeof(treeLast));
			self->iGPU->setProperty("KaberiRed,tree", treeNow);
			IOLog("KaberiRed: t=%us iGPU children:%s\n", t, treeNow[0] ? "" : " none");
			for (char *line = treeNow; *line;) {
				char *end = strchr(line, '\n');
				if (end)
					*end = '\0';
				IOLog("KaberiRed:   %s\n", line);
				if (!end)
					break;
				*end = '\n';
				line = end + 1;
			}
			for (size_t pos = 0, len = strlen(traceSummary); pos < len; pos += 100)
				IOLog("KaberiRed: trace: %.*s\n", 100, traceSummary + pos);
		}
		IOSleep(500);
	}
}

// The report. The panic log keeps only about 10 KB and drops the *middle* of a long report, so the most important
// lines come first: identity, what the firmware left, what loaded and what was patched, then the trace.
void KRDriver::panicReport(uint32_t t, const char *reason) {
	static char report[6144];
	size_t len = 0;
	auto add = [&](const char *fmt, ...) {
		if (len + 1 >= sizeof(report))
			return;
		va_list va;
		va_start(va, fmt);
		int n = vsnprintf(report + len, sizeof(report) - len, fmt, va);
		va_end(va);
		if (n > 0)
			len += static_cast<size_t>(n) < sizeof(report) - len ? static_cast<size_t>(n) : sizeof(report) - len - 1;
	};
	add("KaberiRed %s t=%us: %s 1002:%04x rev %02x subsys %04x:%04x, device-id property %04x\n",
		xStringify(MODULE_VERSION), t, familyName(family), deviceId, revision, subVendor, subDevice, propDeviceId & 0xFFFF);
	add("bars: 0 %llx+%llx 5 %llx+%llx\n", bar0Base, bar0Size, bar5Base, bar5Size);
	if (boot.valid) {
		add("boot: memsize %u MB fbloc %x fboffset %x (carve-out %llx) grbm %x srbm %x gbaddr %x sharray %x rbdis %x "
			"ramcfg %x chmap %x raster %x\n", boot.memSize, boot.fbLocation, boot.fbOffset,
			static_cast<uint64_t>(boot.fbOffset) << 22, boot.grbmStatus, boot.srbmStatus, boot.gbAddrConfig,
			boot.shaderArrayConfig, boot.rbDisable, boot.ramcfg, boot.chmap, boot.rasterConfig);
		for (uint32_t pipe = 0; pipe < displayPipes(family) && pipe < arrsize(boot.pipes); pipe++) {
			auto &p = boot.pipes[pipe];
			add("boot pipe%u: crtc %x grph %x surf %x:%08x pitch %u %ux%u lb %x", pipe, p.crtcControl, p.grphEnable,
				p.grphHigh, p.grphLow, p.pitch, p.xEnd, p.yEnd, p.lbMemoryCtrl);
			if (gpuMmio)
				add("; now crtc %x surf %x:%08x", gpuMmio[Reg::CRTC_CONTROL + pipeRegisterBase(pipe)],
					gpuMmio[Reg::GRPH_PRIMARY_SURFACE_ADDRESS_HIGH + pipeRegisterBase(pipe)],
					gpuMmio[Reg::GRPH_PRIMARY_SURFACE_ADDRESS + pipeRegisterBase(pipe)]);
			add("\n");
		}
		if (gpuMmio)
			add("now: grbm %x srbm %x fbloc %x ih cntl %x rb %x rptr %x wptr %x; cp rptr %x wptr %x\n",
				gpuMmio[Reg::GRBM_STATUS], gpuMmio[Reg::SRBM_STATUS], gpuMmio[Reg::MC_VM_FB_LOCATION],
				gpuMmio[Reg::IH_RB_CNTL], gpuMmio[Reg::IH_RB_BASE], gpuMmio[Reg::IH_RB_RPTR], gpuMmio[Reg::IH_RB_WPTR],
				gpuMmio[Reg::CP_RB0_RPTR], gpuMmio[Reg::CP_RB0_WPTR]);
	}
	auto *vbiosStatus = OSDynamicCast(OSString, iGPU->getProperty("KaberiRed,vbios-status"));
	auto *vbios = OSDynamicCast(OSData, iGPU->getProperty(VBIOS::ImageProperty));
	add("vbios: %s, ATY,bin_image %u bytes\n", vbiosStatus ? vbiosStatus->getCStringNoCopy() : "-",
		vbios ? vbios->getLength() : 0);
	add("kexts:");
	for (size_t i = 0; i < KextCount; i++)
		add(" %s %u@%llx", kextShortNames[i], kextLoads[i], kextSlides[i]);
	add("\nroutes: %u ok, %u failed, %u not landed (last %s); pool %lu/%lu; stages", routesOk, routesFailed,
		routesUnverified, routeLastFailed ? routeLastFailed : "-", routePoolUsed, RoutePoolSize);
	for (size_t i = 0; i < StageCount; i++)
		if (stageCalls[i] || !stageRouted[i])
			add(" %s%s:%u", stageRouted[i] ? "" : "!", stages[i].name, stageCalls[i]);
	add("\npage hook: %d, %u pages validated;", pageHooked, pagesValidated);
	for (size_t i = 0; i < PagePatchCount; i++)
		add(" %s %s%u", pagePatches[i].name, pagePatches[i].enabled ? "" : "off ", pagePatches[i].hits);
	add("\nbreadcrumb: %u writes, %u refused\n", breadcrumbWrites, breadcrumbRefused);
	add("tree:\n%s", treeNow);
	size_t traceLen = strlen(traceSummary);
	add("trace: %s%s\nlast events:", traceLen > 700 ? "..." : "", traceSummary + (traceLen > 700 ? traceLen - 700 : 0));
	uint32_t head = __atomic_load_n(&traceHead, __ATOMIC_RELAXED);
	for (uint32_t i = head > 16 ? head - 16 : 0; i != head; i++) {
		auto &event = traceRing[i % TraceRingSize];
		add(" %s%s=%x", event.exit ? "" : ">", event.name ? event.name : "?", event.value);
	}
	panic("%s\n%s", reason, report);
}
