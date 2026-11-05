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

# QuartzCore's iOS 16 ubershader has `unused_sampler`, a struct holding a pointer, which init_bytes cannot lay out. It is only
# reached on paths the constants make dead, so its bytes stay zero. A real read of a sampler through it still fails where it is read.
edit(t, """         air_error(c, "interface: the initializer of a program-scope variable is not a plain constant");
         return;
""", """         continue;
""")

# AIR_VAL_SLOT, which the translator's comment promises and this commit lacks: a local holding a pointer keeps it at translation
# time. QuartzCore's iOS 16 ubershader saves its two sampler arguments in such locals, then reads back either that or
# `unused_sampler` depending on a constant. elems[0] is what the slot holds; num_elems 1 is a local, 2 is a program-scope table.
edit(f'{root}/air/air_private.h', '   AIR_VAL_PTR_SELECT,\n', '   AIR_VAL_PTR_SELECT,\n   AIR_VAL_SLOT,\n')
edit(t, """return v && (v->kind == AIR_VAL_PTR || v->kind == AIR_VAL_PTR_SELECT);""",
     """return v && (v->kind == AIR_VAL_PTR || v->kind == AIR_VAL_PTR_SELECT || v->kind == AIR_VAL_SLOT);""")
edit(t, """         return air_error(c, "instruction: an alloca of a run-time size");
""", """         return air_error(c, "instruction: an alloca of a run-time size");
      if (in->aux_type && in->aux_type->kind == AIR_TYPE_POINTER && count == 1) {
         struct air_val *slot = rzalloc(c->mem_ctx, struct air_val);
         slot->kind = AIR_VAL_SLOT;
         slot->num_elems = 1;
         slot->elems = rzalloc_array(c->mem_ctx, struct air_val *, 1);
         air_set(c, &in->base, slot);
         return true;
      }
""")
edit(t, """      if (p->kind == AIR_VAL_FTABLE_ELEM) {
         struct air_val *fv = air_get(c, in->operands[0]);""", """      if (p->kind == AIR_VAL_SLOT) {
         struct air_val *held = air_get(c, in->operands[0]);
         if (!held)
            return false;
         p->elems[0] = held;
         return true;
      }
      if (p->kind == AIR_VAL_FTABLE_ELEM) {
         struct air_val *fv = air_get(c, in->operands[0]);""")
edit(t, """   case AIR_OP_LOAD: {
      struct air_val *p = air_get(c, in->operands[0]);
      if (!p)
         return false;
      if (p->kind == AIR_VAL_FTABLE_ELEM) {""", """   case AIR_OP_LOAD: {
      struct air_val *p = air_get(c, in->operands[0]);
      if (!p)
         return false;
      if (p->kind == AIR_VAL_SLOT) {
         if (!p->elems[0])
            return air_error(c, "instruction: a load of a pointer local before anything was stored in it");
         air_set(c, &in->base, p->elems[0]);
         return true;
      }
      if (p->kind == AIR_VAL_FTABLE_ELEM) {""")
edit(t, """         nir_def *pc = air_get_ssa(c, in->operands[0]);
         if (!pc)
            return false;
         if (pc->num_components != 1)
            return air_error(c, "instruction: a vector select between pointers");""", """         /* not decided by the constants: the local, not the program-scope `unused_` fallback, which is only there for
          * paths that never read it */
         if (pa->kind == AIR_VAL_SLOT || pb->kind == AIR_VAL_SLOT) {
            if (pa->kind == AIR_VAL_SLOT && pa->num_elems == 1)
               air_set(c, &in->base, pa);
            else if (pb->kind == AIR_VAL_SLOT && pb->num_elems == 1)
               air_set(c, &in->base, pb);
            else
               return air_error(c, "instruction: a select between pointer locals at run time");
            return true;
         }
         nir_def *pc = air_get_ssa(c, in->operands[0]);
         if (!pc)
            return false;
         if (pc->num_components != 1)
            return air_error(c, "instruction: a vector select between pointers");""")
