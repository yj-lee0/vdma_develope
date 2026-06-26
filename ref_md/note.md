# libvdma_font — 주요 변경점 및 Trade-off 노트

> 라운드별 변경 이력과 trade-off. 신규 라운드는 위쪽에 추가.
>
> | 라운드 | 날짜 | 주제 | 섹션 |
> |--------|------|------|------|
> | R4 | 2026-05-24 | Core/Driver 분리 (디렉토리 재구성, function-agnostic core .ko) | §000 |
> | R3 | 2026-05-22 | vir_addr 기반 API 전환 (vdma_buf_t → vdma_addr_t) | §00 |
> | R2 | 2026-05-22 | Cross-fd CMA 공유 (EXPORT/IMPORT) | §0 |
> | R1 | 2026-05-21 | close 강제 정리 / buf transient ref / 인스턴스 격리 | §1-§7 |

---

## 000. R4 — Core / Driver 분리

### 000.1 변경 요약

기존 monolithic `en683-font-drv.c` (~800 줄) 을 **function-agnostic core**
와 **function-specific driver** 로 분리. 향후 새 VDMA function (scaler,
blender 등) 을 driver 폴더만 추가해서 손쉽게 확장 가능한 구조로 전환.

### 000.2 새 디렉토리 구조

```
skeleton/
├── en683-vdma.h            ← 커널 내부 헤더
├── en683-vdma-uapi.h       ← 공통 UAPI
├── en683-vdma-core.c       ← core .ko
├── userapp/                ← 사용자 라이브러리 + 예제
└── font-drv/               ← font driver .ko
```

### 000.3 분리 기준 — "submit 만 다르다"

| 컴포넌트 | 위치 | 이유 |
|---------|------|------|
| open / release / mmap / poll | core | 모든 function 동일 |
| ALLOC / FREE / WAIT / EXPORT / IMPORT ioctl | core | 모든 function 동일 |
| Cross-fd 공유 (anon_inode) | core | 모든 function 동일, 또한 fops 단일 instance 보장이 cross-function 공유의 전제 |
| SUBMIT 의 공통 흐름 (validate, kref, queue_work, wait) | core | `enx_vdma_submit()` 헬퍼로 노출 |
| **HW 레지스터 시퀀스** | driver | function 별 고유 |
| **SUBMIT UAPI struct** (`en683_font_blit` 등) | driver | function 별 고유 |
| Function-specific ioctl 디스패처 | driver | function 별 SUBMIT ioctl 번호 |

### 000.4 Core 의 공개 API (EXPORT_SYMBOL_GPL)

```c
/* file_operations 콜백 */
enx_vdma_open, enx_vdma_release, enx_vdma_mmap, enx_vdma_poll

/* Common ioctl helper */
enx_vdma_ioctl_alloc, _free, _wait, _export, _import

/* SUBMIT helper (driver 의 function-specific copy_from_user 다음에 호출) */
enx_vdma_submit

/* Device lifecycle */
enx_vdma_core_alloc, _register, _unregister, _free

```

총 15개 EXPORT_SYMBOL_GPL. ABI surface 작아 유지 비용 낮음.

### 000.5 Driver 의 callback 인터페이스

```c
struct enx_vdma_core_ops {
    int  (*hw_run_one)(struct enx_vdma_dev *, struct enx_vdma_buf *dst,
                       struct enx_vdma_buf *src, const void *blit_params);
    void (*hw_abort)(struct enx_vdma_dev *);
    const char *name;
    size_t      blit_size;
};
```

driver 가 작성할 코드는:
1. `hw_run_one()` — HW 레지스터 시퀀스
2. 자기 SUBMIT ioctl 핸들러 (function-specific UAPI struct copy_from_user
   → `enx_vdma_submit()` 호출)
3. `fops` 정의 (core 의 open/release/mmap/poll 그대로 사용)
4. `platform_driver` probe/remove
5. `module_init/exit` (대부분 `module_platform_driver()` 한 줄)

**전체 분량 ~250 줄** — 새 function 추가 비용 최소화.

