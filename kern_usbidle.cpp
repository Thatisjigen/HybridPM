//  HybridPM for the Acer Nitro ANV15-52 - ATTRIBUTION NOTICE: this file is a MODIFIED copy of src/G15PM/kern_btidle.cpp
//  in https://github.com/rodgod-rr/Dell-G15-5530-Hackintosh at commit df44c68 (G15PM by Rodrigo Rangel, which carries
//  NootedGreen's XE-PM as its own Lilu plugin), licensed under the Thou Shalt Not Profit License 1.0
//  (LICENSE-TSNPL-1.0.txt beside this file); this file is under the same license. Changes (22 Nov 2026): the Bluetooth
//  controller is found by its interface class (E0/01/01) instead of Intel's vendor id, so MediaTek's is found too, and
//  SimpleMtkBtFw joins IntelBluetoothFirmware as a firmware loader to take out of the power tree; the Bluetooth policies
//  are set only on the pipes bluetoothd takes, not on the loader's own at boot; the camera is any video-class device on
//  an internal port instead of one vendor and product id; the Dell's Logitech receiver and keyboard-light controller
//  are left out; no file log, the counters are in the IORegistry; boot-args and sysctls renamed.
//
//  USB idle suspend for internal devices whose drivers never ask for it. IOUSBHostFamily suspends an idle device only
//  when its drivers set an idle policy (IOUSBHostInterface::setIdlePolicy, plus IOUSBHostPipe::setIdlePolicy on the IN
//  pipes that always have a read pending: "pausing IO" in IOUSBHostFamily.h). bluetoothd never sets one, because a
//  Mac's Bluetooth sits on UART or PCIe; nor does the camera's UVC driver. The device then stays powered and the xHCI
//  keeps polling its interrupt endpoints (the MediaTek controller's HCI event endpoint every 125 us), which holds the
//  package in shallow C-states.
//   - Bluetooth: IOUSBHostInterface::copyPipe is routed. When bluetoothd takes a pipe of a Bluetooth interface, that
//     interface gets the policy, and so do the IN pipes of interface 0 (HCI events and ACL data). The controller
//     suspends after the idle time and wakes itself with remote wakeup when it has data. Its firmware loader stays a
//     power child of the device in a state without kIOPMLowPower, which IOUSBHostFamily.h says disables idle suspend,
//     so once bluetoothd has the device the loader is taken out of the power tree (PMstop) from a thread call: by then
//     it has no work left. Interfaces no driver has opened (MediaTek's third, for isochronous LE audio) do not count
//     in the device's idle policy, as IOUSBHostFamily.h says, so they need none.
//   - Camera (a video-class interface on an internal port): the policy goes on its video interfaces and on the interrupt
//     IN pipe of the control interface, never on the isochronous streaming pipe, from the copyPipe route, from a scan
//     10 s after a USB device appears and whenever debug.hybridpm.camidle is written. A camera without remote wakeup
//     misses its status interrupt while suspended; opening the camera sends control requests, which resume it.
//  The idle time is 2000 ms for both, Linux's USB autosuspend delay. With 50 ms a Bluetooth LE mouse made the Dell's Intel
//  controller suspend at every pause, and after about 1000 cycles its firmware stopped signalling remote wakeup.
//  Boot-args: hpmbtidle=ms and hpmcamidle=ms (0 = off), -hpmusboff installs nothing. Sysctls (root):
//  debug.hybridpm.btidle (applies with the next Bluetooth transfer), debug.hybridpm.camidle (applies at once).
//  IORegistry root "HybridPMUsbIdle" = {route: 1 ok, 2 symbols missing, 3 route failed, 9 waiting for IOUSBHostFamily;
//  Bluetooth pipes set, Bluetooth interfaces set, last pipe IOReturn, last interface IOReturn, Bluetooth ms, firmware
//  loaders out of the power tree; cameras found, camera interfaces set, camera pipes set, last camera interface IOReturn,
//  last camera pipe IOReturn, camera ms; scans}.
#include <Headers/kern_api.hpp>
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>
#include <sys/sysctl.h>

SYSCTL_DECL(_debug_hybridpm);   // the node is in kern_hybridpm.cpp

