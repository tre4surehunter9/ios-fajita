/*
 * Function constants for BGMTLDevice on iOS 16.
 *
 * CoreAnimation specializes every shader: MTLFunctionDescriptor (name, MTLFunctionConstantValues), then
 * -[MTLLibrary newFunctionWithDescriptor:error:] (QuartzCore's CA::OGL::MetalContext::load_shader_function), and aborts if
 * that returns nil. Metal runs it in its own classes through a compiler this device does not have. Chlorophyll takes every
 * constant as zero, which leaves the call unanswered here and CoreAnimation's ubershaders wrong.
 *
 * So: the setters of MTLFunctionConstantValues are recorded, the call is answered with the plain function behind a proxy that
 * remembers the values, and bg_fc_begin hands them to the AIR translator while a pipeline is built (air_module_function_constants).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void rwa_set_function_constants(const char *values);

static const void *const kValues = &kValues;

static NSMutableDictionary *recorded(id values, BOOL create)
{
	NSMutableDictionary *d = objc_getAssociatedObject(values, kValues);
	if (d == nil && create) {
		d = [NSMutableDictionary dictionary];
		objc_setAssociatedObject(values, kValues, d, OBJC_ASSOCIATION_RETAIN);
	}
	return d;
}

static size_t type_size(NSUInteger type)
{
	switch (type) {
	case MTLDataTypeBool: case MTLDataTypeChar: case MTLDataTypeUChar: return 1;
	case MTLDataTypeShort: case MTLDataTypeUShort: case MTLDataTypeHalf: return 2;
	case MTLDataTypeLong: case MTLDataTypeULong: return 8;
	default: return 4;
	}
}

static void record(id values, id key, const void *p, NSUInteger type)
{
	uint64_t bits = 0;
	memcpy(&bits, p, type_size(type));
	NSMutableDictionary *d = recorded(values, YES);
	@synchronized (d) {
		d[key] = @(bits);
	}
}

static IMP orig_desc, orig_named, orig_set_values;
static unsigned g_recorded;

/*
 * Which class holds the setters is Metal's business (it also has MTLFunctionConstantValuesInternal), so each class that has its
 * own implementation is hooked, with its original kept here. A class is found by walking up from the receiver.
 */
enum { MAX_HOOKED = 12 };
struct hooked { Class cls; SEL sel; IMP orig; };
static struct hooked g_hooked[MAX_HOOKED];
static unsigned g_nhooked;

static IMP original(id self, SEL s)
{
	for (Class c = object_getClass(self); c != Nil; c = class_getSuperclass(c)) {
		for (unsigned i = 0; i < g_nhooked; i++) {
			if (g_hooked[i].cls == c && g_hooked[i].sel == s) {
				return g_hooked[i].orig;
			}
		}
	}
	return NULL;
}

static void hook_name(id self, SEL s, const void *v, NSUInteger type, NSString *name)
{
	record(self, name, v, type);
	g_recorded++;
	((void (*)(id, SEL, const void *, NSUInteger, NSString *))original(self, s))(self, s, v, type, name);
}

static void hook_index(id self, SEL s, const void *v, NSUInteger type, NSUInteger index)
{
	record(self, @(index), v, type);
	g_recorded++;
	((void (*)(id, SEL, const void *, NSUInteger, NSUInteger))original(self, s))(self, s, v, type, index);
}

static void hook_range(id self, SEL s, const void *v, NSUInteger type, NSRange range)
{
	const char *p = v;
	for (NSUInteger i = 0; i < range.length; i++) {
		record(self, @(range.location + i), p + i * type_size(type), type);
	}
	g_recorded++;
	((void (*)(id, SEL, const void *, NSUInteger, NSRange))original(self, s))(self, s, v, type, range);
}