### 000.6 Trade-off 정리

| 변경 | 얻는 것 | 잃는 것 / 부작용 |
|------|--------|----------------|
| Core/Driver 분리 (.ko 별도) | • 새 function 추가 비용 ↓ (driver ~250줄)<br>• core 버그 수정 1번 → 모든 driver 반영<br>• function 별 enable/disable 가능 | • EXPORT_SYMBOL ABI 관리 (vendor BSP 환경에선 실질 부담 X)<br>• modprobe 의존성 (자동 해결) |
| Cross-function 공유 가능 | • font→scaler 등 zero-copy 파이프라인 미래 가능 | • 없음 (core 의 단일 fops instance 가 자동 보장) |
| 디렉토리 분리 (userapp/, font-drv/) | • 영역별 명확성<br>• 새 function 폴더 추가만으로 확장 | • include path 늘어남 (`-I. -Ifont-drv`) |
| UAPI 분리 (`en683-vdma-uapi.h` + driver 별) | • function 별 UAPI 독립 변경<br>• 공통 ioctl 번호 충돌 없음 | • include 2단 (driver UAPI 가 common 자동 끌어옴) |

### 000.7 다른 옵션 (단일 .ko vs 정적 link) 검토 결과

- **Option A (단일 .ko, 모든 function 포함)**: 거부 — 사용 안 하는 function
  도 항상 로드, function 별 Kconfig 세분화 어려움
- **Option B (정적 link, driver 마다 core 사본)**: 거부 — **anon_inode fops
  주소 불일치로 cross-function buffer 공유 불가**, 메모리 중복
- **Option C (core 별도 .ko + driver .ko)**: ✅ **채택** — runtime 동일, 메모리
  최소, cross-function 공유 가능, function 별 enable/disable, drm/v4l2 표준 패턴

### 000.8 사용자 측 영향 = 0

UAPI ioctl 번호와 struct 모양은 변경 없음 (numbers: ALLOC=0x01, FREE=0x02,
WAIT=0x11, SUBMIT=0x10, EXPORT=0x20, IMPORT=0x21). `vdma_font` 라는 cdev
노드 이름도 동일. 사용자 라이브러리만 include 경로 한 줄 추가:

```sh
# 전:
gcc -I. example_vdma_font.c libvdma_font.c -lpthread -o ...

# 후:
gcc -I. -Ifont-drv userapp/example_vdma_font.c userapp/libvdma_font.c \
    -lpthread -o ...
```

### 000.9 새 function 추가 절차 (예: scaler)

1. `skeleton/scaler-drv/` 디렉토리 생성
2. `scaler-drv/en683-scaler-uapi.h` 작성 (`struct en683_scaler_blit`,
   `EN683_SCALER_IOC_SUBMIT = _IOWR(MAGIC, 0x12, ...)`)
3. `scaler-drv/en683-scaler.c` 작성 (~250줄, font 를 템플릿으로)
4. `scaler-drv/Kbuild` 작성
5. 사용자 라이브러리에 `vdma_scaler_*` API 추가 (별도 lib 또는 같은 .so)

→ core, font 는 무변경.

### 000.10 검증

- userapp 빌드: `gcc -std=c11 -O2 -Wall -Wextra` 통과, warning 0
- core / driver 빌드: BSP Kbuild 환경 필요 (host 에서는 syntax 만 검증)

---

## 00. R3 — vir_addr 기반 API 전환

### 00.1 변경 요약

`vdma_buf_t` opaque handle 을 사용자 헤더에서 제거하고, 대신 mmap 가상주소
(`vdma_addr_t = void *`) 를 단일 식별자로 노출. `vdma_buf_t` 구조는 라이브러리
내부 구현으로 강등. open/close 이름 통일. submit 은 `struct vdma_submit_req`
로 묶어 인자 단일화.

### 00.2 주요 변경점

