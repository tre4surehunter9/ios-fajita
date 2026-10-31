/*
 * gputest: the Adreno 630 from a normal iOS process, through SweetGPU's
 * NativeAdreno user client (xnu patch 0098, GPU-PLAN.md Phase 2.2).
 *
 *   Query, GetDeviceInfo, GetTimestamp
 *   BoAlloc + BoCpuMap (IOConnectMapMemory64) + BoMap: an IB buffer and a target
 *   100 submits, each an IB of CP_MEM_WRITEs, WaitFence, check from the CPU
 *   one submit of 16 IBs (the ABI maximum), each writing its own slot
 *   BoImport of the process's own pages: the GPU writes them, the CPU reads them
 *
 * Output goes to stdout (sweetrun puts it on /dev/console, so the USB log).
 * The last line is "GPUTEST PASSED" or "GPUTEST FAILED ...".
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <string.h>
#include "SweetGPUABI.h"

/* IOKit has no headers in the iPhoneOS SDK; the functions are in IOKit.framework. */
extern "C" {
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
kern_return_t IOServiceClose(io_connect_t connect);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector, const void *inputStruct,
    size_t inputStructCnt, void *outputStruct, size_t *outputStructCnt);
kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
    mach_vm_address_t *atAddress, mach_vm_size_t *ofSize, uint32_t options);
}
#define kIOMapAnywhere 1

static io_connect_t conn;
static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { printf("gputest: FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static kern_return_t
call(uint32_t method, const void *in, size_t insz, void *out, size_t outsz)
{
	size_t o = outsz;
	return IOConnectCallStructMethod(conn, method, in, insz, out, out ? &o : NULL);
}

/* PM4 type-7 header, as msm's adreno_pm4 */
static uint32_t
parity(uint32_t v)
{
	return (0x9669 >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^
	       (v >> 28)))) & 1;
}

static uint32_t
pkt7(uint32_t op, uint32_t cnt)
{
	return 0x70000000 | cnt | (parity(cnt) << 15) | ((op & 0x7f) << 16) | (parity(op) << 23);
}

#define CP_MEM_WRITE 0x3d

struct bo {
	uint64_t handle, iova, bytes;
	volatile uint32_t *cpu;
};

static bool
bo_make(struct bo *b, uint64_t bytes, uint64_t iova)
{
	NativeAdrenoAbiBoAlloc a = { bytes, 12, 0 };
	NativeAdrenoAbiBoAllocReply ar = {};
	kern_return_t kr = call(NativeAdrenoMethodBoAlloc, &a, sizeof(a), &ar, sizeof(ar));
	CHECK(kr == KERN_SUCCESS && ar.handle, "BoAlloc %llu bytes: 0x%x", bytes, kr);
	if (kr != KERN_SUCCESS) {
		return false;
	}
	NativeAdrenoAbiBoCpuMap cm = { ar.handle };
	NativeAdrenoAbiBoCpuMapReply cr = {};
	kr = call(NativeAdrenoMethodBoCpuMap, &cm, sizeof(cm), &cr, sizeof(cr));
	CHECK(kr == KERN_SUCCESS, "BoCpuMap: 0x%x", kr);
	mach_vm_address_t addr = 0;
	mach_vm_size_t size = 0;
	if (kr == KERN_SUCCESS) {
		kr = IOConnectMapMemory64(conn, cr.token, mach_task_self(), &addr, &size, kIOMapAnywhere);
		CHECK(kr == KERN_SUCCESS && size >= bytes, "IOConnectMapMemory64 token %u: 0x%x size %llu", cr.token, kr,
		    size);
	}
	NativeAdrenoAbiBoMap m = { ar.handle, iova, 3, 0, 2, 0 };
	NativeAdrenoAbiBoMapReply mr = {};
	kern_return_t kr2 = call(NativeAdrenoMethodBoMap, &m, sizeof(m), &mr, sizeof(mr));
	CHECK(kr2 == KERN_SUCCESS && mr.iova == iova, "BoMap at 0x%llx: 0x%x", iova, kr2);
	b->handle = ar.handle;
	b->iova = iova;
	b->bytes = bytes;
	b->cpu = (volatile uint32_t *)addr;
	return kr == KERN_SUCCESS && kr2 == KERN_SUCCESS && addr != 0;
}

