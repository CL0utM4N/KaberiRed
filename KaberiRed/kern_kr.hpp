//
//  kern_kr.hpp
//  KaberiRed
//

#ifndef kern_kr_hpp
#define kern_kr_hpp

#include <Headers/kern_patcher.hpp>
#include <IOKit/pci/IOPCIDevice.h>

#include "kern_asic.hpp"

class KRDriver {
public:
	void init();

private:
	static KRDriver *callback;

	void processPatcher(KernelPatcher &patcher);
	void processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size);

	void findByConfigSpace();
	void logPCIInfo();
	void probeMMIO();
	void setGpuModel();
	void disableKextCallbacks();

	void startWatcher();
	static void watcherThread(void *arg, wait_result_t);
	void panicReport(uint32_t t, const char *reason);

	IOPCIDevice *iGPU {nullptr};
	ChipFamily family {ChipFamily::Unknown};
	uint16_t deviceId {0};
	uint8_t revision {0};
};

#endif /* kern_kr_hpp */
