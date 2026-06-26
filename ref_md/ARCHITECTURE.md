# EN683 VDMA — Architecture Diagram

전체 아키텍처를 Mermaid 다이어그램으로 시각화. VSCode markdown preview, GitHub,
또는 https://mermaid.live 에서 렌더링됨.

---

## 1. 전체 아키텍처 (Core / Leaves / User)

`architecture.dot` 과 동일한 layout — 3 horizontal stripes (User → Kernel → HW),
Kernel 내부는 좌 **CORE** · 우 **Leaf Modules** 로 분할.

```mermaid
graph TB
    %% ============================================================
    %% Top stripe : User Space
    %% ============================================================
    subgraph UserSpace["User Space"]
        direction TB
        subgraph Apps[" "]
            direction LR
            APP1[font app]
            APP2[scaler app]
            APP3[jpegenc app]
            APP4[jpegdec app]
            APP5[unified app]
        end
        LIB["<b>libvdma.so / libvdma.a</b><br/><i>unified, device-indexed singleton</i><br/>g_dev_inst[ENX_VDMA_FUNC_END]<br/>open · close · alloc · free<br/>*_submit · export · import"]
        APP1 --> LIB
        APP2 --> LIB
        APP3 --> LIB
        APP4 --> LIB
        APP5 --> LIB
    end

    %% ============================================================
    %% Middle stripe : Kernel Space (CORE left, Leaves right)
    %% ============================================================
    subgraph KernelSpace["Kernel Space"]
        direction LR

        subgraph Core["enx_vdma.ko — CORE"]
            direction LR
            subgraph CoreInner[" "]
                direction TB
                COMMON["<b>Common ioctls</b><br/>ALLOC · FREE · MMAP<br/>EXPORT · IMPORT<br/>WAIT · ABORT"]
                MEM["<b>Memory model</b><br/>backing + buf (kref'd)<br/>cross-device share"]
                SUBMIT["<b>SUBMIT dispatcher</b><br/>SYNC inline<br/>ASYNC workqueue"]
                DBG["<b>debugfs</b><br/>stats · bufs<br/>jobs · sessions"]
            end
            OPS["<b>EXPORT_SYMBOL_GPL<br/>boundary</b><br/>enx_vdma_submit<br/>enx_vdma_abort<br/>enx_vdma_open/release<br/>enx_vdma_mmap/poll<br/>ioctl helpers<br/>core_register"]
        end

        subgraph Leaves["Leaf Modules"]
            direction LR
            FONT["<b>enx_vdma-font.ko</b><br/>/dev/enx_vdma_font"]
            DZ0["<b>enx_vdma-dz.ko ch0</b><br/>/dev/enx_vdma_dz0"]
            DZ1["<b>enx_vdma-dz.ko ch1</b><br/>/dev/enx_vdma_dz1"]
            JENC["<b>enx_vdma-jenc.ko</b><br/>/dev/enx_vdma_jenc"]
            JDEC["<b>enx_vdma-jdec.ko</b><br/>/dev/enx_vdma_jdec"]
        end
    end

    %% ============================================================
    %% Bottom stripe : HW
    %% ============================================================
    subgraph HW["HW (EN683 SoC)"]
        direction LR
        HW_F[(FONT engine)]
        HW_DZ0[(DZ ch0)]
        HW_DZ1[(DZ ch1)]
        HW_JE[(JPEG enc)]
        HW_JD[(JPEG dec)]
    end

    %% ─── Cross-stripe edges ───
    %% lib → leaves (가시, 굵게)
    LIB ==>|"ioctl / mmap<br/>via /dev"| Leaves

    %% leaves → core (EXPORT_SYMBOL_GPL boundary, dashed)
    Leaves -.->|"core_register<br/>core_ops"| OPS

    %% leaves → HW (1:1 column-aligned)
    FONT --> HW_F
    DZ0  --> HW_DZ0
    DZ1  --> HW_DZ1
    JENC --> HW_JE
    JDEC --> HW_JD

    %% ─── Styles (architecture.dot 와 동일 팔레트) ───
    style UserSpace fill:#e8f5e9,stroke:#2e7d32,stroke-width:2px
    style KernelSpace fill:#f5f5f5,stroke:#666666,stroke-width:2px
    style Core fill:#e1f5ff,stroke:#0066cc,stroke-width:2px
    style Leaves fill:#fff4e6,stroke:#cc6600,stroke-width:2px
    style HW fill:#fafafa,stroke:#999999,stroke-width:2px
    style LIB fill:#c8e6c9,stroke:#2e7d32
    style OPS fill:#fff9c4,stroke:#f57f17,stroke-width:2px
    style Apps fill:#e8f5e9,stroke:none
    style CoreInner fill:#e1f5ff,stroke:none

    linkStyle default stroke:#666666
```

