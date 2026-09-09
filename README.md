# QEMU Virtual Peripheral & Linux Driver Project

QEMU ARM64 `virt` 머신에 Custom MMIO Peripheral을 직접 구현하고, Device Tree와 Linux Platform Driver를 통해 Guest Linux에서 장치를 인식·제어하는 과정을 단계적으로 확장하는 임베디드 Linux 프로젝트입니다.

기존 Linux Device Driver 프로젝트에서 `ioremap()`을 통해 실제 FPGA Peripheral을 제어했던 경험을 바탕으로, 이번 프로젝트에서는 반대로 **QEMU 내부에서 가상 Peripheral 자체를 구현**하여 Device Model부터 Driver, User Space Interface까지 전체 흐름을 직접 구성하는 것을 목표로 합니다.

---

## Project Goals

- QEMU Device Model과 QOM 구조 이해
- SysBusDevice 기반 Custom MMIO Peripheral 구현
- Register Map 및 MMIO read/write 동작 설계
- Device Tree를 통한 하드웨어 Resource 기술
- Linux Platform Driver 구현
- User Space와 Kernel Driver 간 인터페이스 구성
- Interrupt 기반 장치 동작 확장
- Buildroot를 이용한 임베디드 Linux 이미지 통합

```text
User Application
      ↓
Linux Device Driver
      ↓
Device Tree / Platform Resource
      ↓
MMIO / Interrupt
      ↓
QEMU Virtual Peripheral
```

---

## Tech Stack

- **C**
- **C++** *(User-space control program 예정)*
- **Linux**
- **QEMU**
- **Buildroot**
- **Device Tree**
- **Linux Kernel / Platform Driver**

---

## Development Roadmap

| Phase | 내용 | 상태 |
|---|---|---|
| Phase 1 | QEMU Custom MMIO Peripheral | ✅ 완료 |
| Phase 2 | Device Tree + Linux Platform Driver | ✅ 완료 |
| Phase 3 | User-space Device Control Interface | 예정 |
| Phase 4 | Hardware Interrupt | 예정 |
| Phase 5 | Buildroot / BSP Integration | 예정 |

---

# Phase 1 - QEMU Custom MMIO Peripheral

QEMU ARM64 `virt` 머신에 SysBusDevice 기반 Custom MMIO Peripheral인 `vperiph`를 구현했습니다.

### Highlights

- QOM 기반 Custom Device Type 정의
- `SysBusDevice` 기반 Peripheral 구현
- CONTROL / STATUS / DATA Register 설계
- `MemoryRegion` 및 `MemoryRegionOps` 구현
- MMIO read/write callback 작성
- Guest Physical Address `0x090d0000`에 4KB MMIO Region 배치
- Guest Linux에서 `devmem`으로 Register 동작 검증

검증 결과:

```text
DATA Write    : 10
CONTROL Write : 1
DATA Read     : 20
STATUS Read   : 1
```

**[Phase 1 상세 문서](docs/phase-1.md)**

---

# Phase 2 - Device Tree & Linux Platform Driver

Phase 1에서 만든 `vperiph`를 Device Tree에 기술하고, Guest Linux에서 Platform Device로 인식시킨 뒤 Linux Platform Driver와 연결했습니다.

### Highlights

- `vperiph@90d0000` Device Tree node 생성
- `compatible = "qemu,vperiph"` 정의
- `reg = 0x090d0000 / 0x1000` Resource 기술
- `/proc/device-tree`에서 node 및 Resource 검증
- ARM64 Linux Platform Driver 구현
- `of_match_table`을 통한 Device Tree ↔ Driver 매칭
- `probe()` 호출 확인
- `devm_platform_ioremap_resource()`를 통한 MMIO Resource Mapping
- `readl()` / `writel()`로 Register 제어
- Driver를 통한 `DATA=20`, `STATUS=1` 동작 검증

```text
QEMU vperiph
        ↓
Device Tree
compatible / reg
        ↓
Linux platform_device
        ↓
Platform Driver
        ↓
probe()
        ↓
MMIO Resource Mapping
        ↓
readl() / writel()
        ↓
QEMU Device Callback
```

검증 로그:

```text
vperiph 90d0000.vperiph: vperiph probe called
vperiph 90d0000.vperiph: DATA=20 STATUS=1
```

**[Phase 2 상세 문서](docs/phase-2.md)**

---

## Repository Structure

```text
qemu-virtual-peripheral/
├── README.md
├── docs/
│   ├── phase-1.md
│   └── phase-2.md
├── patches/
│   └── phase-1-qemu-vperiph.patch
└── .gitignore
```

QEMU와 Buildroot 원본 소스는 로컬 개발환경에서만 사용하며, 저장소에는 프로젝트 문서와 직접 수정한 QEMU 변경사항을 patch 형태로 관리합니다.