/* A copy keeps what was recorded: the descriptor's setter may well copy. */
static id hook_copy(id self, SEL s, NSZone *zone)
{
	id c = ((id (*)(id, SEL, NSZone *))original(self, s))(self, s, zone);
	NSMutableDictionary *d = recorded(self, NO);
	if (c != nil && c != self && d != nil) {
		@synchronized (d) {
			objc_setAssociatedObject(c, kValues, [[d mutableCopy] autorelease], OBJC_ASSOCIATION_RETAIN);
		}
	}
	return c;
}

/* And the descriptor remembers the object it was handed. */
static const void *const kGiven = &kGiven;

static void hook_set_values(id self, SEL s, id values)
{
	objc_setAssociatedObject(self, kGiven, values, OBJC_ASSOCIATION_RETAIN);
	((void (*)(id, SEL, id))orig_set_values)(self, s, values);
}

/* The plain function, plus the values; everything else goes to the function. */
@interface BGSpecializedFunction : NSProxy {
@public
	id _base;
	NSDictionary *_constants;
}
@end

@implementation BGSpecializedFunction
- (id)forwardingTargetForSelector:(SEL)s { return _base; }
- (NSMethodSignature *)methodSignatureForSelector:(SEL)s { return [_base methodSignatureForSelector:s]; }
- (void)forwardInvocation:(NSInvocation *)inv { [inv invokeWithTarget:_base]; }
- (BOOL)respondsToSelector:(SEL)s { return [_base respondsToSelector:s]; }
- (BOOL)conformsToProtocol:(Protocol *)p { return [_base conformsToProtocol:p]; }
- (BOOL)isKindOfClass:(Class)c { return [_base isKindOfClass:c]; }
- (void)dealloc
{
	[_base release];
	[_constants release];
	[super dealloc];
}
@end

static id specialized(id library, NSString *name, id values, NSError **error)
{
	id base = ((id (*)(id, SEL, NSString *))objc_msgSend)(library, @selector(newFunctionWithName:), name);
	if (base == nil) {
		printf("bg_specialize: no function %s\n", name.UTF8String);
		if (error != NULL) {
			*error = [NSError errorWithDomain:@"MTLLibraryErrorDomain" code:1
			    userInfo:@{ NSLocalizedDescriptionKey : [@"no function " stringByAppendingString:name ? name : @"(nil)"] }];
		}
		return nil;
	}
	if (error != NULL) {
		*error = nil;
	}
	NSMutableDictionary *d = values != nil ? recorded(values, NO) : nil;
	if (d == nil) {
		return base;
	}
	BGSpecializedFunction *f = [BGSpecializedFunction alloc];
	f->_base = base;
	@synchronized (d) {
		f->_constants = [d copy];
	}
	return f;
}

/* Once, for the log: what Metal itself answers. */
static void report_metal(id self, SEL s, IMP orig, id arg)
{
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		NSError *e = nil;
		id r = ((id (*)(id, SEL, id, NSError **))orig)(self, s, arg, &e);
		printf("bg_specialize: Metal's %s on %s: %s (%s)\n", sel_getName(s), class_getName(object_getClass(self)),
		    r ? "made a function" : "nil", e ? e.localizedDescription.UTF8String : "no error");
		[r release];
	});
}

static id hook_desc(id self, SEL s, MTLFunctionDescriptor *desc, NSError **error)
{
	report_metal(self, s, orig_desc, desc);
	id given = objc_getAssociatedObject(desc, kGiven);
	id values = given != nil ? given : desc.constantValues;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		printf("bg_specialize: %s: descriptor %s, values %s (given %s), %lu recorded here, %u setter calls so far\n",
		    desc.name.UTF8String, class_getName(object_getClass(desc)), values ? class_getName(object_getClass(values)) : "nil",
		    given ? "yes" : "no", (unsigned long)(values ? recorded(values, NO).count : 0), g_recorded);
	});
	return specialized(self, desc.name, values, error);
}

/* -newFunctionWithName:constantValues:error: has three arguments; the report above is for the descriptor call. */
static id hook_named(id self, SEL s, NSString *name, id values, NSError **error)
{
	return specialized(self, name, values, error);
}

