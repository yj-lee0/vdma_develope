// SPDX-License-Identifier: MIT
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L	/* O_CLOEXEC, pthread_atfork, etc. */
#endif
/*
 * libvdma_font - user-space API (process-singleton model, SYNC only)
 *
 * 한 process 안에서 디바이스 fd 를 단 1개만 유지한다.
 * vdma_font_open() 은 refcount 증가, 마지막 close() 에서만 fd 가 닫힘.
 * 두 번째 이후의 open() 에 다른 path 가 들어오면 -EBUSY 반환.
 *
 * 본 라이브러리는 SYNC submit 만 지원합니다. ASYNC / WAIT / DRAIN 은 추후
 * 도입 예정 (ref_md/ref_submit-v2.md 참고).
 *
 * Defensive layer:
 *   - PID + fork-generation guard: fork 자식이 open() 다시 부르기 전엔
 *     모든 API 가 -EOWNERDEAD
 *   - Invalidated flag: 치명적 ioctl 실패 시 후속 호출 차단
 *   - Buffer magic + refcount: use-after-free / double-free 감지
 */

#include "libvdma_font.h"

/* UAPI 헤더가 in-kernel 타입 (u32/u64/s32) 으로 작성되어 있어 userspace
 * 컴파일을 위해 shim. 추후 UAPI 가 __u32/__u64 로 정리되면 제거. */
#include <linux/types.h>
typedef __u32 u32;
typedef __u64 u64;
typedef __s32 s32;
typedef __u16 u16;
typedef __u8  u8;
typedef __s32 s32;

#include "en683-font-uapi.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <assert.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>

#define DEFAULT_DEV_PATH	"/dev/enx_vdma_font"
#define VDMA_BUF_MAGIC		0x56444D41u	/* "VDMA" */
#define VDMA_BUF_POISON		0xDEADBEEFu
#define INST_PATH_MAX		64
#define VDMA_FONT_MAX_SRC	64		/* lib-side cap; kernel enforces via max_src */

/* -------------------------------------------------------------------------- */
/* Internal types                                                             */
/* -------------------------------------------------------------------------- */

struct vdma_buf {
	uint32_t		 magic;		/* VDMA_BUF_MAGIC, poisoned on free */
	_Atomic int		 refcount;	/* user/transient holds */
	struct vdma_buf		*next;		/* g_inst.bufs list */
	uint32_t		 id;
	uint32_t		 kind;
	size_t			 size;
	void			*cpu;
};
typedef struct vdma_buf vdma_buf_t;

struct vdma_inst {
	int			 fd;
	char			 path[INST_PATH_MAX];
	pid_t			 creator_pid;
	uint64_t		 fork_gen_at_open;

	_Atomic int		 init_count;	/* +1 per vdma_font_open() */
	_Atomic int		 invalidated;

	pthread_mutex_t		 lock;		/* protects bufs */
	vdma_buf_t		*bufs;
};

/* -------------------------------------------------------------------------- */
/* Singleton state                                                            */
/* -------------------------------------------------------------------------- */

static struct vdma_inst	 g_inst = {
	.fd		= -1,
	.lock		= PTHREAD_MUTEX_INITIALIZER,
};
static pthread_mutex_t	 g_open_lock = PTHREAD_MUTEX_INITIALIZER;

static _Atomic uint64_t	 g_fork_generation;
static pthread_once_t	 g_fork_once = PTHREAD_ONCE_INIT;

static void atfork_child(void)
{
	/* Child inherits parent's fd table but lib-side locks/lists are stale.
	 * Mark all state invalid; child must call vdma_font_open() to recover. */
	pthread_mutex_init(&g_open_lock, NULL);
	pthread_mutex_init(&g_inst.lock, NULL);
	g_inst.fd = -1;
	g_inst.bufs = NULL;
	atomic_store(&g_inst.init_count, 0);
	atomic_store(&g_inst.invalidated, 0);
	atomic_fetch_add_explicit(&g_fork_generation, 1, memory_order_release);
}

static void register_atfork(void)
{
	pthread_atfork(NULL, NULL, atfork_child);
}

static const char *path_or_default(const char *p)
{
	return (p && *p) ? p : DEFAULT_DEV_PATH;
}

/* -------------------------------------------------------------------------- */
/* Instance validity                                                          */
/* -------------------------------------------------------------------------- */