enum {
	UStatus, UBtPipes, UBtIfs, UBtPipeRet, UBtIfRet, UBtMs, UBtFw,
	UCams, UCamIfs, UCamPipes, UCamIfRet, UCamPipeRet, UCamMs, UScans, UCount
};
static uint32_t gU[UCount] {};
static int gBtMs = 2000, gCamMs = 2000;
static mach_vm_address_t gCopyPipe = 0, gPipeIdle = 0, gIfIdle = 0, gPipeGet = 0, gIfGet = 0;
static thread_call_t gFwCall = nullptr, gScanCall = nullptr;
static uint64_t gFwDevId = 0;
static const char *const gFwLoaders[] {"SimpleMtkBtFw", "IntelBluetoothFirmware"};
static constexpr uint32_t kPortInternal = 2;   // kIOUSBHostPortTypeInternal

static void uPublish() {
	__atomic_store_n(&gU[UBtMs], (uint32_t)gBtMs, __ATOMIC_RELAXED);
	__atomic_store_n(&gU[UCamMs], (uint32_t)gCamMs, __ATOMIC_RELAXED);
	uint32_t copy[UCount];
	for (unsigned i = 0; i < UCount; i++) copy[i] = __atomic_load_n(&gU[i], __ATOMIC_RELAXED);
	IORegistryEntry::getRegistryRoot()->setProperty("HybridPMUsbIdle", copy, sizeof(copy));
}

static uint32_t uNum(IORegistryEntry *e, const char *key) {
	auto n = e ? OSDynamicCast(OSNumber, e->getProperty(key)) : nullptr;
	return n ? n->unsigned32BitValue() : 0xFFFFFFFFu;
}

// The setter only when the object's policy differs: bluetoothd's user client asks copyPipe for the pipe of every
// transfer, so with traffic the route runs dozens of times a second, and each set would make IOUSBHostFamily work out
// the device's idle policy again. Returns whether it set.
static bool uSet(mach_vm_address_t set, mach_vm_address_t get, void *object, int ms, unsigned ok, unsigned ret) {
	if (!set || !object) return false;
	if (get && reinterpret_cast<uint32_t (*)(void *)>(get)(object) == (uint32_t)ms) return false;
	const IOReturn r = reinterpret_cast<IOReturn (*)(void *, uint32_t)>(set)(object, (uint32_t)ms);
	if (r == kIOReturnSuccess) __atomic_fetch_add(&gU[ok], 1, __ATOMIC_RELAXED);
	__atomic_store_n(&gU[ret], (uint32_t)r, __ATOMIC_RELAXED);
	return true;
}

static bool uBtInterface(IORegistryEntry *i) {
	return uNum(i, "bInterfaceClass") == 0xE0 && uNum(i, "bInterfaceSubClass") == 1 && uNum(i, "bInterfaceProtocol") == 1;
}

// Thread call: the firmware loaders still in the power tree under a USB device leave it. They are not registered, so
// getMatchingServices does not see them: they are found among the devices' children.
static void uFwDetach(thread_call_param_t, thread_call_param_t) {
	auto m = IOService::serviceMatching("IOUSBHostDevice");
	auto it = m ? IOService::getMatchingServices(m) : nullptr;
	OSSafeReleaseNULL(m);
	while (auto dev = it ? OSDynamicCast(IOService, it->getNextObject()) : nullptr) {
		auto ci = dev->getChildIterator(gIOServicePlane);
		while (auto c = ci ? OSDynamicCast(IOService, ci->getNextObject()) : nullptr) {
			bool loader = false;
			for (auto name : gFwLoaders) loader = loader || c->metaCast(name);
			if (!loader || c->getProperty("HybridPMStop")) continue;
			c->PMstop();
			c->setProperty("HybridPMStop", true);
			__atomic_fetch_add(&gU[UBtFw], 1, __ATOMIC_RELAXED);
			IOLog("HybridPM: %s out of the power tree of its USB device\n", c->getMetaClass()->getClassName());
		}
		OSSafeReleaseNULL(ci);
	}
	OSSafeReleaseNULL(it);
	uPublish();
}

static void *uCopyPipe(IOService *that, uint8_t address);

