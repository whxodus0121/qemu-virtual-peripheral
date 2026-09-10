# QEMU Virtual Peripheral Bring-up

QEMU ARM64 `virt` Machine에 Custom MMIO Peripheral을 구현하고, Device Tree, Linux Platform Driver, Character Device, Hardware Interrupt, Buildroot까지 연결한 Virtual Peripheral Bring-up 프로젝트이다.

단순히 Linux Driver에서 Register를 접근하는 것에 그치지 않고, Driver가 접근하는 Virtual Hardware를 QEMU 내부에서 직접 구현한 뒤 Guest Linux가 해당 Device를 인식하고 User Space에서 사용할 수 있도록 전체 경로를 구성했다.

```text
C++ User-space CLI
        ↓
/dev/vperiph
        ↓
Linux Platform Driver
        ↓
MMIO
        ↓
QEMU Custom Peripheral
        ↓
Hardware IRQ
        ↓
ARM GIC
        ↓
Linux IRQ Handler
```

최종적으로 Driver와 CLI를 Buildroot에 통합하여 Buildroot가 생성한 Root Filesystem에서 Driver가 부팅 시 자동 로드되고 CLI를 바로 실행할 수 있도록 구성했다.

---

## 프로젝트 목표

이 프로젝트의 핵심 목표는 다음 질문을 직접 구현하며 확인하는 것이다.

```text
QEMU에서 Peripheral은 어떻게 구현되는가?

Guest Linux는 Virtual Hardware의
주소와 Interrupt 정보를 어떻게 전달받는가?

Linux Platform Driver는 Device Tree Resource를
어떻게 MMIO와 IRQ로 연결하는가?

User Space는 Hardware의 Physical Address를
직접 알지 않고 어떻게 Driver를 사용하는가?

Level-triggered Interrupt는
어떻게 Raise / Handle / ACK / Clear 되는가?

Driver와 Application을
Embedded Linux Image에 어떻게 통합하는가?
```

이를 위해 하나의 단순한 `vperiph` Device를 대상으로 Hardware Model부터 Linux System Integration까지 단계적으로 확장했다.

---

## 전체 시스템 구조

```text
Windows 11
    ↓
WSL2 Ubuntu
    │
    ├── QEMU Source
    │    │
    │    └── ARM64 virt Machine
    │         ├── ARM CPU
    │         ├── GIC
    │         └── vperiph
    │              ├── CONTROL
    │              ├── STATUS
    │              ├── DATA
    │              ├── IRQ_ACK
    │              └── IRQ Output
    │
    └── Buildroot
         ├── ARM64 Linux Kernel
         ├── Device Tree
         ├── vperiph_driver.ko
         ├── vperiph_cli
         └── Root Filesystem

Guest Linux
    │
    ├── Device Tree
    │    ├── compatible = "qemu,vperiph"
    │    ├── reg
    │    └── interrupts
    │
    ├── Linux Platform Driver
    │    ├── MMIO Mapping
    │    ├── Character Device
    │    └── IRQ Handler
    │
    └── /usr/bin/vperiph_cli
```

---

## vperiph Register Map

Base Address:

```text
0x090d0000
```

MMIO Size:

```text
0x1000
```

| Offset | Register | Access | 의미 |
|---:|---|---|---|
| `0x00` | CONTROL | R/W | `1`을 쓰면 연산 시작 |
| `0x04` | STATUS | R | `0 = IDLE`, `1 = DONE` |
| `0x08` | DATA | R/W | 입력 및 연산 결과 |
| `0x0C` | IRQ_ACK | W | `1`을 쓰면 IRQ Clear |

기본 동작:

```text
DATA = 10
CONTROL = 1

        ↓

DATA = 20
STATUS = 1
IRQ = HIGH

        ↓

Linux IRQ Handler
IRQ_ACK = 1

        ↓

IRQ = LOW
```

연산 자체는 단순하게 유지하고, 프로젝트의 초점은 Register 동작보다 QEMU Device Model과 Linux Bring-up 경로에 두었다.

---

## 개발 단계