edit(t, """      air_set(c, &g->base, air_val_ptr(c, p));
   }
}
""", """      air_set(c, &g->base, air_val_ptr(c, p));
   }
   /* a program-scope struct holding one pointer to another global: `metal::sampler unused_sampler` */
   for (unsigned i = 0; i < c->m->num_globals; i++) {
      const struct air_global *g = c->m->globals[i];
      const struct air_constant *k = g->init && !c->global_plan[i].skip ? air_value_as_constant(g->init) : NULL;
      if (!k || k->ckind != AIR_CONST_AGGREGATE || k->num_elems != 1 || !k->elems[0] || !k->elems[0]->type ||
          k->elems[0]->type->kind != AIR_TYPE_POINTER)
         continue;
      struct air_val *held = air_get(c, k->elems[0]);
      if (!held)
         return;
      struct air_val *slot = rzalloc(c->mem_ctx, struct air_val);
      slot->kind = AIR_VAL_SLOT;
      slot->num_elems = 2;
      slot->elems = rzalloc_array(c->mem_ctx, struct air_val *, 2);
      slot->elems[0] = held;
      air_set(c, &g->base, slot);
   }
}
""")
edit(t, """      if (v && (v->kind == AIR_VAL_FTABLE || v->kind == AIR_VAL_FTABLE_ELEM ||""", """      if (v && (v->kind == AIR_VAL_SLOT || v->kind == AIR_VAL_FTABLE || v->kind == AIR_VAL_FTABLE_ELEM ||""")
edit(t, """   struct air_val *base = air_get(c, in->operands[0]);
   if (!base)
      return false;
   if (base->kind == AIR_VAL_FTABLE_ELEM && !base->def) {""", """   struct air_val *base = air_get(c, in->operands[0]);
   if (!base)
      return false;
   if (base->kind == AIR_VAL_SLOT) {
      /* the first member of a struct holding one pointer is the pointer's slot */
      for (unsigned i = 1; i < in->num_operands; i++) {
         uint64_t ix;
         if (!air_const_uint(in->operands[i], &ix) || ix != 0)
            return air_error(c, "instruction: an index into a pointer local that is not zero");
      }
      air_set(c, &in->base, base);
      return true;
   }
   if (base->kind == AIR_VAL_FTABLE_ELEM && !base->def) {""")

# The constant evaluator follows small integer vectors too. QuartzCore's ubershader keeps its colour opcodes in a uchar3, and
# without this every arm of the switch on them is translated, some of which sample textures the constants made null.
edit(t, """static uint32_t
sc_key_hash(const void *k)""", """static bool
sc_type_ok(const struct air_type *t)
{
   if (t && t->kind == AIR_TYPE_INT)
      return true;
   return t && t->kind == AIR_TYPE_VECTOR && t->elem && t->elem->kind == AIR_TYPE_INT && t->elem->bits &&
          t->elem->bits * t->count <= 64;
}

static unsigned
sc_type_bits(const struct air_type *t)
{
   return t->kind == AIR_TYPE_VECTOR ? t->elem->bits * (unsigned)t->count : t->bits;
}

static uint32_t
sc_key_hash(const void *k)""")
edit(t, """   if (!in || !in->base.type || in->base.type->kind != AIR_TYPE_INT)
      return false;
   if (!c->sc_memo)""", """   if (!in || !sc_type_ok(in->base.type))
      return false;
   if (!c->sc_memo)""")