**헤더 ([libvdma_font.h](libvdma_font.h))**
- `typedef void *vdma_addr_t` 추가, `vdma_buf_t` 제거
- `vdma_font_init/shutdown` → `vdma_font_open/close` 리네이밍
- `alloc_src/dst(size_t)` → vir_addr 반환 (cpu_out 인자 제거)
- `free(vdma_addr_t, size_t)` — size 는 0 또는 alloc 시 size 검증용
- `struct vdma_blit::src` 가 `vdma_addr_t`
- `struct vdma_submit_req { dst, blits, nblit }` 신설
- `vdma_font_submit(&req)` / `vdma_font_submit_async(&req, token, &job)`
- `vdma_font_export(vdma_addr_t, int *)` 시그니처 변경
- `vdma_font_import(int, size_t *, uint32_t *)` 시그니처 변경 (vir_addr 반환)
- ABI_VERSION 2 → 3
- `_on(path, ...)` 변형 함수 제거

**구현 ([libvdma_font.c](libvdma_font.c))**
- 신설: `find_and_acquire(addr, size_hint)` — vir_addr 역방향 lookup +
  inst->lock 보유 상태에서 transient ref bump 까지 atomic
- 신설: `resolve_submit()` / `unwind_submit()` — submit 의 dst+srcs 일괄 변환/해제
- 제거: `buf_check()`, `buf_acquire()`, `validate_submit()` — find_and_acquire 가 흡수
- 변경: `do_alloc()` 의 `cpu_out**` 인자 제거 (반환 buf->cpu 만 사용)
- 변경: export/import 가 vir_addr 시그니처
- 유지: `vdma_buf_t` 는 라이브러리 .c 내부 alias (`typedef struct vdma_buf vdma_buf_t`)

**예제**
- [example_vdma_font.c](example_vdma_font.c) — 4가지 데모 모두 새 API
- [example_vdma_share.c](example_vdma_share.c) — Producer/Consumer 도 새 API

**커널 / UAPI**
- **0줄 변경** — vir_addr 변환은 전부 라이브러리 안에서

### 00.3 Trade-off 정리

| 변경 | 얻는 것 | 잃는 것 / 부작용 |
|------|--------|----------------|
| vdma_buf_t → vdma_addr_t | • 사용자 mental model 단순 (malloc/free 패턴)<br>• 함수 시그니처 짧음<br>• 문서 분량 감소 | • opaque type 의 self-documenting 효과 → `vdma_addr_t` typedef alias 로 일부 회복 |
| alloc 의 cpu_out 제거 | • 반환값 하나로 충분 (vir_addr) | • 기존 코드 약간 더 수정 필요 |
| free 가 size 받음 | • alloc-free 정합 검증 가능 | • 사용자가 size 를 정확히 기억해야 함 (0 으로 skip 가능) |
| submit_req 구조체 | • self-contained, 로깅/직렬화 쉬움 | • 호출 시 임시 struct 한 줄 추가 |
| init/shutdown → open/close | • POSIX 친화적 이름 | • 기존 호출자 코드 sed 1줄 |
| buf_check / buf_acquire 제거 | • 코드 단순화, magic 검사 race 제거 | • find_and_acquire 가 같은 보호 더 강하게 수행 (lock 내 bump) |

### 00.4 안전성 개선 (부가 효과)

기존 `buf_acquire` 는 magic check 와 CAS 사이에 force_destroy 와 race 가
이론적으로 가능했음. 새 `find_and_acquire` 는 inst->lock 보유 상태에서
lookup + ref bump 까지 한 atomic 시퀀스에 묶어 race window 자체를 없앰.

### 00.5 검증

x86_64 호스트 `gcc -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter`:
- libvdma_font.c + example_vdma_font.c → 빌드 OK, warning 0
- libvdma_font.c + example_vdma_share.c → 빌드 OK, warning 0
- 24KB ELF 생성

### 00.6 다음 라운드 후보

- README/note/mem_mgmt 문서 라운드 정리 (본 갱신)
- export/import 의 envelope 패턴 (cross-process 공유 운영용)
- vdma_font_send_buf / recv_buf 헬퍼 (SCM_RIGHTS 래퍼)
- broker 예제

---

## 0. R2 — Cross-fd CMA 공유 (anon_inode 기반)

