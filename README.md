# QEMU Virtual Peripheral & Linux Driver Project

QEMU ARM64 `virt` 머신에 Custom MMIO Peripheral을 직접 구현하고, Device Tree와 Linux Platform Driver를 통해 Guest Linux에서 장치를 인식·제어하는 과정을 단계적으로 확장하는 임베디드 Linux 프로젝트입니다.

기존 Linux Device Driver 프로젝트에서 `ioremap()`을 통해 실제 FPGA Peripheral을 제어했던 경험을 바탕으로, 이번 프로젝트에서는 반대로 **QEMU 내부에서 가상 Peripheral 자체를 구현**하여 Device Model부터 Driver까지 전체 흐름을 직접 구성하는 것을 목표로 합니다.

---

## Project Goals

- QEMU Device Model과 QOM 구조 이해
- SysBusDevice 기반 Custom MMIO Peripheral 구현
- Register Map 및 MMIO read/write 동작 설계
- Device Tree를 통한 하드웨어 리소스 기술
- Linux Platform Driver 구현
- User Space와 Kernel Driver 간 인터페이스 구성
- Interrupt 기반 장치 동작 확장
- Buildroot를 이용한 임베디드 Linux 이미지 통합

최종적으로 다음 흐름을 하나의 프로젝트에서 구현하는 것이 목표입니다.

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

## System Architecture

```text
Windows 11
    ↓
WSL2 Ubuntu
    │
    ├── Buildroot
    │    ├── ARM64 Linux Kernel
    │    └── Root Filesystem
    │
    └── QEMU Source
         ↓
    qemu-system-aarch64
         ↓
    ARM64 virt Machine
         │
         ├── CPU
         ├── RAM
         ├── UART / GPIO / virtio
         └── vperiph
              ├── CONTROL
              ├── STATUS
              └── DATA
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
| Phase 2 | Device Tree Integration | 예정 |
| Phase 3 | Linux Platform Driver | 예정 |
| Phase 4 | User-space Device Control Interface | 예정 |
| Phase 5 | Hardware Interrupt | 예정 |
| Phase 6 | Buildroot / BSP Integration | 예정 |

---

# Phase 1 - QEMU Custom MMIO Peripheral

QEMU ARM64 `virt` 머신에 SysBusDevice 기반 Custom MMIO Peripheral인 `vperiph`를 구현했습니다.

## Register Map

| Offset | Register | Access | Description |
|---:|---|---|---|
| `0x00` | CONTROL | R/W | `1`을 쓰면 연산 실행 |
| `0x04` | STATUS | R | `0 = IDLE`, `1 = DONE` |
| `0x08` | DATA | R/W | 입력 및 결과 데이터 |

MMIO 영역은 Guest Physical Address의 다음 위치에 배치했습니다.

```text
Base Address : 0x090d0000
Size         : 0x00001000
Range        : 0x090d0000 ~ 0x090d0fff
```

QEMU Monitor의 `info mtree`를 통해 실제 주소 공간에 장치가 배치된 것을 확인했습니다.

```text
00000000090d0000-00000000090d0fff (prio 0, i/o): vperiph
```

---

## Phase 1 Implementation

- QOM 기반 Custom Device Type 정의
- `SysBusDevice` 기반 Peripheral 구현
- CONTROL / STATUS / DATA Register State 구성
- `MemoryRegion` 및 `MemoryRegionOps` 구현
- MMIO read/write callback 작성
- QEMU ARM `virt` Machine의 Address Map에 장치 추가
- Guest Linux에서 `devmem`을 이용해 실제 MMIO 동작 검증

장치 동작은 다음과 같이 설계했습니다.

```text
DATA = 10
CONTROL = 1
    ↓
DATA = 20
STATUS = 1
```

Guest Linux에서 직접 Register에 접근하여 다음 결과를 확인했습니다.

```text
DATA Write      : 10
CONTROL Write   : 1
DATA Read       : 20
STATUS Read     : 1
```

또한 STATUS Register의 Read-Only 동작과 새로운 DATA 입력 시 STATUS가 다시 IDLE 상태로 전환되는 상태 변화도 검증했습니다.

자세한 구현 과정은 아래 문서에서 확인할 수 있습니다.

**[Phase 1 - QEMU Custom MMIO Peripheral](docs/phase-1.md)**

---

## Phase 1 Data Flow

```text
Guest Linux
    │
    │ devmem / MMIO Access
    ▼
ARM64 CPU
    │
    ▼
QEMU Memory System
    │
    │ 0x090d0000 ~ 0x090d0fff
    ▼
vperiph MemoryRegion
    │
    ├── read  → edu_mmio_read()
    └── write → edu_mmio_write()
                  │
                  ▼
          CONTROL / STATUS / DATA
```

Phase 1에서는 Device Tree나 Linux Driver 없이 Guest가 물리주소를 직접 알고 접근했습니다.

다음 Phase에서는 Device Tree를 추가하여 Guest Linux가 하드코딩된 주소가 아니라 하드웨어 기술 정보를 통해 `vperiph`의 MMIO Resource를 인식하도록 확장합니다.

---

## Repository Structure

```text
qemu-linux/
├── README.md
├── docs/
│   └── phase-1.md
├── qemu/
└── buildroot/
```

개발이 진행됨에 따라 각 Phase의 기술 문서를 `docs/` 아래에 추가할 예정입니다.
