// SPDX-License-Identifier: MIT
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "libvdma_jpegdec.h"

#include <linux/types.h>
typedef __u32 u32;
typedef __u64 u64;
typedef __s32 s32;
typedef __u16 u16;
typedef __u8  u8;

#include "en683-jpegdec-uapi.h"

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

#define DEFAULT_DEV_PATH	"/dev/enx_vdma_jdec"
#define VDMA_BUF_MAGIC		0x56444D41u
#define VDMA_BUF_POISON		0xDEADBEEFu
#define INST_PATH_MAX		64

/* -------------------------------------------------------------------------- */
/* Internal types                                                             */
/* -------------------------------------------------------------------------- */

struct vdma_buf {
	uint32_t		 magic;
	_Atomic int		 refcount;
	struct vdma_buf		*next;
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

	_Atomic int		 init_count;
	_Atomic int		 invalidated;

	pthread_mutex_t		 lock;
	vdma_buf_t		*bufs;
};

static struct vdma_inst	 g_inst = {
	.fd		= -1,
	.lock		= PTHREAD_MUTEX_INITIALIZER,
};
static pthread_mutex_t	 g_open_lock = PTHREAD_MUTEX_INITIALIZER;

static _Atomic uint64_t	 g_fork_generation;
static pthread_once_t	 g_fork_once = PTHREAD_ONCE_INIT;

static void atfork_child(void)
{
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

int vdma_jpegdec_open(const char *path)
{
	pthread_once(&g_fork_once, register_atfork);
	path = path_or_default(path);

	pthread_mutex_lock(&g_open_lock);

	int prev = atomic_load_explicit(&g_inst.init_count,
					memory_order_acquire);
	if (prev > 0) {
		if (strncmp(g_inst.path, path, INST_PATH_MAX) != 0) {
			pthread_mutex_unlock(&g_open_lock);
			return -EBUSY;
		}
		atomic_fetch_add_explicit(&g_inst.init_count, 1,
					  memory_order_acq_rel);
		pthread_mutex_unlock(&g_open_lock);
		return 0;
	}

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

	pthread_mutex_unlock(&g_open_lock);
	return 0;
}

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

void vdma_jpegdec_close(const char *path)
{
	(void)path;

	pthread_mutex_lock(&g_open_lock);
	int prev = atomic_fetch_sub_explicit(&g_inst.init_count, 1,
					     memory_order_acq_rel);
	assert(prev > 0 && "vdma_jpegdec_close over-called");
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

vdma_addr_t vdma_jpegdec_alloc_src(size_t size)
{
	vdma_buf_t *b = do_alloc(ENX_BUF_SRC, size);
	return b ? b->cpu : NULL;
}

vdma_addr_t vdma_jpegdec_alloc_dst(size_t size)
{
	vdma_buf_t *b = do_alloc(ENX_BUF_DST, size);
	return b ? b->cpu : NULL;
}

int vdma_jpegdec_free(vdma_addr_t addr, size_t size)
{
	vdma_buf_t *buf = find_and_acquire(addr, size);
	if (!buf) return -EINVAL;

	if (buf_unlink(buf))
		buf_put(buf);
	buf_put(buf);
	return 0;
}

/* -------------------------------------------------------------------------- */
/* Capacity helper                                                            */
/* -------------------------------------------------------------------------- */

size_t vdma_jpegdec_dst_estimate(uint32_t max_w, uint32_t max_h)
{
	/* worst case = YUV444 (3 bytes/pixel). 실제 YUV420 일 수 있지만
	 * dst format 은 디코드 후에야 알 수 있으므로 보수적으로 잡음. */
	return (size_t)max_w * max_h * 3 + 4096;
}

/* -------------------------------------------------------------------------- */
/* Submit (SYNC only)                                                         */
/* -------------------------------------------------------------------------- */

static int validate_req(const struct vdma_jpegdec_req *req)
{
	if (!req || !req->src || !req->dst)
		return -EINVAL;
	if (!req->src_jpeg_size)
		return -EINVAL;
	if (!req->dst_buf_size)
		return -EINVAL;
	return 0;
}

static void fill_submit_args(const struct vdma_jpegdec_req *req,
			     vdma_buf_t *src, vdma_buf_t *dst,
			     struct enx_en683_jpegdec_submit_args *s)
{
	memset(s, 0, sizeof(*s));

	s->cfg.src_addr_flag    = 0;
	s->cfg.src_addr         = src->id;
	s->cfg.dst_addr_flag    = 0;
	s->cfg.dst_addr         = dst->id;

	s->cfg.src_jpeg_size    = req->src_jpeg_size;
	s->cfg.dst_buf_size     = req->dst_buf_size;

	s->cfg.src_cache_flush  = req->src_cache_flush;
	s->cfg.dst_cache_flush  = req->dst_cache_flush;
}

int vdma_jpegdec_submit(struct vdma_jpegdec_req *req)
{
	vdma_buf_t *src = NULL, *dst = NULL;
	struct enx_en683_jpegdec_submit_args args;
	int r;

	if (inst_check()) return -ENODEV;

	r = validate_req(req);
	if (r) return r;

	src = find_and_acquire(req->src, 0);
	if (!src) { r = -EINVAL; goto out; }
	if (src->kind != ENX_BUF_SRC) { r = -EINVAL; goto out; }

	dst = find_and_acquire(req->dst, 0);
	if (!dst) { r = -EINVAL; goto out; }
	if (dst->kind != ENX_BUF_DST) { r = -EINVAL; goto out; }

	fill_submit_args(req, src, dst, &args);

	if (inst_ioctl(VDMAIOSET_JPEG_DEC_SUBMIT, &args) < 0) {
		r = -errno;
	} else {
		/* HW response echo */
		req->resp.dst_size         = args.resp.dst_size;
		req->resp.cycle            = args.resp.cycle;
		req->resp.hsize            = args.resp.hsize;
		req->resp.vsize            = args.resp.vsize;
		req->resp.hsize_wr         = args.resp.hsize_wr;
		req->resp.vsize_wr         = args.resp.vsize_wr;
		req->resp.fmt              = args.resp.fmt;
		req->resp.header_exist     = args.resp.header_exist.value;
		req->resp.unsupport_marker = args.resp.unsupport_marker.value;
		req->resp.header_error     = args.resp.header_error.value;
		req->resp.process_error    = args.resp.process_error.value;
		r = 0;
	}

out:
	if (dst) buf_put(dst);
	if (src) buf_put(src);
	return r;
}

/* -------------------------------------------------------------------------- */
/* Cross-fd buffer sharing                                                    */
/* -------------------------------------------------------------------------- */

int vdma_jpegdec_export(vdma_addr_t addr, int *export_fd_out)
{
	struct vdma_export_args e;
	vdma_buf_t *buf;
	int r;

	if (!export_fd_out) return -EINVAL;
	if (inst_check())   return -ENODEV;

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

vdma_addr_t vdma_jpegdec_import(int export_fd, uint32_t kind,
				size_t *size_out)
{
	struct vdma_import_args im;
	vdma_buf_t *buf;
	void *p;

	if (export_fd < 0) { errno = EBADF; return NULL; }
	if (kind != ENX_BUF_SRC && kind != ENX_BUF_DST) {
		errno = EINVAL; return NULL;
	}
	if (inst_check()) { errno = ENODEV; return NULL; }

	memset(&im, 0, sizeof(im));
	im.export_fd = export_fd;
	im.kind      = kind;
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
	return p;
}