### 0.1 변경 요약

다른 프로세스가 같은 `/dev/vdma_font` 의 CMA buf 를 보게 하는 매커니즘.
UAPI placeholder 였던 `EN683_IOC_EXPORT/IMPORT` 를 anon_inode 기반으로 구현.

### 0.2 주요 변경점

**UAPI** ([en683-font-uapi.h](en683-font-uapi.h))
- `en683_export_args { id, export_fd }` 구체화
- `en683_import_args { export_fd, id, kind, size, mmap_offset }` 구체화

**커널** ([en683-font-drv.c](en683-font-drv.c))
- 신설 자료구조: `struct vdma_buf_attach { buf_node, file_node, buf, vf }`
- `vdma_buf` 에 `attach_list` 필드 — `xa_lock` 으로 보호
- `vdma_file` 에 `imports` 필드 — release 시 detach 정리
- `vdma_buf_lookup_get_owned` → `vdma_buf_lookup_get` 으로 일반화 (owner OR attached)
- `vdma_export_fops` — anon_inode 의 minimal fops (`.release` 만)
- `vdma_do_export` / `vdma_do_import` — `anon_inode_getfd` / `fget` 기반
- `vdma_do_free` — owner / attached 자동 분기
- `vdma_release` — imports 우선 정리, 그 다음 owned bufs

**라이브러리** ([libvdma_font.c](libvdma_font.c) / [.h](libvdma_font.h))
- `vdma_font_export(buf, &fd)` — buf_acquire 로 race 보호 + EXPORT ioctl
- `vdma_font_import(fd, &cpu)` — IMPORT + mmap + vdma_buf_t 생성

**예제** ([example_vdma_share.c](example_vdma_share.c) — 신규)
- fork + socketpair + SCM_RIGHTS 로 부모/자식 cross-fd 공유 양방향 검증

### 0.3 Trade-off 정리

| 변경 | 얻는 것 | 잃는 것 / 부작용 |
|------|--------|----------------|
| anon_inode 기반 EXPORT/IMPORT | • fd-as-capability (자동 보안)<br>• 라이프사이클 kref 자동<br>• dma-buf 의존성 0, ~200 LOC | • 외부 드라이버(V4L2/DRM) 와 zero-copy 연동 불가 → 필요 시 별도 dma-buf 경로 추가 필요 |
| buf->attach_list + buf_node/file_node | • 한 buf 가 여러 fd 에서 동시 사용 가능<br>• release 시 정확한 정리 | • lookup 시 linked list 순회 → 일반적으로 attach 수 작아 무시 가능 |
| FREE 가 owner/attached 자동 분기 | • UAPI 단순화 (DETACH 별도 ioctl 불필요) | • free 의미가 "내 ref 만 떨굼" 으로 살짝 미묘 — 헤더 contract 에 명시 |
| importer 가 같은 global id 사용 | • XArray 한 곳만 보면 됨<br>• mmap offset 일관 | • "id 42 가 누구의 것인지" 디버깅 시 attach_list 순회 필요 |
| 같은 fd 에서 같은 buf 재 import 차단 (-EEXIST) | • free 회수의 모호성 제거 | • 사용자가 어쩌다 두 번 호출 시 명시적 에러 (의도된 보호) |

### 0.4 검증된 라이프사이클 시나리오

| 상황 | 결과 |
|------|------|
| Producer 가 export 후 free → consumer 가 import | OK. anon_fd ref 덕에 buf 살아있음 |
| Producer 가 free → consumer 가 사용 중 | OK. attach ref 덕에 buf 살아있음 |
| Producer SIGKILL → consumer 사용 중 | OK. release 가 owner ref drop, attach 살아있음 |
| Consumer SIGKILL → producer 사용 중 | OK. consumer release 가 detach, producer 영향 없음 |
| 양쪽 모두 종료 | 모든 ref drop → CMA 회수 |

### 0.5 알려진 한계 (코드에 주석)

