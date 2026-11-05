/*
 * libdrm stand-in: Mesa freedreno's msm backend over SweetGPU's NativeAdreno interface (xnu 0098/0099,
 * phone/turnip/NativeAdrenoABI.h). The test build of the native Metal route (GPU-PLAN.md).
 *
 * The idea comes from Bergamota's gpu/freedreno-sweet (MIT), which forwards the same requests to a kernel that
 * speaks msm. Ours does not, so each request is translated here:
 *   - the reported version is msm 1.9, so Mesa uses its softpin submit path (no relocations);
 *   - Mesa asks the kernel for each BO's address (GET_IOVA) but NativeAdreno has the client choose it, so the
 *     addresses are handed out here from this connection's window and mapped at GEM_NEW;
 *   - a BO's last fence is kept here, for GEM_CPU_PREP;
 *   - offset for mmap = BO handle = the kernel's CPU-map token (sweet_drm_mmap, called by Bergamota's Mesa patch).
 * Every "device fd" is a dup of /dev/null and means the one connection of this process.
 */
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <msm_drm.h>
typedef struct NativeAdrenoAbiSubmitBuffer NativeAdrenoAbiSubmitBuffer;   /* the header is written for C++ */
#include "NativeAdrenoABI.h"

/* phone/include/IOKit/IOKitLib.h is small on purpose (the SDK has no IOKit headers); the rest this file needs: */
#define kIOReturnNoMemory       ((kern_return_t)0xe00002bd)
#define kIOReturnNoResources    ((kern_return_t)0xe00002be)
#define kIOReturnBadArgument    ((kern_return_t)0xe00002c2)
#define kIOReturnBusy           ((kern_return_t)0xe00002d5)
#define kIOReturnTimeout        ((kern_return_t)0xe00002d6)
#define kIOMapAnywhere          0x00000001u
#define kIOMapWriteCombineCache 0x00000400u
#define kIOMainPortDefault      ((mach_port_t)0)
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t port, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOConnectCallStructMethod(io_connect_t c, uint32_t selector, const void *in, size_t insz, void *out, size_t *outsz);
extern kern_return_t IOConnectMapMemory64(io_connect_t c, uint32_t type, task_t task, mach_vm_address_t *addr, mach_vm_size_t *size, uint32_t options);
extern kern_return_t IOConnectUnmapMemory64(io_connect_t c, uint32_t type, task_t task, mach_vm_address_t addr);

#define EXPORT __attribute__((visibility("default")))
#define OPEN_TYPE 0x4147u               /* as Turnip's darwin backend opens SweetGPU */
#define PAGE_16K 0x4000ull
#define MAX_BOS 65536u
#define MAX_FREE 8192u
#define KERNEL_MAX_FREQ 710000000ull

struct bo {
	uint64_t iova, size;
	uint32_t fence;                 /* the last submit that used it */
	uint64_t map;                   /* CPU address, 0 if not mapped */
	IOSurfaceRef surface;           /* an import: held until the BO closes; map is the surface's own mapping */
};
struct extent { uint64_t base, size; };

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static io_connect_t g_conn;
static struct NativeAdrenoAbiDeviceInfo g_info;
static struct bo *g_bo;                 /* index = handle */
static uint64_t g_va_next;
static struct extent g_free[MAX_FREE];
static uint32_t g_nfree, g_last_fence, g_top_handle;

static int kerr(kern_return_t kr)
{
	switch (kr) {
	case KERN_SUCCESS:              return 0;
	case kIOReturnTimeout:          return -ETIMEDOUT;
	case kIOReturnNoMemory:         return -ENOMEM;
	case kIOReturnNoResources:      return -ENOSPC;
	case kIOReturnBusy:             return -EBUSY;
	case kIOReturnBadArgument:      return -EINVAL;
	default:                        return -EIO;
	}
}

static int call(uint32_t method, const void *in, size_t insz, void *out, size_t outsz)
{
	size_t got = outsz;
	kern_return_t kr = IOConnectCallStructMethod(g_conn, method, in, insz, out, out ? &got : NULL);
	if (kr != KERN_SUCCESS && kr != kIOReturnTimeout && kr != kIOReturnBusy) {
		printf("sweet_native_drm: method %u -> 0x%x\n", method, kr);
	}
	return kerr(kr);
}

