//
//  kern_start.cpp
//  KaberiRed
//

#include <Headers/kern_api.hpp>
#include <Headers/plugin_start.hpp>

#include "kern_kr.hpp"

static KRDriver kr;

static const char *bootargOff[] {"-kroff"};
static const char *bootargDebug[] {"-krdbg"};
static const char *bootargBeta[] {"-krbeta"};

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
	bootargOff,
	arrsize(bootargOff),
	bootargDebug,
	arrsize(bootargDebug),
	bootargBeta,
	arrsize(bootargBeta),
	KernelVersion::Monterey,
	KernelVersion::Tahoe,
	[]() { kr.init(); },
};
