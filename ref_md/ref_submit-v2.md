# SUBMIT UAPI V2 — Async Support 도입 메모

> **상태: 아직 미적용 / 설계 메모.**
> 현재 font 의 SUBMIT 은 `enx_en683_font_submit_args` (V1) 한 가지만 사용하고
> SYNC only 입니다. ASYNC + V2 는 ABI 호환성 유지하며 도입할 때의 패턴.

---

## 1. 배경

현재 [font-drv/en683-font-uapi.h](../font-drv/en683-font-uapi.h) 의 V1 struct:

```c
struct enx_en683_font_submit_args {
    u32 dst_type_index;
    u16 dst_width;
    u16 dst_height;
    u32 background_overlay;
    u32 font_number;
    struct enx_en683_font_blit blits[];   /* flexible array */
};
```

이 구조에는 `flags` / `job_id` / `user_token` 같은 ASYNC 메타데이터가 없음.
ASYNC 도입 시 같은 struct 에 필드 추가하면 **ABI 깨짐** — 기존 user binary 와
호환 불가.

---

## 2. 결정 — V2 ioctl 추가, V1 유지

mainline 의 V4L2 / DRM 진화 방식과 동일:

| 항목 | 결정 |
|---|---|
| 기존 V1 struct / ioctl | **수정 금지** — legacy binary 호환 |
| 신규 ASYNC 기능 | **V2 struct + V2 ioctl 새로 정의** |
| kernel 본체 | V2 시멘틱으로 단일화, V1 은 wrapper 로 V2 호출 |
| 라이브러리 | 항상 V2 호출 |
| ABI 호환성 | 100% (legacy binary 그대로 동작) |
| 미래 확장 | V2 에 `__resv[]` 두면 V3 회피 가능 |

---

## 3. 제안 V2 struct

```c
struct enx_en683_font_submit_args_v2 {
    /* async wrapper */
    u32 submit_flags;        /* ENX_SUBMIT_SYNC / _ASYNC */
    u32 job_id;              /* OUT (ASYNC only) */
    u64 user_token;          /* IN, echo back on completion */
    u32 __resv[4];           /* future expansion — must be 0 */

    /* V1 payload 와 동일 */
    u32 dst_type_index;
    u16 dst_width;
    u16 dst_height;
    u32 background_overlay;
    u32 font_number;
    struct enx_en683_font_blit blits[];
};
```

### Future-proof reserved 필드
`__resv[4]` 는 미래 V3 회피용. 커널이 0 인지 검증 → 0 이 아니면 `-EINVAL`. 미래
의미 부여 시 0 이 아닌 값으로 update 가능 (구버전 user 가 0 으로 보내면 호환).

---

## 4. ioctl 번호

```c
/* 기존 — V1, sync only */
#define VDMAIOSET_FONT_SUBMIT     _IOWR(ENX_VDMA_IOC_MAGIC, 0x30, struct enx_en683_font_submit_args)

/* 신규 — V2, sync + async */
#define VDMAIOSET_FONT_SUBMIT_V2  _IOWR(ENX_VDMA_IOC_MAGIC, 0x31, struct enx_en683_font_submit_args_v2)
```

---

## 5. 커널 측 구현 패턴

V2 시멘틱으로 단일화, V1 은 그 위의 wrapper:

```c
static long font_ioctl_submit_v2(struct enx_vdma_sess *sess, void __user *uarg)
{
    /* 본체 — 모든 SUBMIT 이 결국 여기로 */
    /* validate flags, __resv[]=0 검증, memdup_user, enx_vdma_submit 호출 */
}

static long font_ioctl_submit(struct enx_vdma_sess *sess, void __user *uarg)
{
    /* V1 → V2 wrapper */
    struct enx_en683_font_submit_args_v2 v2 = { 0 };
    struct enx_en683_font_submit_args v1_hdr;

    if (copy_from_user(&v1_hdr, uarg, sizeof(v1_hdr)))
        return -EFAULT;

    /* V1 은 SYNC 강제, async 메타 없음 */
    v2.submit_flags = ENX_SUBMIT_SYNC;
    v2.job_id       = 0;
    v2.user_token   = 0;
    v2.dst_type_index    = v1_hdr.dst_type_index;
    v2.dst_width         = v1_hdr.dst_width;
    v2.dst_height        = v1_hdr.dst_height;
    v2.background_overlay= v1_hdr.background_overlay;
    v2.font_number       = v1_hdr.font_number;
    /* blits flex array copy ... */

    return font_do_submit_v2(sess, &v2);
}
```

ioctl 디스패처:
```c
case VDMAIOSET_FONT_SUBMIT:    return font_ioctl_submit(sess, uarg);     /* V1 → V2 wrapper */
case VDMAIOSET_FONT_SUBMIT_V2: return font_ioctl_submit_v2(sess, uarg);  /* 본체 */
```

---

## 6. lib 측 — 항상 V2 호출

`libvdma_font` 는 V2 만 사용:
- 신규 빌드된 user app + lib → V2 ioctl → SYNC/ASYNC 모두 활용 가능
- 기존 binary (V1 ioctl 직접 호출) → 그대로 동작 (V1 wrapper 가 V2 본체로 위임)

lib 측 새 API (예시):
```c
int vdma_font_submit_async(const struct vdma_submit_req *req,
                           uint64_t user_token, uint32_t *job_out);
int vdma_font_wait(uint32_t job_id, uint32_t timeout_ms, uint64_t *token_out);
int vdma_font_drain(vdma_completion_t *out, size_t cap, uint32_t timeout_ms);
```

→ submit_async / wait / drain — 이미 [libvdma_font.h](../userapp/libvdma_font.h) 에
선언은 되어 있지만 V1 SYNC only 라서 의미 없음. V2 도입 시 비로소 동작.

---

## 7. 도입 시 체크리스트

- [ ] V2 struct 정의 (`__resv[4]` 포함)
- [ ] `VDMAIOSET_FONT_SUBMIT_V2` ioctl 번호 추가
- [ ] `font_do_submit_v2()` 본체 구현 (validate __resv, flags, SYNC/ASYNC 분기)
- [ ] V1 ioctl 핸들러를 V2 wrapper 로 전환
- [ ] core 의 `enx_vdma_submit` 가 ASYNC 경로 이미 지원 — `ENX_SUBMIT_ASYNC` flag 만 전달
- [ ] lib 의 `vdma_font_submit_async / wait / drain` 구현 (현재 stub)
- [ ] `example_vdma_font.c::demo_async_batch` / `demo_async_worker` 검증

---

## 8. 관련 파일

- [font-drv/en683-font-uapi.h](../font-drv/en683-font-uapi.h) — 현재 V1
- [font-drv/en683-font.c](../font-drv/en683-font.c) — `font_ioctl_submit`
- [enx-vdma-core.c](../enx-vdma-core.c) — `enx_vdma_submit` (ASYNC 경로 이미 구현)
- [userapp/libvdma_font.h](../userapp/libvdma_font.h) — lib 측 async API 선언
- [README.md](README.md) §7 — 통합 체크리스트의 "ASYNC lib API" 항목