/* With g_lock held; the connection and the tables are made once. */
static int connect_once(void)
{
	if (g_conn != 0) {
		return 0;
	}
	io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("SweetGPU"));
	if (s == 0) {
		printf("sweet_native_drm: no SweetGPU service (boot with sweetgpu=3)\n");
		return -ENODEV;
	}
	kern_return_t kr = IOServiceOpen(s, mach_task_self(), OPEN_TYPE, &g_conn);
	IOObjectRelease(s);
	if (kr != KERN_SUCCESS) {
		printf("sweet_native_drm: IOServiceOpen -> 0x%x\n", kr);
		g_conn = 0;
		return -ENODEV;
	}
	g_bo = calloc(MAX_BOS + 1, sizeof(*g_bo));
	int r = g_bo ? call(NativeAdrenoMethodGetDeviceInfo, NULL, 0, &g_info, sizeof(g_info)) : -ENOMEM;
	if (r) {
		return r;
	}
	g_va_next = g_info.va_start;
	printf("sweet_native_drm: chip 0x%x, window 0x%llx + 0x%llx\n", g_info.chip_id,
	    (unsigned long long)g_info.va_start, (unsigned long long)g_info.va_size);
	return 0;
}

/* First fit from what was freed, else the top of the window. With g_lock held. */
static uint64_t va_alloc(uint64_t size)
{
	for (uint32_t i = 0; i < g_nfree; i++) {
		if (g_free[i].size >= size) {
			uint64_t a = g_free[i].base;
			g_free[i].base += size;
			g_free[i].size -= size;
			if (g_free[i].size == 0) {
				g_free[i] = g_free[--g_nfree];
			}
			return a;
		}
	}
	if (g_va_next + size > g_info.va_start + g_info.va_size) {
		return 0;
	}
	uint64_t a = g_va_next;
	g_va_next += size;
	return a;
}

static void va_free(uint64_t base, uint64_t size)
{
	if (g_nfree < MAX_FREE) {
		g_free[g_nfree++] = (struct extent){ base, size };
	}                                       /* else that address range stays unused; the window is 4 GiB */
}

static int gem_new(struct drm_msm_gem_new *rq)
{
	if (rq->flags & MSM_BO_CACHED_COHERENT) {
		return -EINVAL;                 /* NativeAdreno has no IO-coherent memory */
	}
	uint64_t size = (rq->size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	struct NativeAdrenoAbiBoAlloc alloc = { .bytes = size, .align_log2 = 12, .contiguous = 0 };
	struct NativeAdrenoAbiBoAllocReply reply = { 0 };
	int r = call(NativeAdrenoMethodBoAlloc, &alloc, sizeof(alloc), &reply, sizeof(reply));
	if (r) {
		return r;
	}
	if (reply.handle == 0 || reply.handle > MAX_BOS) {
		return -ENOSPC;
	}
	uint64_t iova = va_alloc(size);
	struct NativeAdrenoAbiBoMap map = { .handle = reply.handle, .iova = iova, .permissions = 3, .writeback = 0,
		.shareability = 2 };
	struct NativeAdrenoAbiBoMapReply mapped = { 0 };
	r = iova ? call(NativeAdrenoMethodBoMap, &map, sizeof(map), &mapped, sizeof(mapped)) : -ENOSPC;
	if (r) {
		struct NativeAdrenoAbiBoFree f = { reply.handle };
		call(NativeAdrenoMethodBoFree, &f, sizeof(f), NULL, 0);
		if (iova) {
			va_free(iova, size);
		}
		return r;
	}
	g_bo[reply.handle] = (struct bo){ .iova = iova, .size = size };
	if (reply.handle > g_top_handle) {
		g_top_handle = (uint32_t)reply.handle;
	}
	rq->handle = (uint32_t)reply.handle;
	return 0;
}

static int gem_close(uint32_t handle)
{
	if (handle == 0 || handle > MAX_BOS || g_bo[handle].iova == 0) {
		return -EINVAL;
	}
	struct bo *b = &g_bo[handle];
	if (b->map != 0 && b->surface == NULL) {
		IOConnectUnmapMemory64(g_conn, handle, mach_task_self(), b->map);
	}
	struct NativeAdrenoAbiBoFree f = { handle };
	int r = call(NativeAdrenoMethodBoFree, &f, sizeof(f), NULL, 0);
	va_free(b->iova, b->size);
	if (b->surface != NULL) {
		CFRelease(b->surface);
	}
	memset(b, 0, sizeof(*b));
	return r;
}

static uint64_t rel_ns(const struct drm_msm_timespec *t)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t ns = (t->tv_sec - now.tv_sec) * 1000000000ll + (t->tv_nsec - now.tv_nsec);
	return ns > 0 ? (uint64_t)ns : 0;
}

