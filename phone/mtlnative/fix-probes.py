#!/usr/bin/env python3
"""fix-probes.py CHLOROPHYLL: Chlorophyll's probes as iOS 16 and its async device need them. Each edit must match once."""
import sys
root = sys.argv[1]

def edit(path, old, new):
    s = open(path).read()
    if s.count(old) != 1:
        sys.exit(f'{path}: {s.count(old)} matches for: {old}')
    open(path, 'w').write(s.replace(old, new))

# T4 reads the status straight after commit: written when commits were synchronous; every other test waits first.
edit(f'{root}/test/bgmtl_probe.m',
     'if ([cb status] != 4) { printf("  round %u: status %ld\\n", r, (long)[cb status]); break; }',
     '[cb waitUntilCompleted]; if ([cb status] != 4) { printf("  round %u: status %ld\\n", r, (long)[cb status]); break; }')

# iOS 16's Metal does not offer these two by name: use the device directly. CARenderer gets it from the texture and queue.
p = f'{root}/test/bgmtl_carenderer_probe.m'
edit(p, 'if (!all || !add) { printf("no Metal registration entry points\\n"); return 2; }',
     'printf("MTLCopyAllDevices %s, MTLAddDevice %s, MTLCreateSystemDefaultDevice %s\\n", all ? "found" : "absent", add ? "found" : "absent", '
     'dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice") ? "found" : "absent");')
edit(p, 'printf("devices before: %lu\\n", (unsigned long)[all() count]);', 'if (all) printf("devices before: %lu\\n", (unsigned long)[all() count]);')
edit(p, 'add(dev);\n', 'if (add) add(dev);\n')
edit(p, 'id<MTLDevice> d = ((id (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice"))();',
     'id<MTLDevice> sysdev = ((id (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice"))();\n'
     '      printf("system default device: %s\\n", sysdev ? [[sysdev name] UTF8String] : "nil");\n'
     '      id<MTLDevice> d = dev;')