> **Mermaid 의 한계** : Mermaid 의 auto-layout (dagre/elkjs) 는 dot 와 달리 cluster
> 좌/우 위치를 strict 하게 지정할 수 없습니다. Kernel Space 내부 `direction LR` 로
> CORE 좌측 / Leaf Modules 우측 배치를 hint 하지만, 정확한 픽셀 정렬이 필요하면
> `architecture.png` / `.svg` 를 사용하세요.

---

## 2. 데이터 흐름 (ALLOC + SUBMIT + EXPORT/IMPORT)

```mermaid
sequenceDiagram
    autonumber
    actor User
    participant Lib as libvdma.so
    participant Core as enx_vdma.ko
    participant Leaf as Leaf (e.g., dz)
    participant HW as HW Engine

    Note over User,HW: ALLOC + MMAP
    User->>Lib: vdma_open(ENX_VDMA_SCALER0)
    Lib->>Core: open(/dev/enx_vdma_dz0)
    Core-->>Lib: fd, session
    User->>Lib: vdma_alloc_src(func, size)
    Lib->>Core: ioctl(VDMAIOSET_ALLOC)
    Core->>Core: backing alloc + buf create
    Core-->>Lib: id + mmap_offset
    Lib->>Core: mmap(fd, mmap_offset)
    Core-->>Lib: vir_addr
    Lib-->>User: vdma_addr_t

    Note over User,HW: SUBMIT (SYNC inline)
    User->>Lib: vdma_dz_submit(func, &req)
    Lib->>Core: ioctl(VDMAIOSET_DZ_SUBMIT)
    Core->>Leaf: ops->hw_run_once()
    Leaf->>HW: reg pgm + GO
    HW-->>Leaf: IRQ (done)
    Leaf-->>Core: result
    Core-->>Lib: ioctl return
    Lib-->>User: 0 / errno

    Note over User,HW: EXPORT / IMPORT (cross-device share)
    User->>Lib: vdma_export(SCALER0, addr, &fd)
    Lib->>Core: ioctl(VDMAIOSET_EXPORT)
    Core-->>Lib: anon_inode fd
    User->>Lib: vdma_import(JPEGENC, fd, KIND_SRC)
    Lib->>Core: ioctl(VDMAIOGET_IMPORT)
    Core->>Core: 새 buf view, 같은 backing
    Core-->>Lib: 새 id + mmap_offset
    Lib-->>User: 새 vdma_addr_t (same memory)
```

---

## 3. Core 의 `ops` 인터페이스 (Driver 가 구현)

```mermaid
classDiagram
    class enx_vdma_dev {
        +struct cdev cdev
        +struct xarray buf_xa
        +struct xarray job_xa
        +struct mutex hw_lock
        +struct workqueue_struct *wq
        +struct list_head sessions
        +const struct enx_vdma_core_ops *ops
        +const struct file_operations *fops
    }

    class enx_vdma_core_ops {
        <<interface>>
        +int hw_run_once(vs, dst, src, ...)
        +void hw_abort(dev)
        +int session_open(vs)
        +void session_release(vs)
        +size_t blit_size
    }

    class FontOps {
        +font_hw_run_once()
        +font_hw_abort()
        +font_session_open()
        +font_session_release()
    }

    class DzOps {
        +dz_hw_run_once()
        +dz_hw_abort()
    }

    class JencOps {
        +jenc_hw_run_once()
        +jenc_hw_abort()
    }

    class JdecOps {
        +jdec_hw_run_once()
        +jdec_hw_abort()
    }

    enx_vdma_dev --> enx_vdma_core_ops : ops pointer
    enx_vdma_core_ops <|.. FontOps
    enx_vdma_core_ops <|.. DzOps
    enx_vdma_core_ops <|.. JencOps
    enx_vdma_core_ops <|.. JdecOps
```

---

## 4. 메모리 모델 — Backing / Buf / View

> **Session 모델** : Session 은 `/dev/enx_vdma_*` 의 **fd 별로** 1개씩 생성됩니다
> (`open()` → `struct enx_vdma_session` 1개 → `file->private_data` 보관).
> 따라서 같은 process 라도 device 를 N 개 열면 session 도 N 개. 아래
> 예시에선 **Process A 가 font/dz0 두 개 fd 를, Process B 가 jenc fd 하나**
> 를 열었으므로 총 **3 session** 이 있습니다.