static int wait_fence(uint32_t fence, uint64_t ns)
{
	struct NativeAdrenoAbiWaitFence w = { .fence = fence, .timeout_ns = ns };
	return fence == 0 ? 0 : call(NativeAdrenoMethodWaitFence, &w, sizeof(w), NULL, 0);
}

static int get_param(struct drm_msm_param *p)
{
	struct NativeAdrenoAbiTimestamp ts = { 0 };
	struct NativeAdrenoAbiStatus st = { 0 };
	int r = 0;
	switch (p->param) {
	case MSM_PARAM_GPU_ID:          p->value = 630; break;
	case MSM_PARAM_GMEM_SIZE:       p->value = g_info.gmem_bytes; break;
	case MSM_PARAM_CHIP_ID:         p->value = g_info.chip_id; break;
	case MSM_PARAM_MAX_FREQ:        p->value = KERNEL_MAX_FREQ; break;
	case MSM_PARAM_GMEM_BASE:       p->value = g_info.gmem_base; break;
	case MSM_PARAM_PRIORITIES:      p->value = 1; break;
	case MSM_PARAM_PP_PGTABLE:      p->value = 0; break;
	case MSM_PARAM_VA_START:        p->value = g_info.va_start; break;
	case MSM_PARAM_VA_SIZE:         p->value = g_info.va_size; break;
	case MSM_PARAM_HIGHEST_BANK_BIT: p->value = g_info.highest_bank_bit; break;
	case MSM_PARAM_UBWC_SWIZZLE:    p->value = g_info.ubwc_swizzle; break;
	case MSM_PARAM_MACROTILE_MODE:  p->value = g_info.macrotile_mode; break;
	case MSM_PARAM_TIMESTAMP:
		r = call(NativeAdrenoMethodGetTimestamp, NULL, 0, &ts, sizeof(ts));
		p->value = ts.ticks;
		break;
	case MSM_PARAM_FAULTS:
	case MSM_PARAM_SUSPENDS:
		r = call(NativeAdrenoMethodGetStatus, NULL, 0, &st, sizeof(st));
		p->value = p->param == MSM_PARAM_FAULTS ? st.faults : st.suspends;
		break;
	default:
		return -EINVAL;
	}
	return r;
}

static int gem_info(struct drm_msm_gem_info *i)
{
	if (i->handle == 0 || i->handle > MAX_BOS || g_bo[i->handle].iova == 0) {
		return -EINVAL;
	}
	switch (i->info) {
	case MSM_INFO_GET_OFFSET:       i->value = i->handle; return 0;
	case MSM_INFO_GET_IOVA:         i->value = g_bo[i->handle].iova; return 0;
	case MSM_INFO_SET_NAME:         return 0;
	default:                        return -EINVAL;
	}
}

/* With g_lock held: the kernel takes 16 buffers at a time, its ring keeps them in order. */
static int submit_some(const struct NativeAdrenoAbiSubmit *s, uint32_t *fence)
{
	struct NativeAdrenoAbiSubmitReply reply = { 0 };
	int r = -EBUSY;
	for (int tries = 0; r == -EBUSY && tries < 5000; tries++) {
		r = call(NativeAdrenoMethodSubmit, s, sizeof(*s), &reply, sizeof(reply));
		if (r == -EBUSY) {
			usleep(1000);
		}
	}
	if (r == 0) {
		*fence = (uint32_t)reply.fence;
	}
	return r;
}