static uint64_t
submit(const uint64_t *iova, const uint64_t *bytes, uint32_t n)
{
	NativeAdrenoAbiSubmit s = {};
	s.buffer_count = n;
	for (uint32_t i = 0; i < n; i++) {
		s.buffers[i].iova = iova[i];
		s.buffers[i].bytes = bytes[i];
	}
	NativeAdrenoAbiSubmitReply r = {};
	kern_return_t kr = call(NativeAdrenoMethodSubmit, &s, sizeof(s), &r, sizeof(r));
	CHECK(kr == KERN_SUCCESS && r.fence != 0, "Submit of %u IBs: 0x%x", n, kr);
	return kr == KERN_SUCCESS ? r.fence : 0;
}

static bool
wait_fence(uint64_t fence)
{
	NativeAdrenoAbiWaitFence w = { fence, 1000000000ULL };
	kern_return_t kr = call(NativeAdrenoMethodWaitFence, &w, sizeof(w), NULL, 0);
	CHECK(kr == KERN_SUCCESS, "WaitFence %llu: 0x%x", fence, kr);
	return kr == KERN_SUCCESS;
}

/* An IB at ib->cpu[off/4..] writing value to iova; returns its length in bytes. */
static uint64_t
write_ib(struct bo *ib, uint32_t off, uint64_t iova, uint32_t value)
{
	volatile uint32_t *p = ib->cpu + off / 4;
	p[0] = pkt7(CP_MEM_WRITE, 3);
	p[1] = (uint32_t)iova;
	p[2] = (uint32_t)(iova >> 32);
	p[3] = value;
	__asm__ volatile ("dsb sy" ::: "memory");
	return 16;
}