```mermaid
graph LR
    subgraph BackingPool["Backing Pool (physical memory)"]
        BK1["backing #1<br/>(ecmm 4MB, kref=3)"]
        BK2["backing #2<br/>(ecmm 1MB, kref=1)"]
    end

    subgraph FontDev["font dev — buf_xa"]
        SESS_FA(["font_sess_A<br/>(Proc A's font fd)"])
        BUF_F1["buf #5 (DST)<br/>backing=BK1<br/>owner=font_sess_A"]
        SESS_FA -. owns .-> BUF_F1
    end

    subgraph DzDev["dz0 dev — buf_xa"]
        SESS_DA(["dz_sess_A<br/>(Proc A's dz0 fd)"])
        BUF_DZ1["buf #12 (SRC)<br/>backing=BK1<br/>owner=dz_sess_A"]
        SESS_DA -. owns .-> BUF_DZ1
    end

    subgraph JencDev["jenc dev — buf_xa"]
        SESS_JB(["jenc_sess_B<br/>(Proc B's jenc fd)"])
        BUF_J1["buf #3 (SRC, imported)<br/>backing=BK1<br/>owner=jenc_sess_B"]
        BUF_J2["buf #7 (DST)<br/>backing=BK2<br/>owner=jenc_sess_B"]
        SESS_JB -. owns .-> BUF_J1
        SESS_JB -. owns .-> BUF_J2
    end

    BUF_F1  -. shared backing .-> BK1
    BUF_DZ1 -. shared backing .-> BK1
    BUF_J1  -. shared backing .-> BK1
    BUF_J2  --> BK2

    style BackingPool fill:#fff9c4,stroke:#f57f17
    style FontDev fill:#e1f5ff
    style DzDev fill:#f3e5f5
    style JencDev fill:#e8f5e9
    style SESS_FA fill:#bbdefb,stroke:#1565c0
    style SESS_DA fill:#e1bee7,stroke:#7b1fa2
    style SESS_JB fill:#c8e6c9,stroke:#2e7d32
```

해석:
- **Session 은 fd-당-1개** — Process A 가 font/dz0 두 fd 를 열어서 `font_sess_A`,
  `dz_sess_A` 두 session 이 각각 다른 device 의 `sessions` 리스트에 등록됨.
- **buf ↔ session 1:N** — 한 session 이 그 device 안에서 여러 buf 를 보유 가능.
  (Process B 의 jenc session 은 SRC + DST 두 buf 소유)
- **buf ↔ backing 1:N (shared)** — backing 은 kref 로 보호되며 서로 다른 session
  (또는 다른 device) 의 buf 끼리 EXPORT/IMPORT 로 공유. BK1 은 3 session
  (font/dz/jenc) 에서 모두 view 를 들고 있는 cross-device share 사례.
- **`session.bufs` 리스트** — 각 session 은 자기가 만든 buf 만 track. session 닫힘
  (release) 시 해당 buf 들의 refcount drop → 마지막 ref 떨어지면 backing 도 free.
