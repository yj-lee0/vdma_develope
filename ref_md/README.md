# EN683 VDMA — Skeleton

EN683 VDMA 패밀리 (`font`, `dz`(scaler), `jpegenc`, `jpegdec`) 를 **여러 프로세스·스레드**
에서 안전하게 공유하기 위한 커널 드라이버 + 유저 라이브러리 골격입니다.

> 본 문서는 **사용자 시점 가이드** 입니다.
> 커널 측 상세는 **[CORE.md](CORE.md)** 참고.
> 라운드별 변경 이력 / trade-off 노트는 **[note.md](note.md)**, 메모리 공유 설계는 **[mem_mgmt.md](mem_mgmt.md)**, 자료구조 다이어그램은 **[driver_structs.md](driver_structs.md)** 참고.

---

## 1. 설계 결론 한 줄

> **사용자는 `vdma_addr_t` (mmap virt addr) 또는 buf id 만 다룸. fd / 내부 buf 객체는
> 라이브러리가 숨김. 커널은 function-agnostic core `.ko` + 각 HW driver `.ko`
> (font / dz / jpegenc / jpegdec) 로 분리되어 lifecycle/buf/sharing 은 한 곳에서,
> HW 시퀀스만 driver 가 작성.**

| 책임 | 위치 | 수단 |
|---|---|---|
| HW 레지스터 보호 | 커널 core | `dev->hw_lock` (mutex) |
| ISR ↔ process ctx 동기화 | 커널 core | `dev->irq_lock` (spinlock) |
| 트랜잭션 원자성 | 커널 core | `enx_vdma_submit()` 한 호출 = 한 트랜잭션 |
| Buffer 소유권 | 커널 core | `buf->owner` + `vs->bufs` per-session 리스트 |
| ID ↔ ptr 매핑 | 커널 core | `struct xarray` (`buf_xa`, `job_xa`) |
| Cross-fd 공유 | 커널 core | `VDMAIOSET_EXPORT/IMPORT` (anon_inode) + per-session dedup |
| 비정상 종료 회수 | 커널 core | `fops->release()` + `kref` |
| Debug / 모니터링 | 커널 core | debugfs (`stats / bufs / jobs / sessions`) |
| **HW 레지스터 시퀀스** | kernel driver | `core_ops->hw_run_once()` 콜백 |
| **Function-specific UAPI** | kernel driver | `struct enx_en683_*_submit_args`, `VDMAIOSET_*_SUBMIT` |
| **Mode arbitration** (font) | kernel driver | `font_dev_state.active_mode` (KERNEL / VIDEO_CORE) |
| 인스턴스 관리 | 유저 lib | per-leaf singleton + refcount + fork-safe |
| virt_addr / id ↔ 내부 buf 매핑 | 유저 lib | `verify_buf_info()` (transient ref) |

---

## 2. 파일 구성

스켈레톤은 **core + 4 HW driver** 구조입니다. core 는 function-agnostic 한
lifecycle/buffer/sharing/debug helper 를 모은 `.ko` 이고, 각 HW driver 는 자기
function 의 HW 시퀀스 + UAPI 만 들고 있는 `.ko` 입니다.

