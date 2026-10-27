//  HybridPM: Lilu plugin entry. XE-PM's install, taken out of NootedGreen, runs when Lilu's kernel patcher
//  is ready; without -hpm (or hpm=W) it registers only the read-only debug.hybridpm.sensors sysctl.
//  kern_hybridpm.cpp is under the Thou Shalt Not Profit License 1.0, and so is this file, which only starts it.
#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>

void hpmInstall(KernelPatcher &patcher);

static const char *bootargOff[] {"-hpmoff"};
static const char *bootargDebug[] {"-hpmdbg"};
static const char *bootargBeta[] {"-hpmbeta"};

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal,
	bootargOff,
	arrsize(bootargOff),
	bootargDebug,
	arrsize(bootargDebug),
	bootargBeta,
	arrsize(bootargBeta),
	KernelVersion::Tahoe,   // the XCPM routine is checked byte for byte against macOS 26.7.1 25G241
	KernelVersion::Tahoe,
	[]() {
		lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) { hpmInstall(patcher); });
	}
};