| Phase | 내용 | 상태 |
|---|---|---|
| Phase 1 | QEMU Custom MMIO Peripheral | ✅ |
| Phase 2 | Device Tree + Linux Platform Driver | ✅ |
| Phase 3 | User-space Device Control Interface | ✅ |
| Phase 4 | Hardware Interrupt | ✅ |
| Phase 5 | Buildroot / BSP Integration | ✅ |

### Phase 1 - QEMU Custom MMIO Peripheral

QOM / SysBusDevice 기반 `vperiph`를 구현하고 ARM `virt` Machine의 Guest Physical Address에 MMIO Region을 배치했다.

```text
Guest MMIO Access
→ QEMU MemoryRegion
→ edu_mmio_read() / edu_mmio_write()
→ Virtual Register State
```

`devmem`을 이용해 CONTROL / STATUS / DATA Register 동작을 검증했다.

상세 내용: `docs/phase-1.md`

### Phase 2 - Device Tree + Linux Platform Driver

QEMU가 생성하는 Device Tree에 `vperiph` Node를 추가했다.

```text
compatible = "qemu,vperiph"
reg        = MMIO Base / Size
```

Linux Platform Driver에서는 `compatible` Matching을 통해 `probe()`가 호출되고 `devm_platform_ioremap_resource()`를 이용해 MMIO Resource를 Mapping했다.

상세 내용: `docs/phase-2.md`

### Phase 3 - User-space Device Control Interface

`miscdevice`를 이용해 `/dev/vperiph`를 생성했다.

```text
write() → DATA 입력
ioctl() → CONTROL START
read()  → DATA / STATUS 결과 반환
```

C++ CLI를 구현하여 User Space가 Physical Address를 직접 사용하지 않고 Device File만으로 `vperiph`를 제어하도록 구성했다.

상세 내용: `docs/phase-3.md`

### Phase 4 - Hardware Interrupt

`vperiph`에 IRQ Output을 추가하고 ARM `virt` Machine의 GIC SPI에 연결했다.

```text
vperiph
→ GIC SPI 11
→ ARM CPU
→ Linux IRQ subsystem
→ vperiph_irq_handler()
```

Device Tree에도 SPI 11, Level High Interrupt Resource를 추가했다.

초기에는 IRQ를 HIGH로 Raise한 뒤 Clear하지 않아 Level-triggered Interrupt가 반복 발생했다. 이를 해결하기 위해 `REG_IRQ_ACK = 0x0C`를 추가하고 Driver Handler에서 ACK를 기록하여 Device가 IRQ Line을 LOW로 내리도록 수정했다.

```text
Raise → Handle → ACK → Clear
```

상세 내용: `docs/phase-4.md`

### Phase 5 - Buildroot / BSP Integration

Driver와 CLI를 Buildroot `BR2_EXTERNAL` Tree에 Package로 등록했다.

```text
vperiph-cli
→ generic-package

vperiph-driver
→ kernel-module + generic-package
```

RootFS Overlay에 Init Script를 추가하여 부팅 시 Driver가 자동 로드되도록 구성했다.

```text
Buildroot make
→ Driver / CLI Build
→ RootFS 설치
→ QEMU Boot
→ Driver 자동 Load
→ /dev/vperiph 생성
→ CLI 실행
```

프로젝트용 `vperiph_defconfig`를 저장하고 Clean Build 이후 동일한 동작을 다시 확인했다.

상세 내용: `docs/phase-5.md`

---

## Repository Structure

```text
qemu-virtual-peripheral/
├── README.md
├── .gitignore
│
├── docs/
│   ├── phase-1.md
│   ├── phase-2.md
│   ├── phase-3.md
│   ├── phase-4.md
│   └── phase-5.md
│
├── driver/
│   ├── Makefile
│   └── vperiph_driver.c
│
├── user/
│   └── vperiph_cli.cpp
│
├── br2-external/
│   ├── external.desc
│   ├── external.mk
│   ├── Config.in
│   ├── configs/
│   │   └── vperiph_defconfig
│   ├── package/
│   │   ├── vperiph-cli/
│   │   │   ├── Config.in
│   │   │   └── vperiph-cli.mk
│   │   └── vperiph-driver/
│   │       ├── Config.in
│   │       └── vperiph-driver.mk
│   └── board/
│       └── vperiph/
│           └── rootfs-overlay/
│               └── etc/init.d/
│                   └── S50vperiph
│
└── patches/
    ├── phase-1-qemu-vperiph.patch
    ├── phase-2-device-tree.patch
    ├── phase-3-user-interface.patch
    └── phase-4-hardware-interrupt.patch
```