```
skeleton/
├── enx-vdma.h                       ← 커널 내부 헤더 (driver 가 include)
├── enx-vdma-core.c                  ← core .ko 구현
├── vdma_error.h                     ← 커널 측 에러 코드
├── Makefile                         ← core + 4 driver 통합 빌드
│
├── include-uapi/                    ← 공통 UAPI 헤더
│   ├── enx-vdma-uapi.h              (공통 ioctl, struct, 플래그)
│   ├── en683-font-uapi.h            (font submit_args)
│   ├── en683-dz-uapi.h              (scaler submit_args)
│   ├── en683-npu-uapi.h             (npu submit_args)
│   └── en683-jpeg-uapi.h            (jpegenc/jpegdec submit_args)
│
├── font-drv/                        ← Font driver .ko
├── dz-drv/                          ← Scaler (Digital Zoom) driver .ko
├── jpegenc-drv/                     ← JPEG Encoder driver .ko
├── jpegdec-drv/                     ← JPEG Decoder driver .ko
├── npu-drv/                         ← npu driver .ko
│
├── userapp/                         ← 사용자 영역
│   ├── lib/                         ← 통합 라이브러리
│   │   ├── enx_vdma.h / .c          (4 driver 통합 API)
│   │   ├── vdma_error.h             (사용자 측 에러 코드)
│   │   └── Makefile
│   ├── font/                        ← Font 예제 / 데모
│   ├── scaler/                      ← Scaler 예제 / 데모
│   ├── jpegenc/                     ← JPEG ENC 예제 / 데모
│   ├── jpegdec/                     ← JPEG DEC 예제 / 데모
│   └── npu/            	         ← NPU 예제 / 데모
│
└── ref_md/                          ← 문서
    ├── README.md                    (이 파일)
    ├── ARCHITECTURE.md              (전체 구조)
    ├── CORE.md                      (core 상세)
    ├── driver_structs.md            (자료구조 다이어그램)
    ├── mem_mgmt.md                  (메모리 공유 설계)
    ├── note.md                      (라운드별 변경 이력)
    └── ref_submit-v2.md             (UAPI V2 / reserved 필드 설계 메모)
```

| 파일 | 영역 | 역할 |
|---|---|---|
| [enx-vdma.h](../enx-vdma.h) | kernel (내부) | core 자료구조, `enx_vdma_core_ops`, EXPORT 함수 prototype |
| [enx-vdma-core.c](../enx-vdma-core.c) | kernel core | lifecycle / buffer / sharing / submit / debugfs |
| [include-uapi/enx-vdma-uapi.h](../include-uapi/enx-vdma-uapi.h) | UAPI (공용) | 공통 ioctl 번호, struct, 플래그 |
| [include-uapi/en683-*-uapi.h](../include-uapi/) | UAPI (per-driver) | driver 별 submit args struct |
| [font-drv/en683-font.c](../font-drv/en683-font.c) | kernel driver | font HW 시퀀스 + core 콜백 |
| [dz-drv/en683-dz.c](../dz-drv/en683-dz.c) | kernel driver | scaler HW 시퀀스 + core 콜백 |
| [jpegenc-drv/en683-jpegenc.c](../jpegenc-drv/en683-jpegenc.c) | kernel driver | JPEG enc HW 시퀀스 + core 콜백 |
| [jpegdec-drv/en683-jpegdec.c](../jpegdec-drv/en683-jpegdec.c) | kernel driver | JPEG dec HW 시퀀스 + core 콜백 |
| [npu-drv/en683-npu.c](../npu-drv/en683-npu.c) | kernel driver | NPU HW 시퀀스 + core 콜백 |
| [userapp/lib/enx_vdma.c](../userapp/lib/enx_vdma.c) | user lib | 4 driver 통합 API 구현 |
| [userapp/lib/enx_vdma.h](../userapp/lib/enx_vdma.h) | user lib | 공개 API 헤더 |
| [userapp/lib/vdma_error.h](../userapp/lib/vdma_error.h) | user | 사용자 측 에러 코드 |

UAPI 헤더는 **커널 빌드와 유저 빌드 양쪽에서 include** 됩니다.

---

## 3. 동작 모델 한눈에

```
                                ┌── enx_vdma.so (per-leaf singleton)
[ App 메인 스레드 ]──Init()────►│   ├── /dev/enx_vdma_<func> 별 fd
                                │   └── refcount + fork-safe
                                                              ▼
                                                /dev/enx_vdma_* ──► driver ─► HW
                                                              (한 device 당 1 job 직렬화)

vdma_addr_t   carries   buf->pvirt  → submit/free 시 lib 이 virt → buf 역 lookup
              kref + magic + atomic 보호
```

- **`ENX_DMA_<Func>_Init(...)`**: per-leaf singleton 모델. 두 번째 호출은 refcount++.
  Font 의 경우 init_type 으로 KERNEL / VIDEO_CORE 모드 선택.
- **`ENX_DMA_<Func>_Buffer_Alloc(kind, size)`**: ecmm 으로 CMA 확보, XArray 로 id 발급,
  mmap 까지 lib 가 자동 수행. user 에게는 **mmap 가상주소** 반환. `_EX` 변형은
  buf id 도 출력.