// Thread call: the policy on every internal camera's video interfaces and its control interface's IN pipes. Asking
// copyPipe (the original) for an address the interface does not have returns nothing.
static void uScan(thread_call_param_t, thread_call_param_t) {
	__atomic_fetch_add(&gU[UScans], 1, __ATOMIC_RELAXED);
	uint32_t cameras = 0;
	auto m = IOService::serviceMatching("IOUSBHostDevice");
	auto it = m ? IOService::getMatchingServices(m) : nullptr;
	OSSafeReleaseNULL(m);
	while (auto dev = it ? OSDynamicCast(IOService, it->getNextObject()) : nullptr) {
		if (uNum(dev, "USBPortType") != kPortInternal) continue;
		bool camera = false;
		auto ci = dev->getChildIterator(gIOServicePlane);
		while (auto c = ci ? OSDynamicCast(IOService, ci->getNextObject()) : nullptr) {
			if (!c->metaCast("IOUSBHostInterface") || uNum(c, "bInterfaceClass") != 14) continue;
			camera = true;
			uSet(gIfIdle, gIfGet, c, gCamMs, UCamIfs, UCamIfRet);
			if (uNum(c, "bInterfaceSubClass") != 1 || !gCopyPipe) continue;
			for (uint8_t a = 0x81; a <= 0x8F; a++) {
				auto pipe = reinterpret_cast<OSObject *>(FunctionCast(uCopyPipe, gCopyPipe)(c, a));
				if (!pipe) continue;
				uSet(gPipeIdle, gPipeGet, pipe, gCamMs, UCamPipes, UCamPipeRet);
				pipe->release();
			}
		}
		OSSafeReleaseNULL(ci);
		cameras += camera;
	}
	OSSafeReleaseNULL(it);
	__atomic_store_n(&gU[UCams], cameras, __ATOMIC_RELAXED);
	uPublish();
}

static bool uMatched(void *, void *, IOService *, IONotifier *) {
	if (gScanCall && gCamMs > 0) {
		uint64_t deadline;
		clock_interval_to_deadline(10, kSecondScale, &deadline);
		thread_call_enter_delayed(gScanCall, deadline);
	}
	return true;
}

static void *uCopyPipe(IOService *that, uint8_t address) {
	void *pipe = FunctionCast(uCopyPipe, gCopyPipe)(that, address);
	if (!pipe || !that) return pipe;
	IOService *dev = that->getProvider();
	const uint32_t cls = uNum(that, "bInterfaceClass");
	if (cls == 14) {   // video: the camera's driver opening it
		if (gCamMs <= 0 || uNum(dev, "USBPortType") != kPortInternal) return pipe;
		bool changed = uSet(gIfIdle, gIfGet, that, gCamMs, UCamIfs, UCamIfRet);
		if ((address & 0x80) && uNum(that, "bInterfaceSubClass") == 1)
			changed = uSet(gPipeIdle, gPipeGet, pipe, gCamMs, UCamPipes, UCamPipeRet) || changed;
		if (changed) uPublish();
		return pipe;
	}
	// Off (0) is IOUSBHostFamily's default, and setting it back is the only way a policy set earlier comes off the
	// interface and its pipes, which keep it: so the route returns at once only while it has set none.
	const int bt = gBtMs;
	if ((bt <= 0 && !__atomic_load_n(&gU[UBtIfs], __ATOMIC_RELAXED) && !__atomic_load_n(&gU[UBtPipes], __ATOMIC_RELAXED)) ||
	    !uBtInterface(that)) return pipe;
	char name[24] {};
	proc_selfname(name, sizeof(name));
	if (strcmp(name, "bluetoothd")) return pipe;   // the firmware loader's own pipes, at boot, stay as they are
	if (bt > 0 && dev && gFwCall && dev->getRegistryEntryID() != __atomic_load_n(&gFwDevId, __ATOMIC_RELAXED)) {
		__atomic_store_n(&gFwDevId, dev->getRegistryEntryID(), __ATOMIC_RELAXED);
		thread_call_enter(gFwCall);
	}
	bool changed = uSet(gIfIdle, gIfGet, that, bt, UBtIfs, UBtIfRet);
	if ((address & 0x80) && uNum(that, "bInterfaceNumber") == 0)
		changed = uSet(gPipeIdle, gPipeGet, pipe, bt, UBtPipes, UBtPipeRet) || changed;
	if (changed) uPublish();
	return pipe;
}