- 각 dev 의 `buf_xa` 는 독립된 id 공간 (그래서 #5, #12, #3, #7 각각 다른 dev 의 id).

---

## 5. UserApp Singleton 구조

```mermaid
graph TB
    subgraph Process["User Process"]
        subgraph LibState["libvdma 내부 상태"]
            ARR["g_dev_inst[ENX_VDMA_FUNC_END]<br/>(array of struct vdma_dev*)"]
            ARR -- "[0]" --> S0["FONT instance<br/>fd, init_count=2, bufs..."]
            ARR -- "[1]" --> S1["SCALER0 instance<br/>fd, init_count=1"]
            ARR -- "[2]" --> NULL2["[2] = NULL<br/>(SCALER1 미사용)"]
            ARR -- "[3]" --> S3["JPEGENC instance<br/>fd, init_count=1"]
            ARR -- "[4]" --> NULL4["[4] = NULL<br/>(JPEGDEC 미사용)"]
        end

        T1[Thread 1] -.uses.-> ARR
        T2[Thread 2] -.uses.-> ARR
    end

    S0 -. ioctl/mmap .-> KFD0["/dev/enx_vdma_font fd"]
    S1 -. ioctl/mmap .-> KFD1["/dev/enx_vdma_dz0 fd"]
    S3 -. ioctl/mmap .-> KFD3["/dev/enx_vdma_jenc fd"]

    style LibState fill:#e8f5e9,stroke:#2e7d32
```

---

## 6. fork() 안전성 메커니즘

```mermaid
graph TB
    PARENT["부모 process<br/>g_dev_inst[FONT] = inst (fd=10, fork_gen=0)<br/>g_fork_generation = 0"]

    PARENT -- "fork()" --> CHILD["자식 process (fork 직후)"]

    CHILD -- "atfork_child 자동 호출" --> RESET["g_dev_inst[i] 모두 fd=-1<br/>g_fork_generation++ = 1"]

    RESET -- "자식이 alloc 시도" --> CHECK["dev_check() 호출"]
    CHECK -- "fork_gen 다름<br/>(0 ≠ 1)" --> FAIL["-EOWNERDEAD 반환"]

    FAIL -- "자식이 vdma_open(FONT) 재호출" --> NEW["자식 자기만의 fd<br/>fork_gen=1 기록"]
    NEW -- "이후 alloc OK" --> OK["정상 동작"]

    style PARENT fill:#bbdefb,stroke:#1565c0
    style CHILD fill:#ffccbc,stroke:#d84315
    style OK fill:#c8e6c9,stroke:#2e7d32
```

---

## 7. SYNC vs ASYNC submit 경로

```mermaid
graph TB
    USER["user ioctl(VDMAIOSET_*_SUBMIT)"]

    USER --> DISP["enx_vdma_submit dispatcher<br/>(공통 validation)"]

    DISP -- "ENX_SUBMIT_SYNC" --> SYNC["enx_vdma_submit_sync<br/>━━━━━━━━━━━━━━━━<br/>1. fill_job_buf<br/>2. mutex_lock_killable<br/>3. ops->hw_run_once 직접 호출<br/>4. cleanup"]

    DISP -- "ENX_SUBMIT_ASYNC" --> ASYNC["enx_vdma_submit_async<br/>━━━━━━━━━━━━━━━━<br/>1. kmemdup blits<br/>2. xa_alloc(job_id)<br/>3. queue_work<br/>4. return immediately"]

    SYNC -- "직접 실행" --> HW["HW operation"]
    HW -- "IRQ wake" --> SYNC

    ASYNC -- "queue_work" --> WORKER["worker thread<br/>vdma_job_worker"]
    WORKER -- "mutex_lock" --> WHW["ops->hw_run_once"]
    WHW -- "IRQ wake" --> WORKER
    WORKER -- "vdma_job_finish<br/>wake_up + wake_up_interruptible" --> WAITERS["user wait /<br/>poll waiters"]

    style SYNC fill:#e8f5e9,stroke:#2e7d32
    style ASYNC fill:#fff4e6,stroke:#cc6600
```

---

## 8. 파일 트리

```
skeleton/
├── enx-vdma-core.c          ◄ ROOT (core 구현)
├── enx-vdma.h               ◄ core internal header
├── Makefile                 ◄ DRV_MODULES 변수로 통합 빌드
│
├── include-uapi/            ◄ UAPI 헤더 (kernel + user 공통)
│   ├── enx-vdma-uapi.h
│   ├── en683-font-uapi.h
│   ├── en683-dz-uapi.h
│   ├── en683-jpegenc-uapi.h
│   └── en683-jpegdec-uapi.h
│
├── font-drv/                ◄ DRV: FONT
├── dz-drv/                  ◄ DRV: DZ (multi-channel)
├── jpegenc-drv/             ◄ DRV: JPEG ENC
├── jpegdec-drv/             ◄ DRV: JPEG DEC
│
└── userapp/
    ├── lib/                 ◄ 통합 userspace lib
    │   ├── libvdma.h
    │   ├── libvdma.c
    │   └── Makefile
    ├── font/                ◄ legacy per-device lib + example
    ├── scaler/
    ├── jpegenc/
    └── jpegdec/
```

---

## 렌더링 도구

### Mermaid (이 문서)
| 도구 | 사용법 |
|---|---|
| VSCode | `Markdown Preview Enhanced` 확장 또는 `Ctrl+Shift+V` |
| GitHub / GitLab | README/Wiki 에 그대로 push 시 자동 렌더 |
| mermaid.live | https://mermaid.live/ — 코드 붙여넣기 |
| mmdc CLI | `npm install -g @mermaid-js/mermaid-cli` 후 `mmdc -i ARCHITECTURE.md -o out.png` |

### Graphviz (.dot 파일)

같은 디렉토리에 `.dot` 파일 3 종 + 미리 렌더된 PNG/SVG 가 있음:

| 파일 | 내용 | 산출물 |
|---|---|---|
| `architecture.dot` | 전체 아키텍처 (Core/Driver/User/HW) | `architecture.png`, `.svg` |
| `architecture-flow.dot` | Backing/Buf 공유 모델 (cross-device) | `architecture-flow.png`, `.svg` |
| `architecture-submit.dot` | SUBMIT path (SYNC vs ASYNC) | `architecture-submit.png`, `.svg` |

재 렌더링:
```sh
cd ref_md/
dot -Tpng architecture.dot         -o architecture.png
dot -Tsvg architecture-flow.dot    -o architecture-flow.svg
dot -Tpdf architecture-submit.dot  -o architecture-submit.pdf
```

`graphviz` 패키지 필요 (`sudo apt install graphviz`).