- **`ENX_DMA_<Func>_Execute(cfg, ...)`**: cfg 내 `*_addr_flag` 로 ID / PHYS / VIRT
  선택. VIRT 모드면 lib 가 `verify_buf_info()` 로 내부 buf 로 변환 + transient ref
  보유. SYNC.
- **`ENX_DMA_<Func>_Buffer_Free(addr, size)`**: lib 이 virt → 내부 buf 역방향 lookup.
- **`ENX_DMA_<Func>_Exit()`**: 마지막 refcount 떨어질 때 모든 자원 강제 정리.
- **`fops->release(fd close)`**: 커널 측에서 진행 중 job 대기 → 소유 buffer 일괄
  `kref_put`, attached buffer 일괄 detach. 비정상 종료 (SIGKILL/segfault) 에서도
  누수 0.
- **export/import** (수동 ioctl 또는 lib 헬퍼): 다른 fd 에 buf 공유. anon_inode fd
  가 capability 가 되어 SCM_RIGHTS 로 전달. per-session dedup 으로 idempotent.

---

## 4. UAPI 요약

공통 ioctl (모든 driver 공유):

```c
VDMAIOSET_ALLOC        /* CMA buffer 할당, id+mmap_offset 반환 */
VDMAIOSET_FREE         /* 자기 session 의 buf 참조 해제 */
VDMAIOSET_WAIT         /* ASYNC submit 의 완료 회수 */
VDMAIOSET_EXPORT       /* 자기 buf 를 anon_inode fd 로 변환 */
VDMAIOGET_IMPORT       /* anon_inode fd 를 자기 session 의 buf 로 등록 */
VDMAIOGET_MAX_BUFS     /* device 의 max_bufs 조회 */
VDMAIOGET_MAX_SRCS     /* device 의 max_src 조회 (driver-specific 제한) */
VDMAIOSET_MOD_INIT     /* font: KERNEL/VIDEO_CORE 모드 set */
```

driver-specific submit:

```c
VDMAIOSET_FONT_SUBMIT       /* font  : N src + 1 dst */
VDMAIOSET_DZ_SUBMIT         /* dz    : 1 src + 1 dst, scale/crop/flip/fmt */
VDMAIOSET_JPEG_ENC_SUBMIT   /* jpegenc : YUV → JPEG */
VDMAIOSET_JPEG_DEC_SUBMIT   /* jpegdec : JPEG → YUV */
VDMAIOSET_JPEG_DISCARD      /* jpeg : 진행 중 op 폐기 */
VDMAIOSET_NPU_SUBMIT       /* npu  : 1 src + 1 dst */
```

상세는 [CORE.md](CORE.md), [include-uapi/](../include-uapi/) 헤더 참고.

### Buffer 종류 (alloc 시 kind)

| 매크로 | 의미 |
|--------|------|
| `VDMA_KIND_SRC` | source 버퍼 (HW 가 읽음) |
| `VDMA_KIND_DST` | destination 버퍼 (HW 가 씀) |

### Address type (submit 시 `*_addr_flag`)

| 매크로 | addr 값 | 의미 |
|--------|--------|------|
| `ENX_VDMA_ADDR_TYPE_ID` (=0) | `uint32_t buf_id` | lib alloc 의 buf id (Buffer_Alloc_EX 가 반환) |
| `ENX_VDMA_ADDR_TYPE_PHYS` (=1) | `phys_addr_t` | 사용자가 별도 확보한 raw 물리 주소 |
| `ENX_VDMA_ADDR_TYPE_VIRT` (=2) | `(uintptr_t)void*` | mmap 가상주소 (가장 흔한 사용) |

→ `ENX_VDMA_ADDR_TYPE_VIRT` 가 default. lib 가 내부적으로 ID 로 변환해서 kernel 에 전달.

### Init type (Init 시 init_type)

| 매크로 | 의미 |
|--------|------|
| `ENX_VDMA_INIT_TYPE_NONE` | 초기화 안 함 |
| `ENX_VDMA_INIT_TYPE_KERNEL` | 커널이 HW 직접 제어 (일반적) |
| `ENX_VDMA_INIT_TYPE_VIDEO_CORE` | RTOS firmware 가 HW 제어 (font 만) |

---

