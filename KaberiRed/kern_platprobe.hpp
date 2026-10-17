//
//  kern_platprobe.hpp
//  KaberiRed
//
//  Platform probe (-krplatprobe): a read-only diagnostic for whole-machine freezes early in boot. See the .cpp.
//

#ifndef kern_platprobe_hpp
#define kern_platprobe_hpp

namespace PlatformProbe {
	// Starts the probe thread if -krplatprobe is set. Safe to call once, as early as possible.
	void start();
}

#endif /* kern_platprobe_hpp */