QEMU와 Buildroot Source Tree 및 Build Output은 Repository에 포함하지 않는다.

---

## Buildroot Integration

프로젝트 Configuration 적용:

```bash
cd buildroot

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

make BR2_EXTERNAL=../br2-external vperiph_defconfig
make
```

Build가 완료되면 주요 결과물은 다음 위치에 생성된다.

```text
output/images/Image
output/images/rootfs.ext4
```

Target RootFS에는 다음 항목이 포함된다.

```text
/usr/bin/vperiph_cli
/lib/modules/<kernel-version>/updates/vperiph_driver.ko
/etc/init.d/S50vperiph
```

---

## QEMU 실행

QEMU Source에는 `patches/`에 기록된 QEMU 변경사항을 적용한 뒤 ARM64 System Emulator를 Build해야 한다.

현재 프로젝트에서 사용한 실행 형태:

```bash
./qemu-system-aarch64 \
  -M virt \
  -cpu cortex-a53 \
  -nographic \
  -smp 1 \
  -kernel /path/to/buildroot/output/images/Image \
  -append "rootwait root=/dev/vda console=ttyAMA0" \
  -drive file=/path/to/buildroot/output/images/rootfs.ext4,if=none,format=raw,id=hd0 \
  -device virtio-blk-device,drive=hd0 \
  -net none
```

QEMU Source Tree 자체는 Repository에 포함하지 않으며, Patch는 프로젝트에서 사용한 QEMU Source 기준 변경사항을 기록한다.

---

## 실행 결과

부팅 과정에서 Driver가 자동으로 Load된다.

```text
Loading vperiph driver...
vperiph_driver: loading out-of-tree module taints kernel.
vperiph 90d0000.vperiph: vperiph probe called
vperiph 90d0000.vperiph: registered /dev/vperiph
```

Guest Linux 확인:

```bash
lsmod
ls -l /dev/vperiph
which vperiph_cli
```

CLI 실행:

```bash
vperiph_cli 10
```

결과:

```text
vperiph 90d0000.vperiph: vperiph interrupt received
input=10
result=20
status=1
```

최종 검증 경로:

```text
C++ CLI
    ↓
Character Device
    ↓
Linux Platform Driver
    ↓
MMIO
    ↓
QEMU vperiph
    ↓
IRQ
    ↓
ARM GIC
    ↓
Linux IRQ Handler
    ↓
IRQ ACK
```

---

## 구현을 통해 확인한 내용

이 프로젝트를 통해 다음 요소를 하나의 경로에서 직접 연결했다.

```text
QEMU QOM / SysBusDevice
MemoryRegion / MMIO
ARM virt Memory Map
Device Tree
Platform Driver
devm_platform_ioremap_resource()
miscdevice / file_operations
copy_from_user() / copy_to_user()
C++ User-space Application
GIC SPI
Linux IRQ Handler
Level-triggered IRQ ACK / Clear
Buildroot Package
Out-of-tree Kernel Module
RootFS Overlay
Boot-time Module Loading
Buildroot defconfig
```

특히 Driver만 구현한 것이 아니라 Driver가 접근하는 Virtual Peripheral의 Register와 IRQ 동작을 QEMU 쪽에서 직접 구현하고, Device Tree를 통해 Linux에 Resource를 전달한 뒤 User Space까지 연결했다.

---

## 프로젝트 범위

이 프로젝트는 실제 상용 Board의 전체 BSP를 구현한 프로젝트는 아니다.

QEMU ARM64 `virt` Platform과 직접 구현한 Virtual Peripheral을 대상으로:

```text
Virtual Hardware Bring-up
Linux Driver Integration
Interrupt Integration
Buildroot System Integration
```

과정을 구현하고 검증하는 데 목적을 두었다.

따라서 프로젝트의 성격은 **QEMU 기반 Virtual Peripheral Bring-up 및 Buildroot Embedded Linux Integration**으로 정의한다.