static int gem_submit(struct drm_msm_gem_submit *rq)
{
	const struct drm_msm_gem_submit_bo *bos = (const void *)(uintptr_t)rq->bos;
	const struct drm_msm_gem_submit_cmd *cmds = (const void *)(uintptr_t)rq->cmds;
	struct NativeAdrenoAbiSubmit s;
	uint32_t fence = g_last_fence;
	memset(&s, 0, sizeof(s));
	for (uint32_t i = 0; i < rq->nr_cmds; i++) {
		const struct drm_msm_gem_submit_cmd *c = &cmds[i];
		if (c->type == MSM_SUBMIT_CMD_IB_TARGET_BUF) {
			continue;               /* only a target of another IB; msm skips it too */
		}
		if (c->submit_idx >= rq->nr_bos || c->nr_relocs != 0) {
			return -EINVAL;
		}
		uint32_t h = bos[c->submit_idx].handle;
		if (h == 0 || h > MAX_BOS || g_bo[h].iova == 0) {
			return -EINVAL;
		}
		s.buffers[s.buffer_count++] = (struct NativeAdrenoAbiSubmitBuffer){ .handle = h,
			.iova = g_bo[h].iova + c->submit_offset, .bytes = c->size };
		if (s.buffer_count == NativeAdrenoAbiMaxBuffers) {
			int r = submit_some(&s, &fence);
			if (r) {
				return r;
			}
			s.buffer_count = 0;
		}
	}
	if (s.buffer_count != 0) {
		int r = submit_some(&s, &fence);
		if (r) {
			return r;
		}
	}
	for (uint32_t i = 0; i < rq->nr_bos; i++) {
		if (bos[i].handle != 0 && bos[i].handle <= MAX_BOS) {
			g_bo[bos[i].handle].fence = fence;
		}
	}
	g_last_fence = fence;
	rq->fence = fence;
	rq->fence_fd = -1;
	return 0;
}

static int command(unsigned long idx, void *data)
{
	switch (idx) {
	case DRM_MSM_GET_PARAM:         return get_param(data);
	case DRM_MSM_SET_PARAM:         return -EINVAL;
	case DRM_MSM_GEM_NEW:           return gem_new(data);
	case DRM_MSM_GEM_INFO:          return gem_info(data);
	case DRM_MSM_GEM_SUBMIT:        return gem_submit(data);
	case DRM_MSM_GEM_MADVISE:
		((struct drm_msm_gem_madvise *)data)->retained = 1;
		return 0;
	case DRM_MSM_SUBMITQUEUE_NEW:
		((struct drm_msm_submitqueue *)data)->id = 1;
		return 0;
	case DRM_MSM_SUBMITQUEUE_CLOSE: return 0;
	case DRM_MSM_SUBMITQUEUE_QUERY: {
		struct drm_msm_submitqueue_query *q = data;
		if (q->param != MSM_SUBMITQUEUE_PARAM_FAULTS || q->len < sizeof(uint64_t)) {
			return -EINVAL;
		}
		struct NativeAdrenoAbiStatus st = { 0 };
		int r = call(NativeAdrenoMethodGetStatus, NULL, 0, &st, sizeof(st));
		*(uint64_t *)(uintptr_t)q->data = st.faults;
		q->len = sizeof(uint64_t);
		return r;
	}
	default:
		printf("sweet_native_drm: unhandled msm request %lu\n", idx);
		return -EINVAL;
	}
}