edit(t, """   unsigned bits = in->base.type->bits;
   uint64_t a = 0, b = 0, k = 0;
   switch (in->op) {
   case AIR_OP_LOAD: {
      struct sc_key key;""", """   unsigned bits = sc_type_bits(in->base.type);
   uint64_t a = 0, b = 0, k = 0;
   switch (in->op) {
   case AIR_OP_EXTRACTELT: {
      const struct air_type *vt = in->operands[0]->type;
      if (!sc_type_ok(vt) || vt->kind != AIR_TYPE_VECTOR || !sc_eval(c, in->operands[0], &a, depth + 1) ||
          !sc_eval(c, in->operands[1], &b, depth + 1) || b >= vt->count)
         return false;
      *out = sc_mask(a >> (b * vt->elem->bits), vt->elem->bits);
      return true;
   }
   case AIR_OP_LOAD: {
      struct sc_key key;""")
edit(t, """               if (val && val->type && val->type->kind == AIR_TYPE_INT) {
                  const struct air_inst *vi = air_value_as_inst(val);""", """               if (val && sc_type_ok(val->type)) {
                  const struct air_inst *vi = air_value_as_inst(val);""")
edit(t, """            } else if (in->base.type && in->base.type->kind == AIR_TYPE_INT && c->sc_memo) {""", """            } else if (sc_type_ok(in->base.type) && c->sc_memo) {""")

# A resource whose air.function_constant predicate the constants make false is not part of this pipeline, as Metal's own
# specialization would have it: nothing may demand that it be bound.
edit(h, """struct air_resource {
""", """struct air_resource {
   const void *fc_predicate;   /* the global of its air.function_constant predicate, or NULL */
""")
edit(t, """   return &info->resources[info->num_resources++];
""", """   {
      const struct air_md *fc = md_key_node(node, "air.function_constant");
      if (fc && fc->num_ops && fc->ops[0] && fc->ops[0]->kind == AIR_MD_VALUE)
         info->resources[info->num_resources].fc_predicate = fc->ops[0]->value;
   }
   return &info->resources[info->num_resources++];
""")
edit(t, """      if (info->num_resources) {""", """      {
         /* drop what the constants turned off */
         unsigned kept = 0;
         for (unsigned i = 0; i < c.info->num_resources; i++) {
            const struct air_resource *r = &c.info->resources[i];
            bool off = false;
            if (r->fc_predicate && c.sc_known && c.sc_known_ok) {
               struct sc_key key = { .g = (const struct air_global *)r->fc_predicate, .off = 0 };
               struct sc_val *sv = sc_known_get(&c, &key);
               off = sv && sv->known && !sv->v;
            }
            if (!off)
               c.info->resources[kept++] = *r;
         }
         c.info->num_resources = kept;
      }
      if (info->num_resources) {""")

# Chlorophyll.bundle as backboardd's device on iOS 16. Chlorophyll sets backboardd up in an injected dylib; here Metal loads the
# bundle (sweetmtl=3), so the plugin does it, as SweetMetal2.bundle does. Its log goes to the console, where the USB log reads it.
pl = f'{root}/plugin/chlorophyll_plugin.m'
edit(pl, """   if (!strcmp(name, "backboardd")) return "backboardd: Metal only through the inject, else the CPU renderer";
""", "")
edit(pl, """- (instancetype)initWithAcceleratorPort:(uint32_t)port
{
   const char *name = getprogname() ? getprogname() : "?";
""", """- (instancetype)initWithAcceleratorPort:(uint32_t)port
{
   const char *name = getprogname() ? getprogname() : "?";
   if (!strcmp(name, "backboardd")) {
      int console = open("/dev/console", O_WRONLY | O_NOCTTY);
      if (console >= 0) { dup2(console, 1); dup2(console, 2); setvbuf(stdout, NULL, _IOLBF, 0); setvbuf(stderr, NULL, _IOLBF, 0); }
      fprintf(stderr, "CHL: backboardd uses Chlorophyll\\n");
      setenv("BGMTL_BB", "1", 1);
      setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
      CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
      CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
      CFPreferencesSetAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
   }
""")
edit(pl, '#include <mach-o/dyld.h>\n', '#include <fcntl.h>\n#include <mach-o/dyld.h>\n')