static int uBtSysctl SYSCTL_HANDLER_ARGS {
	int v = gBtMs;
	const int e = sysctl_handle_int(oidp, &v, 0, req);
	if (!e && req->newptr) { gBtMs = v < 0 ? 0 : v > 10000 ? 10000 : v; uPublish(); }
	return e;
}
SYSCTL_PROC(_debug_hybridpm, OID_AUTO, btidle, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, uBtSysctl, "I",
            "Bluetooth USB idle suspend after N ms, 0 = off (with the next transfer)");
static int uCamSysctl SYSCTL_HANDLER_ARGS {
	int v = gCamMs;
	const int e = sysctl_handle_int(oidp, &v, 0, req);
	if (!e && req->newptr) { gCamMs = v < 0 ? 0 : v > 60000 ? 60000 : v; if (gScanCall) thread_call_enter(gScanCall); }
	return e;
}
SYSCTL_PROC(_debug_hybridpm, OID_AUTO, camidle, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, uCamSysctl, "I",
            "camera USB idle suspend after N ms, 0 = off (at once)");

static const char *gUsbPath[] {"/System/Library/Extensions/IOUSBHostFamily.kext/Contents/MacOS/IOUSBHostFamily"};
static KernelPatcher::KextInfo gUsb {"com.apple.iokit.IOUSBHostFamily", gUsbPath, arrsize(gUsbPath), {true}, {},
                                     KernelPatcher::KextInfo::Unloaded};

void usbIdleInstall() {
	if (checkKernelArgument("-hpmusboff")) return;
	int ms = 0;
	if (PE_parse_boot_argn("hpmbtidle", &ms, sizeof(ms))) gBtMs = ms < 0 ? 0 : ms > 10000 ? 10000 : ms;
	if (PE_parse_boot_argn("hpmcamidle", &ms, sizeof(ms))) gCamMs = ms < 0 ? 0 : ms > 60000 ? 60000 : ms;
	gFwCall = thread_call_allocate(uFwDetach, nullptr);
	gScanCall = thread_call_allocate(uScan, nullptr);
	gU[UStatus] = 9;
	uPublish();
	lilu.onKextLoadForce(&gUsb, 1, [](void *, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
		if (!gUsb.loadIndex || index != gUsb.loadIndex) return;
		// The node debug.hybridpm is registered by hpmInstall, which runs when the patcher loads, before this.
		sysctl_register_oid(&sysctl__debug_hybridpm_btidle);
		sysctl_register_oid(&sysctl__debug_hybridpm_camidle);
		gPipeIdle = patcher.solveSymbol(index, "__ZN13IOUSBHostPipe13setIdlePolicyEj", address, size);
		patcher.clearError();
		gIfIdle = patcher.solveSymbol(index, "__ZN18IOUSBHostInterface13setIdlePolicyEj", address, size);
		patcher.clearError();
		gPipeGet = patcher.solveSymbol(index, "__ZN13IOUSBHostPipe13getIdlePolicyEv", address, size);   // optional
		patcher.clearError();
		gIfGet = patcher.solveSymbol(index, "__ZN18IOUSBHostInterface13getIdlePolicyEv", address, size);
		patcher.clearError();
		if (!gPipeIdle || !gIfIdle) {
			gU[UStatus] = 2;
			uPublish();
			return;
		}
		KernelPatcher::RouteRequest route("__ZN18IOUSBHostInterface8copyPipeEh", uCopyPipe, gCopyPipe);
		if (!patcher.routeMultiple(index, &route, 1, address, size) || !gCopyPipe) {
			gCopyPipe = 0;
			gU[UStatus] = 3;
			patcher.clearError();
			uPublish();
			return;
		}
		gU[UStatus] = 1;
		uPublish();
		if (auto m = IOService::serviceMatching("IOUSBHostDevice")) {
			IOService::addMatchingNotification(gIOMatchedNotification, m, uMatched, nullptr);
			m->release();
		}
		IOLog("HybridPM: USB idle policy route installed, Bluetooth %d ms, camera %d ms\n", gBtMs, gCamMs);
	});
}