EXPORT int drmCommandWriteRead(int fd, unsigned long idx, void *data, unsigned long size)
{
	(void)fd; (void)size;
	/* These two wait: they must not hold the lock against the other threads' submits. */
	if (idx == DRM_MSM_WAIT_FENCE) {
		struct drm_msm_wait_fence *w = data;
		return wait_fence(w->fence, rel_ns(&w->timeout));
	}
	if (idx == DRM_MSM_GEM_CPU_PREP) {
		struct drm_msm_gem_cpu_prep *p = data;
		pthread_mutex_lock(&g_lock);
		uint32_t f = p->handle && p->handle <= MAX_BOS ? g_bo[p->handle].fence : 0;
		pthread_mutex_unlock(&g_lock);
		int r = wait_fence(f, p->op & MSM_PREP_NOSYNC ? 0 : rel_ns(&p->timeout));
		return r == -ETIMEDOUT && (p->op & MSM_PREP_NOSYNC) ? -EBUSY : r;
	}
	pthread_mutex_lock(&g_lock);
	int r = connect_once();
	if (r == 0) {
		r = command(idx, data);
	}
	pthread_mutex_unlock(&g_lock);
	return r;
}

EXPORT int drmCommandWrite(int fd, unsigned long idx, void *data, unsigned long size)
{
	return drmCommandWriteRead(fd, idx, data, size);
}

EXPORT int drmCommandRead(int fd, unsigned long idx, void *data, unsigned long size)
{
	return drmCommandWriteRead(fd, idx, data, size);
}

EXPORT int drmIoctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	if (request == DRM_IOCTL_GEM_CLOSE) {
		pthread_mutex_lock(&g_lock);
		int r = gem_close(((struct drm_gem_close *)arg)->handle);
		pthread_mutex_unlock(&g_lock);
		if (r) {
			errno = -r;
			return -1;
		}
		return 0;
	}
	printf("sweet_native_drm: unhandled ioctl 0x%lx\n", request);
	errno = ENOSYS;
	return -1;
}

EXPORT void *sweet_drm_mmap(int fd, size_t len, uint64_t offset)
{
	(void)fd;
	uint32_t handle = (uint32_t)offset;
	pthread_mutex_lock(&g_lock);
	if (handle != 0 && handle <= MAX_BOS && g_bo[handle].surface != NULL) {
		void *base = (void *)(uintptr_t)g_bo[handle].map;       /* the kernel has no CPU map for imports */
		pthread_mutex_unlock(&g_lock);
		return base;
	}
	struct NativeAdrenoAbiBoCpuMap m = { handle };
	struct NativeAdrenoAbiBoCpuMapReply reply = { 0 };
	mach_vm_address_t addr = 0;
	mach_vm_size_t size = 0;
	void *out = NULL;
	int r = call(NativeAdrenoMethodBoCpuMap, &m, sizeof(m), &reply, sizeof(reply));
	if (r == 0 && IOConnectMapMemory64(g_conn, reply.token, mach_task_self(), &addr, &size,
	    kIOMapAnywhere | kIOMapWriteCombineCache) == KERN_SUCCESS && size >= len) {
		g_bo[handle].map = addr;
		out = (void *)(uintptr_t)addr;
	} else {
		printf("sweet_native_drm: CPU map of BO %u (%zu bytes) failed\n", handle, len);
	}
	pthread_mutex_unlock(&g_lock);
	return out;
}

EXPORT int sweet_drm_munmap(void *addr, size_t len)
{
	(void)len;
	uint64_t a = (uint64_t)(uintptr_t)addr;
	int r = -EINVAL;
	pthread_mutex_lock(&g_lock);
	for (uint32_t h = 1; h <= g_top_handle && r != 0; h++) {
		if (g_bo[h].map == a) {
			if (g_bo[h].surface == NULL) {
				IOConnectUnmapMemory64(g_conn, h, mach_task_self(), a);
				g_bo[h].map = 0;
			}
			r = 0;
		}
	}
	pthread_mutex_unlock(&g_lock);
	return r;
}

/* air_metal.c's struct rwa_sbo: surface_id in, the rest out. */
struct rwa_sbo {
	uint32_t surface_id, handle;
	uint64_t size, iova;
	uint32_t offset, bytes_per_row, width, height, pixel_format, planes;
};

/*
 * An IOSurface as a BO: NativeAdreno pins the surface's pages at this process's own mapping of it (BoImport). The GPU maps
 * them non-cacheable and the CPU keeps its cached view, so sweet_drm_bo_sync cleans and invalidates around CPU use.
 */