int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	io_service_t svc = MACH_PORT_NULL;
	for (int i = 0; i < 60 && svc == MACH_PORT_NULL; i++) {     /* the GPU comes up in parallel with boot */
		svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("SweetGPU"));
		if (svc == MACH_PORT_NULL) {
			sleep(1);
		}
	}
	if (svc == MACH_PORT_NULL) {
		printf("gputest: no SweetGPU service (sweetgpu=3?)\nGPUTEST FAILED no service\n");
		return 1;
	}
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 2, &conn);
	IOObjectRelease(svc);
	if (kr != KERN_SUCCESS) {
		printf("gputest: IOServiceOpen: 0x%x\nGPUTEST FAILED open\n", kr);
		return 1;
	}

	NativeAdrenoAbiParams q = {};
	kr = call(NativeAdrenoMethodQuery, NULL, 0, &q, sizeof(q));
	CHECK(kr == KERN_SUCCESS && q.abi_version == NativeAdrenoAbiVersion, "Query: 0x%x version %u", kr, q.abi_version);
	printf("gputest: ABI v%u, %u-bit GPU VA, %u-bit PA, max BO %llu MiB, flags 0x%x\n", q.abi_version, q.input_bits,
	    q.output_bits, q.max_bo_bytes >> 20, q.flags);
	NativeAdrenoAbiDeviceInfo di = {};
	kr = call(NativeAdrenoMethodGetDeviceInfo, NULL, 0, &di, sizeof(di));
	CHECK(kr == KERN_SUCCESS && di.chip_id == 0x06030001, "GetDeviceInfo: 0x%x chip 0x%x", kr, di.chip_id);
	printf("gputest: chip 0x%08x, GMEM %u KiB at 0x%llx, window 0x%llx + 0x%llx\n", di.chip_id, di.gmem_bytes >> 10,
	    di.gmem_base, di.va_start, di.va_size);
	NativeAdrenoAbiTimestamp t0 = {}, t1 = {};
	call(NativeAdrenoMethodGetTimestamp, NULL, 0, &t0, sizeof(t0));

	struct bo ib = {}, dst = {};
	const uint64_t base = di.va_start;
	if (!bo_make(&ib, 0x10000, base) || !bo_make(&dst, 0x10000, base + 0x100000)) {
		printf("GPUTEST FAILED buffers\n");
		return 1;
	}

	/* 100 single submits */
	unsigned good = 0;
	uint64_t s0 = mach_absolute_time();
	for (uint32_t i = 0; i < 100; i++) {
		dst.cpu[0] = 0;
		uint64_t len = write_ib(&ib, 0, dst.iova, 0x9e70000 | i);
		uint64_t f = submit(&ib.iova, &len, 1);
		if (f == 0 || !wait_fence(f)) {
			break;
		}
		good += dst.cpu[0] == (0x9e70000 | i);
	}
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	uint64_t us = (mach_absolute_time() - s0) * tb.numer / tb.denom / 1000;
	CHECK(good == 100, "single submits: %u/100 correct", good);
	printf("gputest: single submits %u/100 correct, %llu us each (round trip from userland)\n", good, us / 100);

	/* one submit of 16 IBs */
	uint64_t iovas[16], lens[16];
	for (uint32_t i = 0; i < 16; i++) {
		dst.cpu[64 + i] = 0;
		lens[i] = write_ib(&ib, 0x100 + 16 * i, dst.iova + 4 * (64 + i), 0x16b0000 | i);
		iovas[i] = ib.iova + 0x100 + 16 * i;
	}
	uint64_t f = submit(iovas, lens, 16);
	unsigned multi = 0;
	if (f != 0 && wait_fence(f)) {
		for (uint32_t i = 0; i < 16; i++) {
			multi += dst.cpu[64 + i] == (0x16b0000 | i);
		}
	}
	CHECK(multi == 16, "16-IB submit: %u/16 correct", multi);
	printf("gputest: one submit of 16 IBs: %u/16 correct, fence %llu\n", multi, f);

	/* import: the GPU writes this process's own (cacheable) page */
	vm_address_t own = 0;
	kr = vm_allocate(mach_task_self(), &own, 0x4000, VM_FLAGS_ANYWHERE);
	unsigned imported = 0;
	if (kr == KERN_SUCCESS) {
		volatile uint32_t *op = (volatile uint32_t *)own;
		op[0] = 0x11111111;
		for (uintptr_t a = own; a < own + 0x4000; a += 64) {
			__asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");  /* the GPU must see memory */
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		NativeAdrenoAbiBoImport im = { own, 0x4000 };
		NativeAdrenoAbiBoAllocReply ir = {};
		kr = call(NativeAdrenoMethodBoImport, &im, sizeof(im), &ir, sizeof(ir));
		CHECK(kr == KERN_SUCCESS, "BoImport: 0x%x", kr);
		NativeAdrenoAbiBoMap m = { ir.handle, base + 0x200000, 3, 0, 2, 0 };
		NativeAdrenoAbiBoMapReply mr = {};
		kern_return_t kr2 = kr == KERN_SUCCESS ? call(NativeAdrenoMethodBoMap, &m, sizeof(m), &mr, sizeof(mr)) : kr;
		CHECK(kr2 == KERN_SUCCESS, "BoMap of the import: 0x%x", kr2);
		if (kr == KERN_SUCCESS && kr2 == KERN_SUCCESS) {
			uint64_t len = write_ib(&ib, 0x400, base + 0x200000, 0x1a90a7ed);
			uint64_t iova = ib.iova + 0x400;
			uint64_t fi = submit(&iova, &len, 1);
			if (fi != 0 && wait_fence(fi)) {
				for (uintptr_t a = own; a < own + 0x4000; a += 64) {
					__asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
				}
				__asm__ volatile ("dsb sy" ::: "memory");
				imported = op[0] == 0x1a90a7ed;
			}
			CHECK(imported, "import: the CPU reads 0x%08x, want 0x1a90a7ed", op[0]);
		}
		NativeAdrenoAbiBoFree fr = { ir.handle };
		call(NativeAdrenoMethodBoFree, &fr, sizeof(fr), NULL, 0);
	}
	printf("gputest: import of this process's page: %s\n", imported ? "the GPU wrote it" : "FAILED");

	call(NativeAdrenoMethodGetTimestamp, NULL, 0, &t1, sizeof(t1));
	NativeAdrenoAbiStatus st = {};
	call(NativeAdrenoMethodGetStatus, NULL, 0, &st, sizeof(st));
	printf("gputest: GPU timestamp advanced %llu ticks, faults %llu\n", t1.ticks - t0.ticks, st.faults);
	CHECK(t1.ticks > t0.ticks && st.faults == 0, "timestamp/status");

	IOServiceClose(conn);
	if (failures) {
		printf("GPUTEST FAILED %d checks\n", failures);
		return 1;
	}
	printf("GPUTEST PASSED\n");
	return 0;
}
