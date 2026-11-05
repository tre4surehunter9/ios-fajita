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

static IMP orig_name, orig_index, orig_range, orig_desc, orig_named;

static void hook_name(id self, SEL s, const void *v, NSUInteger type, NSString *name)
{
	record(self, name, v, type);
	((void (*)(id, SEL, const void *, NSUInteger, NSString *))orig_name)(self, s, v, type, name);
}

static void hook_index(id self, SEL s, const void *v, NSUInteger type, NSUInteger index)
{
	record(self, @(index), v, type);
	((void (*)(id, SEL, const void *, NSUInteger, NSUInteger))orig_index)(self, s, v, type, index);
}

static void hook_range(id self, SEL s, const void *v, NSUInteger type, NSRange range)
{
	const char *p = v;
	for (NSUInteger i = 0; i < range.length; i++) {
		record(self, @(range.location + i), p + i * type_size(type), type);
	}
	((void (*)(id, SEL, const void *, NSUInteger, NSRange))orig_range)(self, s, v, type, range);
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
	return specialized(self, desc.name, desc.constantValues, error);
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

__attribute__((constructor)) static void bg_specialize_install(void)
{
	Class v = NSClassFromString(@"MTLFunctionConstantValues"), l = NSClassFromString(@"_MTLLibrary");
	if (v == Nil || l == Nil) {
		printf("bg_specialize: MTLFunctionConstantValues %p, _MTLLibrary %p: not installed\n", v, l);
		return;
	}
	orig_name = swap_imp(v, @selector(setConstantValue:type:withName:), (IMP)hook_name);
	orig_index = swap_imp(v, @selector(setConstantValue:type:atIndex:), (IMP)hook_index);
	orig_range = swap_imp(v, @selector(setConstantValues:type:withRange:), (IMP)hook_range);
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
	rwa_set_function_constants(g_values);
}

void bg_fc_end(void)
{
	rwa_set_function_constants(NULL);
	free(g_values);
	g_values = NULL;
}