EXPORT int sweet_drm_surface_bo(int fd, struct rwa_sbo *io)
{
	(void)fd;
	IOSurfaceRef surface = IOSurfaceLookup(io->surface_id);
	if (surface == NULL) {
		return -ENOENT;
	}
	uintptr_t base = (uintptr_t)IOSurfaceGetBaseAddress(surface);
	uint64_t size = (IOSurfaceGetAllocSize(surface) + 4095) & ~4095ull;
	pthread_mutex_lock(&g_lock);
	int r = connect_once();
	struct NativeAdrenoAbiBoImport in = { .address = base, .bytes = size };
	struct NativeAdrenoAbiBoAllocReply reply = { 0 };
	if (r == 0) {
		r = base != 0 && (base & 4095) == 0 && size != 0 ? 0 : -EINVAL;
	}
	if (r == 0) {
		r = call(NativeAdrenoMethodBoImport, &in, sizeof(in), &reply, sizeof(reply));
	}
	if (r == 0 && (reply.handle == 0 || reply.handle > MAX_BOS)) {
		r = -ENOSPC;
	}
	uint64_t iova = 0;
	if (r == 0) {
		iova = va_alloc(size);
		struct NativeAdrenoAbiBoMap map = { .handle = reply.handle, .iova = iova, .permissions = 3, .writeback = 0,
			.shareability = 2 };
		struct NativeAdrenoAbiBoMapReply mapped = { 0 };
		r = iova ? call(NativeAdrenoMethodBoMap, &map, sizeof(map), &mapped, sizeof(mapped)) : -ENOSPC;
		if (r) {
			struct NativeAdrenoAbiBoFree f = { reply.handle };
			call(NativeAdrenoMethodBoFree, &f, sizeof(f), NULL, 0);
			if (iova) {
				va_free(iova, size);
			}
		}
	}
	if (r == 0) {
		g_bo[reply.handle] = (struct bo){ .iova = iova, .size = size, .map = base, .surface = surface };
		if (reply.handle > g_top_handle) {
			g_top_handle = (uint32_t)reply.handle;
		}
		io->handle = (uint32_t)reply.handle;
		io->size = size;
		io->iova = iova;
		io->offset = 0;
		io->bytes_per_row = (uint32_t)IOSurfaceGetBytesPerRow(surface);
		io->width = (uint32_t)IOSurfaceGetWidth(surface);
		io->height = (uint32_t)IOSurfaceGetHeight(surface);
		io->pixel_format = IOSurfaceGetPixelFormat(surface);
		io->planes = (uint32_t)IOSurfaceGetPlaneCount(surface);
	} else {
		CFRelease(surface);
		printf("sweet_native_drm: IOSurface %u import failed: %d\n", io->surface_id, r);
	}
	pthread_mutex_unlock(&g_lock);
	return r;
}

