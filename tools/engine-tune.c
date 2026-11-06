// Set runtime tunables on the synthetic-tone engine (needs root).  Build: clang -o local/engine-tune tools/engine-tune.c -framework IOKit -framework CoreFoundation
// Usage: sudo local/engine-tune tsmode=1 tick_ms=2
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("OhMyDSPAudioEngine"));
    if (!s) { printf("engine not found\n"); return 1; }
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '='); if (!eq) continue; *eq = 0; int v = atoi(eq + 1);
        CFStringRef k = CFStringCreateWithCString(NULL, argv[i], kCFStringEncodingUTF8); CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
        CFDictionarySetValue(d, k, n); printf("%s=%d\n", argv[i], v);
    }
    kern_return_t r = IORegistryEntrySetCFProperties(s, d); printf("result 0x%x\n", r); return r != 0;
}