1. `copy_to_user` 실패 시 fd 누수 가능 — `get_unused_fd_flags + anon_inode_getfile + fd_install` 패턴으로 완벽 해결 가능. 현재는 best-effort.
2. cross-instance 공유 불가 — 같은 device 의 다른 inst 도 안 됨 (`buf->dev != dev` 검사).
3. kind 변환 불가 — SRC 만든 걸 DST 로 import 불가.
4. 외부 dma-buf 연동 없음 — 의도된 범위 외.

### 0.6 다음 라운드 후보

- 외부 dma-buf import/export (V4L2/DRM 연동)
- debugfs 에 attach 리스트 노출
- cross-instance 공유 허용 옵션
- `get_unused_fd_flags + fd_install` 분리 패턴으로 fd 누수 race 제거

자세한 설계 노트는 [mem_mgmt.md](mem_mgmt.md) 참조.

---

## 1. R1 — 변경 배경

---

## 1. 변경 배경

`libvdma_font`는 사용자 앱과 커널 드라이버 사이의 중간 레이어로,
싱글톤 스타일의 외부 API를 제공하면서 내부적으로는 여러 디바이스 fd를
관리할 수 있는 구조입니다.

이번 라운드에서는 다음 세 가지를 적용했습니다:

1. **shutdown 강제 정리** — 마지막 init-ref가 떨어질 때 해당 인스턴스의
   모든 자원(buf, mmap, fd)을 강제 해제.
2. **buf transient refcount** — submit 도중 다른 스레드가 같은 buf를
   free하는 race를 라이브러리 내부에서 보호.
3. **헤더 thread-safety matrix** — 사용자에게 안전/비효율/금지 영역을
   명시.

---

## 2. 주요 변경점

### 2.1 `struct vdma_inst` 확장

```c
struct vdma_inst {
    /* ... 기존 필드 ... */
    _Atomic int refcount;     /* 총: init + buf + transient */
    _Atomic int init_count;   /* + NEW: init() 호출 매칭용 */
    _Atomic int invalidated;
    /* ... */
};
```

- `refcount`: 인스턴스 살아있음의 모든 보유자 합계 (alloc 시 +1, buf 해제 시 -1, init 시 +1, shutdown 시 -1, drain snapshot 시 +1).
- `init_count`: **shutdown이 "마지막 init"을 판별하는 용도**. refcount 만으로는 init 외의 ref들(buf, transient)과 구분이 불가.

### 2.2 `vdma_font_init` / `vdma_font_shutdown`

```c
int vdma_font_init(const char *path) {
    /* refcount 증가 (find/open) + init_count 증가 */
}

void vdma_font_shutdown(const char *path) {
    /* init_count 감소.
     * 마지막 init이면:
     *   1) 레지스트리에서 unlink (g_reg_lock 보유 중)
     *   2) inst_force_destroy(inst) 호출
     */
}
```

### 2.3 `inst_force_destroy`

```c
static void inst_force_destroy(struct vdma_inst *inst)
{
    atomic_store(&inst->invalidated, 1);  /* 신규 진입 차단 */

    /* fd 조기 close → 다른 스레드의 poll/blocked ioctl 즉시 깨움 */
    int fd = inst->fd; inst->fd = -1;
    if (fd >= 0) close(fd);

    /* bufs 리스트 steal (inst->lock 잠깐 잡고) */
    pthread_mutex_lock(&inst->lock);
    vdma_buf_t *list = inst->bufs; inst->bufs = NULL;
    pthread_mutex_unlock(&inst->lock);

    /* 각 buf 강제 해제 — CAS 대기로 transient ref 드레인 */
    while (list) {
        vdma_buf_t *next = list->next;
        buf_force_destroy(list);
        list = next;
    }

    /* init의 inst-ref drop — 마지막 ref가 inst_destroy 트리거 */
    inst_put(inst);
}
```

### 2.4 `buf_acquire` / `buf_force_destroy`

**buf_acquire**: CAS로 `refcount > 0 → refcount + 1`.
- 이미 destruction 중이면 (`refcount <= 0`) 실패 → submit이 `-EINVAL` 반환
- 성공하면 transient ref 보유 → 다른 스레드의 force_destroy CAS가 1→0
  하지 못함 (refcount > 1)