EXPORT int sweet_drm_bo_sync(int fd, uint32_t h, uint64_t off, uint64_t size)
{
	(void)fd;
	if (h == 0 || h > MAX_BOS) {
		return -EINVAL;
	}
	pthread_mutex_lock(&g_lock);
	uintptr_t base = g_bo[h].surface != NULL ? (uintptr_t)g_bo[h].map : 0;
	uint64_t total = g_bo[h].size;
	pthread_mutex_unlock(&g_lock);
	if (base == 0 || off >= total) {
		return 0;                       /* our own BOs are write-combining: nothing to clean */
	}
	uint64_t n = size == 0 || size > total - off ? total - off : size;
	for (uintptr_t a = (base + off) & ~(uintptr_t)63; a < base + off + n; a += 64) {
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

/* bgmtl.m's struct bg_gpu_info: only flags & 3 is read (bit0 ready, bit1 lost). */
struct bg_gpu_info { uint32_t abi_version, flags, gpu_id, chip_id; uint64_t va_start, va_size; uint32_t last_fence, abi_minor; };

EXPORT int sweet_drm_info(int fd, struct bg_gpu_info *out)
{
	(void)fd;
	pthread_mutex_lock(&g_lock);
	int r = connect_once();
	if (r == 0) {
		out->abi_version = NativeAdrenoAbiVersion;
		out->flags = 1;
		out->gpu_id = 630;
		out->chip_id = g_info.chip_id;
		out->va_start = g_info.va_start;
		out->va_size = g_info.va_size;
		out->last_fence = g_last_fence;
		out->abi_minor = 2;
	}
	pthread_mutex_unlock(&g_lock);
	return r;
}

EXPORT int drmOpenWithType(const char *name, const char *busid, int type)
{
	(void)busid; (void)type;
	if (name == NULL || strcmp(name, "msm") != 0) {
		return -1;
	}
	pthread_mutex_lock(&g_lock);
	int r = connect_once();
	pthread_mutex_unlock(&g_lock);
	return r ? -1 : open("/dev/null", O_RDWR | O_CLOEXEC);
}

EXPORT int drmClose(int fd) { return close(fd); }

EXPORT drmVersionPtr drmGetVersion(int fd)
{
	(void)fd;
	drmVersionPtr v = calloc(1, sizeof(*v));
	if (v != NULL) {
		v->version_major = 1;
		v->version_minor = 9;
		v->name = strdup("msm");
		v->name_len = 3;
		v->date = strdup("0");
		v->date_len = 1;
		v->desc = strdup("SweetGPU");
		v->desc_len = 8;
	}
	return v;
}

EXPORT void drmFreeVersion(drmVersionPtr v)
{
	if (v != NULL) {
		free(v->name);
		free(v->date);
		free(v->desc);
		free(v);
	}
}

EXPORT int drmGetCap(int fd, uint64_t cap, uint64_t *value) { (void)fd; (void)cap; *value = 0; return 0; }

/* Linked by Mesa's generic code, never reached on this path: one GPU, no PRIME, no syncobjs, no device list. */
EXPORT int drmPrimeHandleToFD(int fd, uint32_t h, uint32_t f, int *p) { (void)fd; (void)h; (void)f; (void)p; return -ENOSYS; }
EXPORT int drmPrimeFDToHandle(int fd, int p, uint32_t *h) { (void)fd; (void)p; (void)h; return -ENOSYS; }
EXPORT int drmGetDevices2(uint32_t f, drmDevicePtr d[], int n) { (void)f; (void)d; (void)n; return 0; }
EXPORT int drmGetDevice2(int fd, uint32_t f, drmDevicePtr *d) { (void)fd; (void)f; (void)d; return -ENODEV; }
EXPORT void drmFreeDevice(drmDevicePtr *d) { (void)d; }
EXPORT void drmFreeDevices(drmDevicePtr d[], int n) { (void)d; (void)n; }
EXPORT char *drmGetDeviceNameFromFd2(int fd) { (void)fd; return NULL; }
EXPORT int drmGetDeviceFromDevId(dev_t id, uint32_t f, drmDevicePtr *d) { (void)id; (void)f; (void)d; return -ENODEV; }
EXPORT int drmGetMagic(int fd, drm_magic_t *m) { (void)fd; (void)m; return -ENOSYS; }
EXPORT int drmDestroyContext(int fd, drm_context_t c) { (void)fd; (void)c; return -ENOSYS; }
EXPORT int drmSyncobjCreate(int fd, uint32_t f, uint32_t *h) { (void)fd; (void)f; (void)h; return -ENOSYS; }
EXPORT int drmSyncobjDestroy(int fd, uint32_t h) { (void)fd; (void)h; return -ENOSYS; }
EXPORT int drmSyncobjReset(int fd, const uint32_t *h, uint32_t n) { (void)fd; (void)h; (void)n; return -ENOSYS; }
EXPORT int drmSyncobjFDToHandle(int fd, int o, uint32_t *h) { (void)fd; (void)o; (void)h; return -ENOSYS; }
EXPORT int drmSyncobjImportSyncFile(int fd, uint32_t h, int s) { (void)fd; (void)h; (void)s; return -ENOSYS; }
EXPORT int drmSyncobjExportSyncFile(int fd, uint32_t h, int *s) { (void)fd; (void)h; (void)s; return -ENOSYS; }
