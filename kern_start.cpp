//  HybridPM: Lilu plugin entry. XE-PM's install, taken out of NootedGreen, runs when Lilu's kernel patcher
//  is ready; with -nohpm it registers only the read-only debug.hybridpm.sensors sysctl. The USB idle policies
//  (kern_usbidle.cpp) wait for IOUSBHostFamily and have their own switch, -hpmusboff.
//  kern_hybridpm.cpp and kern_usbidle.cpp are under the Thou Shalt Not Profit License 1.0, and so is this file, which
//  only starts them.
#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>

void hpmInstall(KernelPatcher &patcher);
void usbIdleInstall();

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
		usbIdleInstall();
	}
};