**buf_force_destroy**: CAS로 `refcount == 1 → 0`.
- transient ref가 있으면 CAS 실패 → `sched_yield()` 후 재시도
- 성공하면 buf 메모리 안전하게 해제 (`munmap` + `free`)
- 커널측 `ioctl(FREE)`는 생략 (fd가 곧 닫히고 release()가 reap)

### 2.5 `vdma_font_submit` / `submit_async` 수정

진입 시 dst + 모든 srcs에 대해 `buf_acquire`, 종료 시 `buf_put`.

```c
int vdma_font_submit(vdma_buf_t *dst, const struct vdma_blit *srcs, size_t n)
{
    /* validate */
    if (buf_acquire(dst)) return -EINVAL;
    for (i = 0; i < n; i++) {
        if (buf_acquire(srcs[i].src)) goto out;
    }
    /* fill_submit + inst_ioctl(SUBMIT) */
out:
    for (i = 0; i < acquired; i++) buf_put(srcs[i].src);
    buf_put(dst);
    return r;
}
```

### 2.6 `inst_put` 버그 픽스

기존 `inst_put`은 레지스트리에서 inst를 찾지 못하면 (force-destroy가 미리
unlink한 경우) destroy를 호출하지 않아 **메모리 누수**가 발생할 수 있었음.
이제는 refcount가 0이 되면 항상 destroy.

### 2.7 헤더 contract 추가

`libvdma_font.h` 상단에 다음 두 블록 추가:

- **shutdown 강제 정리 조항** — "마지막 init-ref가 떨어지면 모든 buf/cpu/fd
  강제 해제, 이후 stale 포인터 접근 금지"
- **Thread safety matrix** — 동시 호출 안전 / 비효율 / 금지 케이스 명시

---

## 3. Trade-off 정리

| 변경 | 얻는 것 | 잃는 것 / 부작용 |
|------|--------|----------------|
| **shutdown 강제 정리** | • deinit 시점에 모든 자원 해제 보장<br>• 사용자 누수 흡수<br>• OS 종료 의존 제거 | • shutdown 이후 stale `vdma_buf_t *` / `cpu` 접근 시 UAF (contract로 금지)<br>• in-flight 잡이 있으면 close(fd)에서 잠시 블록 (커널 release의 flush_work)<br>• over-call 시 assert (디버그 빌드 한정 abort) |
| **buf transient refcount** | • free vs submit race 진짜 해결<br>• 멀티 스레드/멀티 모듈 환경 contract 강화 | • SUBMIT당 atomic CAS N+1번 추가 — OSD/60fps 워크로드 기준 측정 불가, 수만 회/초 빈도에선 측정 가능<br>• 코드 복잡도 약간 증가 |
| **shutdown 후 ioctl(FREE) 생략** | • 불필요한 syscall 절약 | • kernel-side 정리는 release() 의존 (의도된 변화) |
| **fd 조기 close (force-destroy 시작 시점)** | • 블록된 다른 스레드 즉시 깨움 (POLLHUP) | • 다른 스레드의 ioctl이 EBADF — invalidated로 이미 차단되므로 실질 차이 없음 |
| **inst_put unlink 누수 픽스** | • force-destroy가 미리 unlink한 경우에도 정확히 destroy | • 동작 차이 없음 (순수 버그 픽스) |
| **헤더 contract 강화** | • 사용자가 가정 가능한 영역 명확<br>• 디버깅/코드 리뷰 기준점 | • 기존에 contract 위반 중이던 사용자 코드는 *명시적으로* 잘못된 사용으로 분류됨 (실제 동작은 그대로) |

### 별로 안 잃는 것들 (의도된 작은 부작용)

- **`buf_force_destroy`의 sched_yield 스핀**: deinit 경로에서만 일어나므로 hot path 영향 0.
- **메모리**: `struct vdma_inst`에 4 바이트(`init_count`) 추가. 인스턴스당.
- **헤더 분량 증가**: 코드 변경 없이 contract 주석만 늘어남.