## 5. 라이브러리 사용 패턴

### 5.1 Scaler — 단순 사용

```c
#include "enx_vdma.h"

ENX_DMA_Scaler_Init(0, ENX_VDMA_INIT_TYPE_KERNEL);   /* ch=0 */

void *src = ENX_DMA_Scaler_Buffer_Alloc(0, VDMA_KIND_SRC, src_size);
void *dst = ENX_DMA_Scaler_Buffer_Alloc(0, VDMA_KIND_DST, dst_size);

memcpy(src, input_image, src_size);

VDMA_DZ_CONFIG_S cfg = {
    .src_addr_flag = ENX_VDMA_ADDR_TYPE_VIRT,
    .src_addr      = (uintptr_t)src,
    .src_format    = VDMA_FMT_YUV422_YUYV,
    .src_width        = 1920, .src_height        = 1080,
    .src_width_total  = 1920, .src_height_total  = 1080,

    .dst_addr_flag = ENX_VDMA_ADDR_TYPE_VIRT,
    .dst_addr      = (uintptr_t)dst,
    .dst_format    = VDMA_FMT_YUV422_YUYV,
    .dst_width        = 1280, .dst_height        = 720,
    .dst_width_total  = 1280,

    .src_cache_flush = 1,
    .dst_cache_flush = 1,
};
ENX_DMA_Scaler_Execute(0, &cfg);              /* SYNC, 완료까지 block */

ENX_DMA_Scaler_Buffer_Free(0, src, src_size);
ENX_DMA_Scaler_Buffer_Free(0, dst, dst_size);
ENX_DMA_Scaler_Exit(0);
```

### 5.2 JPEG Encoder

```c
ENX_DMA_JpegEnc_Init();

void *src = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_SRC, yuv_size);
void *dst = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_DST, dst_max_size);

memcpy(src, yuv_image, yuv_size);

VDMA_JPEG_ENC_CONFIG_S cfg = {
    .src_addr_flag = ENX_VDMA_ADDR_TYPE_VIRT, .src_addr = (uintptr_t)src,
    .dst_addr_flag = ENX_VDMA_ADDR_TYPE_VIRT, .dst_addr = (uintptr_t)dst,
    .src_format      = VDMA_FMT_YUV422_YUYV,
    .src_width_total = 1920, .src_height_total = 1080,
    .src_width       = 1920, .src_height       = 1080,
    .quality         = 80,
    .src_cache_flush = 1, .dst_cache_flush = 1,
};

size_t jpeg_size;
ENX_DMA_JpegEnc_Execute(&cfg, &jpeg_size);    /* dst 의 [0..jpeg_size] 에 JPEG */

write(jpeg_fd, dst, jpeg_size);

ENX_DMA_JpegEnc_Buffer_Free(src, yuv_size);
ENX_DMA_JpegEnc_Buffer_Free(dst, dst_max_size);
ENX_DMA_JpegEnc_Exit();
```

### 5.3 JPEG Decoder

```c
ENX_DMA_JpegDec_Init();

void *src = ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_SRC, jpeg_size);
void *dst = ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_DST, yuv_max_size);

memcpy(src, jpeg_data, jpeg_size);

VDMA_JPEG_DEC_CONFIG_S cfg = {
    .src_addr_flag = ENX_VDMA_ADDR_TYPE_VIRT, .src_addr = (uintptr_t)src,
    .dst_addr_flag = ENX_VDMA_ADDR_TYPE_VIRT, .dst_addr = (uintptr_t)dst,
    .src_jpeg_size = jpeg_size,
    .dst_buf_size  = yuv_max_size,
    .src_cache_flush = 1, .dst_cache_flush = 1,
};

size_t yuv_size;
ENX_DMA_JpegDec_Execute(&cfg, &yuv_size);     /* yuv 데이터 + 디코드된 크기 */

ENX_DMA_JpegDec_Buffer_Free(src, jpeg_size);
ENX_DMA_JpegDec_Buffer_Free(dst, yuv_max_size);
ENX_DMA_JpegDec_Exit();
```

### 5.4 Font (Kernel mode 예시)