static int inst_check(void)
{
	if (g_inst.fd < 0)
		return -ENODEV;
	if (atomic_load_explicit(&g_inst.invalidated, memory_order_acquire))
		return -EBADF;
	if (g_inst.creator_pid != getpid())
		return -EOWNERDEAD;
	if (g_inst.fork_gen_at_open !=
	    atomic_load_explicit(&g_fork_generation, memory_order_acquire))
		return -EOWNERDEAD;
	return 0;
}

static int inst_ioctl(unsigned long cmd, void *p)
{
	int r = ioctl(g_inst.fd, cmd, p);
	if (r < 0) {
		int e = errno;
		if (e == ENODEV || e == EIO || e == ESHUTDOWN) {
			atomic_store_explicit(&g_inst.invalidated, 1,
					      memory_order_release);
		}
		errno = e;
	}
	return r;
}

/* -------------------------------------------------------------------------- */
/* Buffer list                                                                */
/* -------------------------------------------------------------------------- */

static void buf_link(vdma_buf_t *buf)
{
	pthread_mutex_lock(&g_inst.lock);
	buf->next = g_inst.bufs;
	g_inst.bufs = buf;
	pthread_mutex_unlock(&g_inst.lock);
}

static int buf_unlink(vdma_buf_t *buf)
{
	int found = 0;
	pthread_mutex_lock(&g_inst.lock);
	for (vdma_buf_t **pp = &g_inst.bufs; *pp; pp = &(*pp)->next) {
		if (*pp == buf) {
			*pp = buf->next;
			buf->next = NULL;
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&g_inst.lock);
	return found;
}

static void buf_destroy(vdma_buf_t *buf)
{
	struct vdma_free_args f = { .id = buf->id };

	if (buf->cpu)
		munmap(buf->cpu, buf->size);
	if (!atomic_load_explicit(&g_inst.invalidated, memory_order_acquire) &&
	    g_inst.fd >= 0)
		(void)ioctl(g_inst.fd, VDMAIOSET_FREE, &f);
	buf->magic = VDMA_BUF_POISON;
	free(buf);
}

static void buf_put(vdma_buf_t *buf)
{
	if (atomic_fetch_sub_explicit(&buf->refcount, 1,
				      memory_order_acq_rel) != 1)
		return;
	buf_destroy(buf);
}

/* Shutdown's force-destroy: claim via CAS(1→0). */
static void buf_force_destroy(vdma_buf_t *b)
{
	int expected = 1;
	while (!atomic_compare_exchange_strong_explicit(
			&b->refcount, &expected, 0,
			memory_order_acq_rel, memory_order_acquire)) {
		sched_yield();
		expected = 1;
	}
	if (b->cpu) munmap(b->cpu, b->size);
	b->magic = VDMA_BUF_POISON;
	free(b);
}

/*
 * vir_addr → buf 역방향 lookup. g_inst.lock 보유 상태에서 transient ref 까지
 * 같이 잡음 (force_destroy 와의 race 방지). 호출자는 사용 후 반드시 buf_put().
 * size_hint != 0 이면 buf->size 와 일치해야 매칭.
 */
static vdma_buf_t *find_and_acquire(vdma_addr_t addr, size_t size_hint)
{
	vdma_buf_t *found = NULL;

	if (!addr) return NULL;

	pthread_mutex_lock(&g_inst.lock);
	for (vdma_buf_t *b = g_inst.bufs; b; b = b->next) {
		if (b->cpu != addr) continue;
		if (size_hint && b->size != size_hint) continue;
		if (atomic_load_explicit(&b->refcount,
					 memory_order_acquire) > 0) {
			atomic_fetch_add_explicit(&b->refcount, 1,
						  memory_order_acq_rel);
			found = b;
		}
		break;
	}
	pthread_mutex_unlock(&g_inst.lock);
	return found;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* -------------------------------------------------------------------------- */

int vdma_font_open(const char *path)
{
	pthread_once(&g_fork_once, register_atfork);
	path = path_or_default(path);

	pthread_mutex_lock(&g_open_lock);

	int prev = atomic_load_explicit(&g_inst.init_count,
					memory_order_acquire);
	if (prev > 0) {
		/* Already open — second+ caller must match path. */
		if (strncmp(g_inst.path, path, INST_PATH_MAX) != 0) {
			pthread_mutex_unlock(&g_open_lock);
			return -EBUSY;
		}
		atomic_fetch_add_explicit(&g_inst.init_count, 1,
					  memory_order_acq_rel);
		pthread_mutex_unlock(&g_open_lock);
		return 0;
	}

	/* First open — actually open the fd. */
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		int saved = errno;
		pthread_mutex_unlock(&g_open_lock);
		return -saved;
	}

	g_inst.fd		= fd;
	strncpy(g_inst.path, path, INST_PATH_MAX - 1);
	g_inst.path[INST_PATH_MAX - 1] = '\0';
	g_inst.creator_pid	= getpid();
	g_inst.fork_gen_at_open	= atomic_load_explicit(&g_fork_generation,
						       memory_order_acquire);
	atomic_store(&g_inst.invalidated, 0);
	atomic_store(&g_inst.init_count, 1);

	/* Enter KERNEL mode (mandatory before any submit). VIDEO_CORE mode
	 * is not supported by this library — issue MOD_INIT(KERNEL). */
	{
		u32 mode = ENX_VDMA_MODE_KERNEL;
		if (ioctl(g_inst.fd, VDMAIOSET_MOD_INIT, &mode) < 0) {
			int saved = errno;
			close(g_inst.fd);
			g_inst.fd = -1;
			atomic_store(&g_inst.init_count, 0);
			pthread_mutex_unlock(&g_open_lock);
			return -saved;
		}
	}

	pthread_mutex_unlock(&g_open_lock);
	return 0;
}

/* Force-tear-down all resources. Called when last init_count is dropped. */
static void inst_force_destroy(void)
{
	atomic_store_explicit(&g_inst.invalidated, 1, memory_order_release);

	int fd = g_inst.fd;
	g_inst.fd = -1;
	if (fd >= 0) close(fd);

	pthread_mutex_lock(&g_inst.lock);
	vdma_buf_t *list = g_inst.bufs;
	g_inst.bufs = NULL;
	pthread_mutex_unlock(&g_inst.lock);

	while (list) {
		vdma_buf_t *next = list->next;
		list->next = NULL;
		buf_force_destroy(list);
		list = next;
	}
}

void vdma_font_close(const char *path)
{
	(void)path;	/* singleton — path 무시 (open 시점에 고정) */

	pthread_mutex_lock(&g_open_lock);
	int prev = atomic_fetch_sub_explicit(&g_inst.init_count, 1,
					     memory_order_acq_rel);
	assert(prev > 0 && "vdma_font_close over-called");
	int last = (prev == 1);
	pthread_mutex_unlock(&g_open_lock);

	if (last)
		inst_force_destroy();
}

/* -------------------------------------------------------------------------- */
/* Buffer alloc / free                                                        */
/* -------------------------------------------------------------------------- */

static vdma_buf_t *do_alloc(uint32_t kind, size_t size)
{
	struct vdma_alloc_args a;
	vdma_buf_t *buf;
	void *p;

	if (size == 0 || size > UINT32_MAX) { errno = EINVAL; return NULL; }

	if (inst_check()) { errno = ENODEV; return NULL; }

	memset(&a, 0, sizeof(a));
	a.kind  = kind;
	a.size  = (uint32_t)size;

	if (inst_ioctl(VDMAIOSET_ALLOC, &a) < 0)
		return NULL;

	p = mmap(NULL, size, PROT_READ | PROT_WRITE,
		 MAP_SHARED, g_inst.fd, (off_t)a.mmap_offset);
	if (p == MAP_FAILED) {
		int saved = errno;
		struct vdma_free_args f = { .id = a.id };
		(void)ioctl(g_inst.fd, VDMAIOSET_FREE, &f);
		errno = saved;
		return NULL;
	}

	buf = calloc(1, sizeof(*buf));
	if (!buf) {
		int saved = errno;
		struct vdma_free_args f = { .id = a.id };
		munmap(p, size);
		(void)ioctl(g_inst.fd, VDMAIOSET_FREE, &f);
		errno = saved;
		return NULL;
	}
	buf->magic = VDMA_BUF_MAGIC;
	atomic_store(&buf->refcount, 1);
	buf->id    = a.id;
	buf->kind  = kind;
	buf->size  = size;
	buf->cpu   = p;

	buf_link(buf);
	return buf;
}

vdma_addr_t vdma_font_alloc_src(size_t size)
{
	vdma_buf_t *b = do_alloc(ENX_BUF_SRC, size);
	return b ? b->cpu : NULL;
}

vdma_addr_t vdma_font_alloc_dst(size_t size)
{
	vdma_buf_t *b = do_alloc(ENX_BUF_DST, size);
	return b ? b->cpu : NULL;
}

int vdma_font_free(vdma_addr_t addr, size_t size)
{
	vdma_buf_t *buf = find_and_acquire(addr, size);
	if (!buf) return -EINVAL;

	if (buf_unlink(buf))
		buf_put(buf);		/* user ref drop */
	buf_put(buf);			/* transient ref drop */
	return 0;
}

/* -------------------------------------------------------------------------- */
/* Submit (SYNC only)                                                         */
/* -------------------------------------------------------------------------- */

/* YCbCr packed (Y<<16 | Cb<<8 | Cr) → per-byte fields. */
static inline uint8_t y_of (uint32_t c) { return (c >> 16) & 0xff; }
static inline uint8_t cb_of(uint32_t c) { return (c >>  8) & 0xff; }
static inline uint8_t cr_of(uint32_t c) { return (c      ) & 0xff; }

/*
 * Build the font-specific submit struct (flex array, sized for nblit).
 * Translates lib-level &vdma_blit into UAPI &enx_en683_font_blit.
 *
 * NOTE: dst id is conveyed via blits[0].dst_addr_y0 because the UAPI does
 * not have a dedicated dst_id field; this matches the kernel's expectation
 * in font_ioctl_submit (dst_kind == ENX_BUF_ID → id = blits[0].dst_addr_y0).
 */
static struct enx_en683_font_submit_args *
make_submit_args(const struct vdma_submit_req *req,
		 vdma_buf_t *dst,
		 vdma_buf_t * const *src_bufs)
{
	size_t bytes = sizeof(struct enx_en683_font_submit_args)
		     + req->nblit * sizeof(struct enx_en683_font_blit);
	struct enx_en683_font_submit_args *s = calloc(1, bytes);
	if (!s) return NULL;

	/* dst_type_index = 0 : kernel-mode + offset(id) semantics */
	s->dst_type_index      = 0;
	s->dst_width           = (uint16_t)req->dst_width;
	s->dst_height          = (uint16_t)req->dst_height;
	s->background_overlay  = req->background_overlay;
	s->font_number         = (uint32_t)req->nblit;

	for (size_t i = 0; i < req->nblit; i++) {
		const struct vdma_blit *b = &req->blits[i];
		struct enx_en683_font_blit *out = &s->blits[i];

		out->src_addr_flag       = 0;	/* offset(id) */
		out->src_addr            = src_bufs[i]->id;
		out->src_width           = (uint16_t)b->width;
		out->src_height          = (uint16_t)b->height;
		out->font_xpos           = (uint16_t)b->dst_x;
		out->font_ypos           = (uint16_t)b->dst_y;

		/* dst id stored in y0 of every blit (kernel uses blits[0]) */
		out->dst_addr_y0         = dst->id;

		out->font_color_y        = y_of (b->font_color);
		out->font_color_cb       = cb_of(b->font_color);
		out->font_color_cr       = cr_of(b->font_color);
		out->font_value          = (uint8_t)b->font_value;

		out->outline_color_y     = y_of (b->outline_color);
		out->outline_color_cb    = cb_of(b->outline_color);
		out->outline_color_cr    = cr_of(b->outline_color);
		out->outline_value       = (uint8_t)b->outline_value;

		out->bg_color_y          = y_of (b->bg_color);
		out->bg_color_cb         = cb_of(b->bg_color);
		out->bg_color_cr         = cr_of(b->bg_color);

		out->font_wr_mode        = (uint8_t)b->wr_mode;
		out->font_color_tone     = (uint8_t)b->color_tone;
		out->font_alpha          = (uint16_t)b->alpha;
		out->font_threshold      = (uint8_t)b->threshold;
	}

	return s;
}

static int resolve_submit(const struct vdma_submit_req *req,
			  vdma_buf_t **dst_out,
			  vdma_buf_t **src_bufs,
			  size_t *acquired_out)
{
	vdma_buf_t *dst;

	*acquired_out = 0;
	*dst_out      = NULL;

	if (!req || !req->dst || !req->blits ||
	    req->nblit == 0 || req->nblit > VDMA_FONT_MAX_SRC)
		return -EINVAL;
	if (!req->dst_width || !req->dst_height)
		return -EINVAL;

	dst = find_and_acquire(req->dst, 0);
	if (!dst) return -EINVAL;
	if (dst->kind != ENX_BUF_DST) { buf_put(dst); return -EINVAL; }
	*dst_out = dst;

	for (size_t i = 0; i < req->nblit; i++) {
		vdma_buf_t *s = find_and_acquire(req->blits[i].src, 0);
		if (!s) return -EINVAL;
		if (s->kind != ENX_BUF_SRC) {
			buf_put(s);
			return -EINVAL;
		}
		src_bufs[i] = s;
		(*acquired_out)++;
	}
	return 0;
}

static void unwind_submit(vdma_buf_t *dst, vdma_buf_t **src_bufs, size_t acquired)
{
	for (size_t i = 0; i < acquired; i++) buf_put(src_bufs[i]);
	if (dst) buf_put(dst);
}

int vdma_font_submit(const struct vdma_submit_req *req)
{
	vdma_buf_t *dst = NULL;
	vdma_buf_t *src_bufs[VDMA_FONT_MAX_SRC];
	struct enx_en683_font_submit_args *s;
	size_t acquired = 0;
	int r;

	if (inst_check()) return -ENODEV;

	r = resolve_submit(req, &dst, src_bufs, &acquired);
	if (r) { unwind_submit(dst, src_bufs, acquired); return r; }

	s = make_submit_args(req, dst, src_bufs);
	if (!s) { unwind_submit(dst, src_bufs, acquired); return -ENOMEM; }

	r = (inst_ioctl(VDMAIOSET_FONT_SUBMIT, s) < 0) ? -errno : 0;
	free(s);

	unwind_submit(dst, src_bufs, acquired);
	return r;
}

/* -------------------------------------------------------------------------- */
/* Cross-fd buffer sharing (EXPORT / IMPORT)                                  */
/* -------------------------------------------------------------------------- */

int vdma_font_export(vdma_addr_t addr, int *export_fd_out)
{
	struct vdma_export_args e;
	vdma_buf_t *buf;
	int r;

	if (!export_fd_out)
		return -EINVAL;
	if (inst_check())
		return -ENODEV;

	buf = find_and_acquire(addr, 0);
	if (!buf) return -EINVAL;

	memset(&e, 0, sizeof(e));
	e.id = buf->id;
	if (inst_ioctl(VDMAIOSET_EXPORT, &e) < 0) {
		r = -errno;
		goto out;
	}
	*export_fd_out = e.export_fd;
	r = 0;
out:
	buf_put(buf);
	return r;
}

vdma_addr_t vdma_font_import(int export_fd,
			     size_t *size_out, uint32_t *kind_out)
{
	struct vdma_import_args im;
	vdma_buf_t *buf;
	void *p;

	if (export_fd < 0) { errno = EBADF; return NULL; }
	if (inst_check())  { errno = ENODEV; return NULL; }

	memset(&im, 0, sizeof(im));
	im.export_fd = export_fd;
	if (inst_ioctl(VDMAIOGET_IMPORT, &im) < 0)
		return NULL;

	p = mmap(NULL, im.size, PROT_READ | PROT_WRITE,
		 MAP_SHARED, g_inst.fd, (off_t)im.mmap_offset);
	if (p == MAP_FAILED) {
		int saved = errno;
		struct vdma_free_args f = { .id = im.id };
		(void)ioctl(g_inst.fd, VDMAIOSET_FREE, &f);
		errno = saved;
		return NULL;
	}

	buf = calloc(1, sizeof(*buf));
	if (!buf) {
		int saved = errno;
		struct vdma_free_args f = { .id = im.id };
		munmap(p, im.size);
		(void)ioctl(g_inst.fd, VDMAIOSET_FREE, &f);
		errno = saved;
		return NULL;
	}
	buf->magic = VDMA_BUF_MAGIC;
	atomic_store(&buf->refcount, 1);
	buf->id    = im.id;
	buf->kind  = im.kind;
	buf->size  = im.size;
	buf->cpu   = p;

	buf_link(buf);

	if (size_out) *size_out = im.size;
	if (kind_out) *kind_out = im.kind;
	return p;
}
