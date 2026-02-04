#include "common.h"

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#include "log.h"

void sc_hide_from_dock(void) {
    @autoreleasepool {
        // Initialize NSApplication and set activation policy
        // This must be done before SDL creates any windows
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        
        LOGD("Application activation policy set to Accessory (hidden from Dock)");
    }
}

#endif