```c
ENX_DMA_Font_Init(ENX_VDMA_INIT_TYPE_KERNEL);

/* Config alloc — flexible array of font_info[blit_count] */
VDMA_FONT_CONFIG_S *cfg = ENX_DMA_Font_CfgAlloc(blit_count, /*yc_index=*/0);
cfg->dst_width  = 1280;
cfg->dst_height = 720;
cfg->dst_type_index = ENX_VDMA_ADDR_TYPE_VIRT;
cfg->font_info[0].dst_addr_y = (uintptr_t)dst_buf;
cfg->font_info[0].src_addr_flag = ENX_VDMA_ADDR_TYPE_VIRT;
cfg->font_info[0].src_addr      = (uintptr_t)src_buf;
/* ... fill font_info[i] for i in 1..blit_count-1 ... */

ENX_DMA_Font_Update(cfg);          /* SYNC submit */

ENX_DMA_Font_CfgFree();             /* frees all configs allocated by this instance */
ENX_DMA_Font_Exit();
```

Font 의 `VIDEO_CORE` 모드는 RTOS firmware 가 HW 를 제어하는 모드입니다. 사용
패턴이 다르므로 별도 안내 (전용 데모 `userapp/font/` 참조).

### 5.5 ID 모드 사용 (alloc 시 id 받기)

```c
UInt32 src_id, dst_id;
void *src = ENX_DMA_Scaler_Buffer_Alloc_EX(0, VDMA_KIND_SRC, src_size, &src_id);
void *dst = ENX_DMA_Scaler_Buffer_Alloc_EX(0, VDMA_KIND_DST, dst_size, &dst_id);

VDMA_DZ_CONFIG_S cfg = {
    .src_addr_flag = ENX_VDMA_ADDR_TYPE_ID,
    .src_addr      = src_id,
    .dst_addr_flag = ENX_VDMA_ADDR_TYPE_ID,
    .dst_addr      = dst_id,
    /* ... */
};
ENX_DMA_Scaler_Execute(0, &cfg);
```

→ ID 모드는 lib lookup 한 단계 생략 — 매우 빈번한 submit 시나리오에 유용.

---

## 6. Thread-safety 계약

### 동시 호출 안전 ✅
- 서로 다른 device 의 동시 호출 (Scaler ch=0 / ch=1 / Font / JpegEnc / JpegDec)
- 같은 device 의 alloc / free / submit (서로 다른 buf)
- 같은 buf 에 대한 submit + 다른 스레드의 free
  (lib `find_buf_by_virt()` 가 lookup + transient ref bump 까지 한 atomic 시퀀스)

### 안전하지만 비효율 ⚠️
- 한 device 의 같은 buf 를 두 스레드가 동시에 submit — 직렬화 됨

### 금지 (UB) ❌
- 시그널 핸들러에서 lib API 호출 (async-signal-safe 아님)
- `Exit` 이후 stale `vdma_addr_t` 접근 (munmap 됨)
- Font 의 `VIDEO_CORE` 모드에서 multi-thread alloc/realloc/update (단일 스레드 보장)

### fork() 안전성 ✅
- `atfork_child` 가 자식 측 lib 상태 invalidate
- 자식은 `ENX_DMA_<Func>_Init()` 을 다시 호출해야 사용 가능
- 무효 상태에서 API 호출은 `-ENX_ERR_VDMA_FORK_STALE` 반환

상세는 [enx_vdma.h](../userapp/lib/enx_vdma.h) 상단 contract 참조.

---

## 7. 에러 코드

[vdma_error.h](../userapp/lib/vdma_error.h) 정의. 두 가지 범위:

| 범위 | 의미 |
|------|------|
| `ENX_ERR_VDMA_INVALID_ARG = -EINVAL` 등 | POSIX errno alias. `strerror(-rc)` 호환 |
| `ENX_ERR_VDMA_DRV_MISMATCH = 0x7000A001` 등 | libvdma 확장 코드 (0x7000A000 ~) |

API return value 규약:
- **0 on success, negative errno on failure** (int API)
- pointer 반환 API: NULL on failure + positive errno set

자주 보는 에러 :