### 명시적으로 안 바꾼 것 (의도)

- **drain 동시 호출 차단** — 강제하면 단일 스레드 사용자에게 불필요한 락 부담. 헤더 권장사항으로만.
- **inst path 정규화 (realpath)** — 별도 라운드.
- **stats/introspection API** — 별도 라운드.

---

## 4. 검증

- `gcc -std=c11 -O2 -Wall -Wextra` 빌드 통과 (warning 0)
- `example_vdma_font.c` + `libvdma_font.c` 링크 → 24KB ELF
- x86_64 호스트에서 syntax/ABI 검증

---

## 5. 사용자 가이드 변경

### 종료 시 패턴

```c
/* 권장 */
vdma_font_init(NULL);
/* ... 사용 ... */
vdma_font_free(buf);       /* 명시적으로 free하면 더 안전 (UAF 가드) */
vdma_font_shutdown(NULL);  /* 안 free한 buf도 자동 정리 */

/* 금지 */
vdma_font_shutdown(NULL);
memset(cpu, 0, size);      /* ← shutdown 후 cpu 포인터 사용 UB */
```

### 멀티 스레드 패턴

```c
/* OK: 서로 다른 buf */
Thread A: vdma_font_submit(dst1, ...);
Thread B: vdma_font_submit(dst2, ...);

/* OK: 같은 buf의 submit + free — 라이브러리가 transient ref로 보호 */
Thread A: vdma_font_submit(buf, ...);   /* refcount=2 */
Thread B: vdma_font_free(buf);          /* unlink만, 실제 free는 A 완료 후 */

/* 권장: drain 호출자는 하나의 스레드 */
Drainer:  while (1) vdma_font_drain(out, N, -1);

/* 금지: 시그널 핸들러에서 호출 */
void handler(int sig) {
    vdma_font_submit(...);  /* ← async-signal-safe 아님 */
}
```

---

## 6. 향후 라운드 후보

| 우선 | 항목 | 이득 |
|------|------|------|
| 🟠 | **realpath 정규화** | 같은 디바이스를 다른 경로로 표현해도 단일 인스턴스로 dedupe |
| 🟠 | **stats / introspection API** | 운영 디버깅. alive inst, alive buf, inflight 잡 카운터 노출 |
| 🟢 | **MAX_INSTANCES enforce on init** | 등록 무제한 → silent drop 방지 |
| 🟢 | **destructor for auto-shutdown** | `__attribute__((destructor))`로 process exit 시 정리 |
| 🟢 | **환경변수 디버그 로그** | `VDMA_FONT_LOG=1`로 submit/free 추적 |
| 🟢 | **kernel-side**: `EN683_IOC_CANCEL`, `EN683_WAIT_NOBLOCK`, dma-buf | UAPI 확장은 BSP 통합 마일스톤 |

---

## 7. 관련 파일

> 경로는 R4 (core/driver 분리) 기준.

- [userapp/libvdma_font.h](userapp/libvdma_font.h) — 공개 API + contract
- [userapp/libvdma_font.c](userapp/libvdma_font.c) — 라이브러리 구현
- [userapp/example_vdma_font.c](userapp/example_vdma_font.c) — 사용 예제 (R3)
- [userapp/example_vdma_share.c](userapp/example_vdma_share.c) — Cross-fd 공유 데모 (R2)
- [en683-vdma-uapi.h](en683-vdma-uapi.h) — 공통 UAPI (R4)
- [en683-vdma.h](en683-vdma.h) — 커널 내부 헤더 (R4)
- [en683-vdma-core.c](en683-vdma-core.c) — core .ko (R4)
- [font-drv/en683-font-uapi.h](font-drv/en683-font-uapi.h) — Font 전용 UAPI (R4)
- [font-drv/en683-font.c](font-drv/en683-font.c) — Font driver .ko (R4)
- [mem_mgmt.md](mem_mgmt.md) — 공유 설계 노트 (R2)
- [driver_structs.md](driver_structs.md) — 자료구조 관계 노트
- [README.md](README.md) — 현재 구조 + 사용 가이드