static IMP swap_imp(Class c, SEL s, IMP with)
{
	Method m = class_getInstanceMethod(c, s);
	if (m == NULL) {
		printf("bg_specialize: %s has no %s\n", class_getName(c), sel_getName(s));
		return NULL;
	}
	return method_setImplementation(m, with);
}

/* Each class in the list with an implementation of its own gets the hook. */
static void hook_class(Class c, SEL s, IMP with)
{
	Method m = c != Nil ? class_getInstanceMethod(c, s) : NULL;
	if (m == NULL || g_nhooked == MAX_HOOKED) {
		return;
	}
	if (method_getImplementation(m) == with) {
		return;                 /* inherited from a class already done */
	}
	/* the class that really owns it: the original is found by walking up, so record that one */
	Class owner = c;
	for (Class up = class_getSuperclass(c); up != Nil && class_getInstanceMethod(up, s) == m; up = class_getSuperclass(up)) {
		owner = up;
	}
	g_hooked[g_nhooked++] = (struct hooked){ owner, s, method_setImplementation(m, with) };
	printf("bg_specialize: hooked -[%s %s]\n", class_getName(owner), sel_getName(s));
}

__attribute__((constructor)) static void bg_specialize_install(void)
{
	Class l = NSClassFromString(@"_MTLLibrary"), desc = NSClassFromString(@"MTLFunctionDescriptor");
	const char *const names[] = { "MTLFunctionConstantValuesInternal", "_MTLFunctionConstantValues", "MTLFunctionConstantValues" };
	if (l == Nil || desc == Nil) {
		printf("bg_specialize: _MTLLibrary %p, MTLFunctionDescriptor %p: not installed\n", l, desc);
		return;
	}
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		Class v = objc_getClass(names[i]);
		hook_class(v, @selector(setConstantValue:type:withName:), (IMP)hook_name);
		hook_class(v, @selector(setConstantValue:type:atIndex:), (IMP)hook_index);
		hook_class(v, @selector(setConstantValues:type:withRange:), (IMP)hook_range);
		hook_class(v, @selector(copyWithZone:), (IMP)hook_copy);
	}
	orig_set_values = swap_imp(desc, @selector(setConstantValues:), (IMP)hook_set_values);
	orig_desc = swap_imp(l, @selector(newFunctionWithDescriptor:error:), (IMP)hook_desc);
	orig_named = swap_imp(l, @selector(newFunctionWithName:constantValues:error:), (IMP)hook_named);
}

static void append(NSMutableString *out, id fn)
{
	if (fn == nil || object_getClass(fn) != [BGSpecializedFunction class]) {
		return;
	}
	[((BGSpecializedFunction *)fn)->_constants enumerateKeysAndObjectsUsingBlock:^(id key, NSNumber *bits, BOOL *stop) {
		if ([key isKindOfClass:[NSString class]]) {
			[out appendFormat:@"%@=0x%llx,", key, bits.unsignedLongLongValue];
		} else {
			[out appendFormat:@"#%llu=0x%llx,", [key unsignedLongLongValue], bits.unsignedLongLongValue];
		}
	}];
}

/* Between these two, this thread's translations use the functions' values. The fragment's come last and win. */
static __thread char *g_values;

void bg_fc_begin(id vertex, id fragment)
{
	NSMutableString *s = [NSMutableString string];
	append(s, vertex);
	append(s, fragment);
	g_values = s.length ? strdup(s.UTF8String) : NULL;
	if (g_values != NULL) {
		/* what reproduces this pipeline on the PC: AIR_FC_SET=<values> air2nir LIB FUNCTION */
		printf("bg_specialize: %s: %s\n", [[fragment name] UTF8String], g_values);
	}
	rwa_set_function_constants(g_values);
}

void bg_fc_end(void)
{
	rwa_set_function_constants(NULL);
	free(g_values);
	g_values = NULL;
}