| 에러 | 의미 |
|------|------|
| `ENX_ERR_VDMA_INVALID_ARG` | NULL 포인터 / 잘못된 인자 |
| `ENX_ERR_VDMA_NO_DEVICE` | device 미 초기화 (`*_Init()` 안 부름) |
| `ENX_ERR_VDMA_BAD_FD` | 치명적 ioctl 후 fd invalidate |
| `ENX_ERR_VDMA_QUOTA_EXCEEDED` | per-device max_bufs 초과 |
| `ENX_ERR_VDMA_FORK_STALE` | fork 자식이 부모 instance 사용 시도 |
| `ENX_ERR_VDMA_BUSY` | HW engine busy |
| `ENX_ERR_VDMA_TIMED_OUT` | submit timeout |
| `ENX_ERR_VDMA_DRV_MISMATCH` | 다른 driver 용 API 호출 |
| `ENX_ERR_VDMA_HW_FAULT` | HW 가 error 보고 (JPEG overflow 등) |

---

## 8. 디버깅 — debugfs

`CONFIG_DEBUG_FS=y` 빌드에서 자동 노출. 권한: root only.

```
/sys/kernel/debug/enx_vdma/<node_name>/
├── stats          ← in_flight / submits / completed / bytes_inuse / sessions_open
├── bufs           ← 활성 buf 목록 (id / kind / size / refs / owner_pid)
├── jobs           ← 활성 job 목록 (id / pid / srcs / flags / done / result)
├── sessions       ← 활성 session 목록 (pid / bufs / imports / jobs / bytes_owned)
│
└── (각 driver 가 추가하는 항목)
    ├── regs           ← HW reg snapshot + decoded fields
    ├── last_submit    ← 가장 최근 hw_run_once 호출 snapshot
    └── (font 추가) mode / irq_stats
```

### 자주 쓰는 워크플로우

| 의심 | 보는 곳 |
|------|------|
| 메모리 누수 | `sessions` 의 pid 별 `bytes_owned` |
| HW hang | `stats` 의 `in_flight` 지속 증가, `regs` 의 `GO_BIT` |
| Buf 잘못 잡힘 | `bufs` 의 owner_pid + refs |
| 마지막 submit 분석 | per-driver `last_submit` |

상세는 [CORE.md §9](CORE.md) 참고.

---

## 9. 운영 시 자주 묻는 케이스

**Q. 한 프로세스가 죽어도 다른 프로세스는 멀쩡한가?**
A. 그렇습니다. 죽은 프로세스의 fd 가 정리되면서 `enx_vdma_release()` 가 호출되고,
그 fd 소유 buffer + 진행 중 job 만 회수합니다.

**Q. fork() 후 자식이 부모의 라이브러리 상태를 그대로 쓸 수 있나?**
A. 안 됩니다. `pthread_atfork` 핸들러가 자식 측 lib 상태를 무효화하므로,
자식은 `ENX_DMA_<Func>_Init()` 을 다시 호출해야 합니다. 무효 상태에서 API 호출은
`-ENX_ERR_VDMA_FORK_STALE` 반환.

**Q. 같은 buf 를 두 프로세스가 공유하고 싶다.**
A. EXPORT / IMPORT ioctl 을 직접 사용 (또는 lib 헬퍼). anon_inode fd 를 SCM_RIGHTS
로 전달하면 받은 프로세스에서 IMPORT 로 같은 backing 을 매핑할 수 있습니다.
같은 backing 을 두 번 IMPORT 하면 `-EEXIST` + 기존 view 반환 (idempotent).

**Q. 공유한 buf 를 받은 쪽이 free 하면 보낸 쪽도 못 보나?**
A. 아닙니다. backing 은 kref. 모든 view 가 사라질 때만 ecmm 회수.

**Q. SUBMIT 도중 다른 프로세스가 끼어들 수 있나?**
A. 끼어들 수 없습니다. `dev->hw_lock` 이 worker 의 `hw_run_once` 진입~완료까지
잡혀 있고 모든 HW 접근은 그 안에서만 일어납니다.

**Q. submit 중에 다른 스레드가 같은 addr 를 free 하면?**
A. `find_buf_by_virt()` 가 lookup + transient ref bump 까지 한 번에 잡으므로
보호됩니다. submit 이 완료된 뒤에 실제 free 가 일어납니다.

