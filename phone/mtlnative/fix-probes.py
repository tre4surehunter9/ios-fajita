#!/usr/bin/env python3
"""fix-probes.py CHLOROPHYLL: Chlorophyll as iOS 16 needs it (probes, function constants). Each edit must match once."""
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
# MTLAddDevice asserts until Metal has enumerated once. BG_NOADD: the same run with the device never registered.
edit(p, 'add(dev);\n',
     'id<MTLDevice> firstdev = ((id (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice"))();\n'
     '      printf("before registering, system default device: %s\\n", firstdev ? [[firstdev name] UTF8String] : "nil");\n'
     '      printf("registering: %s\\n", getenv("BG_NOADD") ? "no" : "yes");\n'
     '      if (add && !getenv("BG_NOADD")) add(dev);\n')
edit(p, 'id<MTLDevice> d = ((id (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice"))();',
     'id<MTLDevice> sysdev = ((id (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice"))();\n'
     '      printf("system default device: %s\\n", sysdev ? [[sysdev name] UTF8String] : "nil");\n'
     '      id<MTLDevice> d = dev;')

# Real function constants (phone/mtlnative/bg_specialize.m): the translator's AIR_FC_SET list, per thread, by name or #index.
h = f'{root}/air/nir_air.h'
edit(h, 'unsigned air_module_function_constants(void *mem_ctx, const void *bitcode, size_t size,',
     'extern __thread const char *air_fc_set_tls;\nunsigned air_module_function_constants(void *mem_ctx, const void *bitcode, size_t size,')
t = f'{root}/air/air_to_nir.c'
edit(t, 'const char *set = getenv("AIR_FC_SET");', 'const char *set = air_fc_set_tls ? air_fc_set_tls : getenv("AIR_FC_SET");')
edit(t, """(*out)[num].bits = strtoull(q + nl + 1, NULL, 0);
""", """(*out)[num].bits = strtoull(q + nl + 1, NULL, 0);
                     else if (q[0] == '#' && strtoull(q + 1, NULL, 10) == idx && strchr(q, '='))
                        (*out)[num].bits = strtoull(strchr(q, '=') + 1, NULL, 0);
""")
edit(t, '/* -- the interface ------------------------------------------------------ */',
     '__thread const char *air_fc_set_tls;\n\n/* -- the interface ------------------------------------------------------ */')
a = f'{root}/air/air_metal.c'
edit(a, """struct rwa_compute *
rwa_compute_create(struct rwa_device *device, const void *bitcode, size_t size,""",
     """void rwa_set_function_constants(const char *values);
void rwa_set_function_constants(const char *values) { air_fc_set_tls = values; }

struct rwa_compute *
rwa_compute_create(struct rwa_device *device, const void *bitcode, size_t size,""")
d = f'{root}/src/bgmtl.m'
edit(d, '#include "air_metal.h"\n', '#include "air_metal.h"\nextern void bg_fc_begin(id vertex, id fragment), bg_fc_end(void);\n')
edit(d, """   bg_lock();
   struct rwa_render *r = rwa_render_create(""", """   bg_fc_begin(vertex, fragment);
   bg_lock();
   struct rwa_render *r = rwa_render_create(""")
edit(d, """&shape, &info, why, sizeof(why));
   bg_unlock();
""", """&shape, &info, why, sizeof(why));
   bg_unlock();
   bg_fc_end();
""")
edit(d, """   bg_lock();
   struct rwa_compute *c = rwa_compute_create(_rwa, air.bytes, air.length, [[function name] UTF8String], &info, why, sizeof(why));
   bg_unlock();
""", """   bg_fc_begin(nil, function);
   bg_lock();
   struct rwa_compute *c = rwa_compute_create(_rwa, air.bytes, air.length, [[function name] UTF8String], &info, why, sizeof(why));
   bg_unlock();
   bg_fc_end();
""")