**Q. Exit 후에 alloc 받았던 virt_addr 를 더 만져도 되나?**
A. 안 됩니다. Exit 가 munmap 을 수행하므로 SEGV 위험.

**Q. font 의 VIDEO_CORE 모드는 무엇인가?**
A. RTOS firmware 가 HW 를 직접 제어하는 모드. kernel 은 descriptor 만 채우고
firmware 에 위임. `Font_Init(ENX_VDMA_INIT_TYPE_VIDEO_CORE)` 로 선택.

**Q. 같은 device 를 한 프로세스에서 여러 번 Init 해도 되나?**
A. 됩니다. per-leaf singleton + refcount — 두 번째 호출은 refcount++ 만 함.
Exit 도 같은 횟수만큼 호출.

---

## 10. 빌드

### 유저 라이브러리

`userapp/lib/` 에 Makefile 제공:

```sh
cd skeleton/userapp/lib
make                           # enx_vdma.a + enx_vdma.so
make CROSS_COMPILE=riscv64-enx142-linux-gnu-
make clean
```

산출물 :
- `enx_vdma.a` — static
- `enx_vdma.so` — shared
- `enx_vdma.h` (헤더는 install 대상)

### 사용자 앱 (per-function 데모)

```sh
cd skeleton/userapp/scaler && make    # scaler demo
cd skeleton/userapp/font   && make    # font demo
cd skeleton/userapp/jpegenc && make
cd skeleton/userapp/jpegdec && make
```

### 커널 모듈

`skeleton/` 의 Makefile 이 BSP 의 kbuild 와 통합:

```sh
cd linux_bsp_EN683
source ./Cross.param
cd sw_module/Link/skeleton
make
```

빌드 결과:
- `enx_vdma.ko` — core helper (EXPORT_SYMBOL_GPL)
- `font-drv/enx_vdma-font.ko`
- `dz-drv/enx_vdma-dz.ko`
- `jpegenc-drv/enx_vdma-jpegenc.ko`
- `jpegdec-drv/enx_vdma-jpegdec.ko`

로드 (의존성 순서) :

```sh
insmod enx_vdma.ko
insmod font-drv/enx_vdma-font.ko
insmod dz-drv/enx_vdma-dz.ko
insmod jpegenc-drv/enx_vdma-jpegenc.ko
insmod jpegdec-drv/enx_vdma-jpegdec.ko

# 또는 modprobe (core 자동 의존성 해결)
modprobe enx_vdma-font
modprobe enx_vdma-dz
modprobe enx_vdma-jpegenc
modprobe enx_vdma-jpegdec
```

### Driver 추가 / 제외

`skeleton/Makefile` 의 `DRV_MODULES` 변수로 빌드 대상 driver 선택:

```makefile
# 모두 빌드 (default)
DRV_MODULES ?= dz-drv/ font-drv/ jpegenc-drv/ jpegdec-drv/

# 일부만 빌드
make DRV_MODULES="font-drv/ dz-drv/"
```

---

## 11. 추가 자료

- **[ARCHITECTURE.md](ARCHITECTURE.md)** — 전체 시스템 토폴로지 (Mermaid 다이어그램)
- **[CORE.md](CORE.md)** — function-agnostic core 의 자료구조 / lifecycle / lock 계층 / debugfs / driver 인터페이스
- **[mem_mgmt.md](mem_mgmt.md)** — cross-fd buffer 공유의 설계 근거
- **[driver_structs.md](driver_structs.md)** — 자료구조 관계 다이어그램
- **[note.md](note.md)** — 라운드별 변경 / trade-off 이력
- **[ref_submit-v2.md](ref_submit-v2.md)** — UAPI V2 / reserved 필드 도입 메모

---

## 12. 한 줄 요약

> **사용자는 `vdma_addr_t` 또는 buf id 만 보고, 라이브러리는 per-leaf singleton +
> virt → buf 역방향 lookup 으로 격리/race 를 보호하고, 커널 core 는 HW 직렬화 +
> cross-fd 격리 + lifecycle 을 책임지며, 각 HW driver 는 자기 HW 시퀀스만 작성한다.
> 네 계층이 각자 자기 책임만 지키면 멀티 프로세스 / 멀티 스레드 / cross-fd 공유가
> 자동으로 안전해진다.**
