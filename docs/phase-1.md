# Phase 1 - QEMU Custom MMIO Peripheral

## 1. 목표

QEMU ARM64 `virt` 머신에 직접 Custom MMIO Peripheral을 구현하고, Guest Linux에서 실제 MMIO read/write가 동작하는지 검증한다.

이번 Phase의 목표는 단순히 QEMU를 실행하는 것이 아니라 다음 흐름을 직접 구현하고 이해하는 것이다.

```text
Guest Linux
    ↓
ARM64 CPU MMIO Access
    ↓
QEMU Memory System
    ↓
Custom MemoryRegion
    ↓
read/write Callback
    ↓
Virtual Peripheral Register
```

최종적으로 다음 기능을 갖는 `vperiph` 장치를 구현한다.

- QOM 기반 Custom Device Type 정의
- SysBusDevice 기반 MMIO Peripheral 구현
- CONTROL / STATUS / DATA Register 정의
- MMIO read/write callback 구현
- ARM `virt` 머신의 Guest Physical Address에 장치 배치
- Guest Linux에서 직접 MMIO 접근을 통한 동작 검증

---

## 2. 배경 개념

### 2.1 QEMU Device Model

QEMU는 CPU와 메모리뿐 아니라 UART, RTC, GPIO 등의 하드웨어 장치도 소프트웨어로 에뮬레이션한다.

이번 프로젝트에서는 실제 Peripheral 대신 QEMU 내부에 C 코드로 가상 Peripheral을 구현한다.

| 실제 하드웨어 | QEMU Device Model |
|---|---|
| Peripheral 장치 | QOM Device |
| 장치 인스턴스 | DeviceState |
| SoC Peripheral | SysBusDevice |
| Hardware Register | State 구조체의 필드 |
| MMIO 주소 영역 | MemoryRegion |
| Register Read | MemoryRegion read callback |
| Register Write | MemoryRegion write callback |
| 장치 초기화 | instance_init / realize |

### 2.2 QOM

QOM(QEMU Object Model)은 C 언어에서 QEMU의 객체 타입과 상속 구조를 표현하기 위한 Object Model이다.

이번 장치의 상속 구조는 다음과 같다.

```text
Object
  ↓
DeviceState
  ↓
SysBusDevice
  ↓
EduMmioState
```

`EduMmioState` 구조체의 첫 번째 멤버로 `SysBusDevice`를 포함시켜 QOM의 상속 관계를 구성했다.

```c
struct EduMmioState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    uint32_t control;
    uint32_t status;
    uint32_t data;
};
```

TypeInfo에서는 부모 타입을 `TYPE_SYS_BUS_DEVICE`로 지정했다.

```c
static const TypeInfo edu_mmio_info = {
    .name = TYPE_EDU_MMIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(EduMmioState),
    .instance_init = edu_mmio_init,
    .class_init = edu_mmio_class_init,
};
```

### 2.3 MMIO

MMIO(Memory-Mapped I/O)는 Peripheral의 Register를 CPU의 주소 공간에 배치하고 일반 메모리처럼 read/write하여 장치를 제어하는 방식이다.

이번 장치는 Guest Physical Address:

```text
0x090d0000 ~ 0x090d0fff
```

에 4KB 크기의 MMIO 영역을 배치했다.

Register는 다음과 같이 정의했다.

| Offset | Register | 권한 | 의미 |
|---:|---|---|---|
| `0x00` | CONTROL | R/W | `1`을 쓰면 연산 수행 |
| `0x04` | STATUS | R | `0 = IDLE`, `1 = DONE` |
| `0x08` | DATA | R/W | 입력 및 결과 데이터 |

실제 Guest Physical Address는 다음과 같다.

```text
CONTROL = 0x090d0000
STATUS  = 0x090d0004
DATA    = 0x090d0008
```

---

## 3. 왜 필요한가

기존 FPGA 프로젝트에서는 Linux Kernel Driver에서 `ioremap()`을 이용해 실제 FPGA Peripheral의 물리주소를 매핑하고 `outw()`를 통해 장치를 제어했다.

이번에는 반대로 QEMU 내부에서 하드웨어 자체를 소프트웨어로 구현한다.

기존 프로젝트가:

```text
Linux Driver
    ↓
ioremap()
    ↓
실제 FPGA MMIO
```

였다면 이번 프로젝트는:

```text
Guest Linux
    ↓
MMIO Access
    ↓
QEMU MemoryRegion
    ↓
직접 구현한 Virtual Peripheral
```

구조이다.

이를 통해 Linux Driver 관점뿐 아니라, Driver가 접근하는 Peripheral의 Register와 MMIO 동작을 가상 하드웨어 쪽에서 직접 설계하고 구현하는 경험을 얻는 것을 목표로 했다.

---

## 4. 시스템 구조

전체 개발 및 실행 환경은 다음과 같다.

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
         ├── UART
         ├── GPIO
         └── vperiph
              ├── CONTROL
              ├── STATUS
              └── DATA
```

`vperiph`는 ARM `virt` 머신의 다음 주소에 배치했다.

```text
Base Address : 0x090d0000
Size         : 0x00001000
```

QEMU Monitor의 `info mtree`를 통해 실제 System Memory에 다음과 같이 매핑된 것을 확인했다.

```text
00000000090d0000-00000000090d0fff (prio 0, i/o): vperiph
```

---

## 5. 구현 과정

### 5.1 Custom QOM Device 정의

외부 Device Type 이름은 기존 QEMU의 `edu` PCI 장치와 혼동되지 않도록 `vperiph`로 정의했다.

```c
#define TYPE_EDU_MMIO "vperiph"

OBJECT_DECLARE_SIMPLE_TYPE(EduMmioState, EDU_MMIO)
```

Device State에는 MMIO 영역과 Register 상태를 저장했다.

```c
struct EduMmioState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    uint32_t control;
    uint32_t status;
    uint32_t data;
};
```

### 5.2 Register Map 정의

```c
#define REG_CONTROL 0x00
#define REG_STATUS  0x04
#define REG_DATA    0x08

#define EDU_MMIO_SIZE 0x1000
```

장치 동작은 단순한 연산 장치 형태로 설계했다.

```text
DATA = 10
CONTROL = 1

        ↓

DATA = 20
STATUS = 1
```

DATA를 새로 쓰면 이전 연산 결과를 초기화하기 위해 STATUS를 다시 `0`으로 변경한다.

### 5.3 MMIO Read Callback

Guest CPU가 vperiph의 MMIO 영역을 읽으면 QEMU가 `edu_mmio_read()`를 호출한다.

```c
static uint64_t edu_mmio_read(void *opaque, hwaddr addr,
                              unsigned size)
{
    EduMmioState *s = opaque;

    switch (addr) {
    case REG_CONTROL:
        return s->control;

    case REG_STATUS:
        return s->status;

    case REG_DATA:
        return s->data;

    default:
        return 0;
    }
}
```

callback으로 전달되는 `addr`은 Guest의 전체 Physical Address가 아니라 해당 MemoryRegion 내부의 offset이다.

예를 들어:

```text
Guest Access Address = 0x090d0008
MemoryRegion Base    = 0x090d0000

Callback addr
= 0x090d0008 - 0x090d0000
= 0x08
```

따라서 `REG_DATA`에 대한 접근으로 처리된다.

### 5.4 MMIO Write Callback

```c
static void edu_mmio_write(void *opaque, hwaddr addr,
                           uint64_t value, unsigned size)
{
    EduMmioState *s = opaque;

    switch (addr) {
    case REG_CONTROL:
        s->control = value;

        if (value == 1) {
            s->data *= 2;
            s->status = 1;
        }
        break;

    case REG_STATUS:
        /* STATUS is read-only */
        break;

    case REG_DATA:
        s->data = value;
        s->status = 0;
        break;
    }
}
```

CONTROL에 `1`이 쓰이면 장치가 DATA에 저장된 값을 두 배로 변경하고 STATUS를 DONE 상태인 `1`로 변경한다.

STATUS는 Read Only Register로 정의하여 write 요청을 무시하도록 구현했다.

### 5.5 MemoryRegionOps 연결

MMIO 영역에서 어떤 callback을 사용할 것인지 `MemoryRegionOps`로 정의했다.

```c
static const MemoryRegionOps edu_mmio_ops = {
    .read = edu_mmio_read,
    .write = edu_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,

    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};
```

모든 Register를 `uint32_t`로 정의했기 때문에 정상 Register 접근 크기도 32bit로 제한했다.

### 5.6 MMIO MemoryRegion 초기화

```c
static void edu_mmio_init(Object *obj)
{
    EduMmioState *s = EDU_MMIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio,
                          OBJECT(s),
                          &edu_mmio_ops,
                          s,
                          TYPE_EDU_MMIO,
                          EDU_MMIO_SIZE);

    sysbus_init_mmio(sbd, &s->mmio);
}
```

`memory_region_init_io()`를 통해 4KB 크기의 MMIO 영역과 read/write callback을 연결했다.

`sysbus_init_mmio()`를 통해 생성한 MemoryRegion을 SysBusDevice가 제공하는 MMIO Region으로 등록했다. 이 시점에서는 MMIO Region의 존재만 정의되며 실제 Guest Physical Address는 아직 할당되지 않는다.

### 5.7 ARM virt Machine에 Device 배치

ARM `virt` 머신의 기존 memory map을 확인한 뒤 다른 Peripheral과 충돌하지 않는 영역으로 다음 주소를 선택했다.

```text
0x090d0000 ~ 0x090d0fff
```

`base_memmap`에 다음 항목을 추가했다.

```c
[VIRT_VPERIPH] = {
    0x090d0000,
    0x00001000
},
```

이후 `virt` 머신 초기화 과정에서 실제 Device instance를 생성했다.

```c
static void create_vperiph(VirtMachineState *vms)
{
    DeviceState *dev;
    SysBusDevice *sbd;

    dev = qdev_new(TYPE_EDU_MMIO);
    sbd = SYS_BUS_DEVICE(dev);

    sysbus_realize_and_unref(sbd, &error_fatal);

    sysbus_mmio_map(
        sbd,
        0,
        vms->memmap[VIRT_VPERIPH].base
    );
}
```

장치 생성 과정은 다음과 같다.

```text
qdev_new()
    ↓
QOM에서 vperiph Type 검색
    ↓
EduMmioState instance 생성
    ↓
edu_mmio_init()
    ↓
MemoryRegion 초기화
    ↓
Device realize
    ↓
sysbus_mmio_map()
    ↓
0x090d0000에 MMIO Region 배치
```

---

## 6. 핵심 코드

핵심 흐름을 단순화하면 다음과 같다.

```c
#define REG_CONTROL 0x00
#define REG_STATUS  0x04
#define REG_DATA    0x08

struct EduMmioState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;

    uint32_t control;
    uint32_t status;
    uint32_t data;
};

static uint64_t edu_mmio_read(void *opaque,
                              hwaddr addr,
                              unsigned size)
{
    EduMmioState *s = opaque;

    switch (addr) {
    case REG_CONTROL:
        return s->control;
    case REG_STATUS:
        return s->status;
    case REG_DATA:
        return s->data;
    default:
        return 0;
    }
}

static void edu_mmio_write(void *opaque,
                           hwaddr addr,
                           uint64_t value,
                           unsigned size)
{
    EduMmioState *s = opaque;

    switch (addr) {
    case REG_CONTROL:
        s->control = value;

        if (value == 1) {
            s->data *= 2;
            s->status = 1;
        }
        break;

    case REG_STATUS:
        break;

    case REG_DATA:
        s->data = value;
        s->status = 0;
        break;
    }
}
```

Guest에서 DATA Register에 값을 저장한 뒤 CONTROL Register에 START 명령을 쓰면 QEMU 내부에서 Peripheral 동작이 수행된다.

```text
Guest: DATA = 10
          ↓
QEMU: s->data = 10

Guest: CONTROL = 1
          ↓
QEMU:
s->data *= 2
s->status = 1

Guest: DATA read
          ↓
20 반환
```

---

## 7. 실행 및 검증

### 7.1 QEMU Memory Map 확인

QEMU Monitor에서:

```text
info mtree
```

를 실행했다.

다음 결과를 통해 `vperiph`가 의도한 주소에 정상적으로 배치된 것을 확인했다.

```text
00000000090d0000-00000000090d0fff (prio 0, i/o): vperiph
```

### 7.2 초기 Register 확인

Guest Linux에서 `devmem`을 이용하여 직접 MMIO Register를 확인했다.

```bash
devmem 0x090d0008 32
devmem 0x090d0004 32
```

초기 DATA와 STATUS 값이 `0`임을 확인했다.

### 7.3 DATA Register Write

DATA에 10을 저장했다.

```bash
devmem 0x090d0008 32 10
```

다시 읽었을 때:

```text
0x0000000A
```

가 반환되는 것을 확인했다.

### 7.4 CONTROL 동작 검증

CONTROL에 `1`을 기록했다.

```bash
devmem 0x090d0000 32 1
```

이후 DATA를 읽었다.

```bash
devmem 0x090d0008 32
```

결과:

```text
0x00000014
```

즉 `10 → 20`으로 변경되었다.

STATUS도 확인했다.

```bash
devmem 0x090d0004 32
```

결과:

```text
0x00000001
```

따라서 CONTROL 명령에 의해 QEMU Peripheral 내부 연산이 정상적으로 수행됨을 확인했다.

### 7.5 Read-Only Register 검증

STATUS Register에 직접 값을 쓰는 동작을 시도한 뒤 다시 값을 읽어 기존 STATUS 값이 유지되는지 확인했다.

이를 통해 STATUS에 대한 write가 무시되는 Read Only Register 동작을 검증했다.

### 7.6 상태 전이 검증

새로운 DATA 값을 기록하면 STATUS가 다시 IDLE 상태인 `0`으로 변경되는 것을 확인했다.

이후 CONTROL에 다시 `1`을 기록하면 DATA 연산이 수행되고 STATUS가 다시 `1`로 전환되는 것을 확인했다.

```text
DATA Write
    ↓
STATUS = IDLE

CONTROL = START
    ↓
DATA Processing
    ↓
STATUS = DONE
```

---

## 8. 배운 점

이번 Phase를 통해 Linux Driver가 접근하는 하드웨어의 반대쪽인 **Peripheral Device Model**의 동작을 직접 구현해볼 수 있었다.

기존에는 MMIO를 주로 Driver 입장에서 다음과 같이 이해했다.

```text
Driver
  ↓
ioremap()
  ↓
Peripheral Register
```

이번 구현을 통해 Peripheral 쪽에서는 반대로:

```text
Guest CPU MMIO Access
  ↓
QEMU MemoryRegion
  ↓
read/write callback
  ↓
Device State 변경
```

이라는 구조로 동작한다는 것을 확인했다.

또한 QEMU에서 Device를 구현하기 위해서는 단순한 read/write 함수뿐 아니라 다음 요소들이 함께 연결되어야 한다는 것을 이해했다.

```text
QOM Type
   ↓
Device Instance
   ↓
SysBusDevice
   ↓
MemoryRegion
   ↓
MemoryRegionOps
   ↓
Guest Physical Address Mapping
```

특히 다음 두 단계의 차이를 명확하게 이해할 수 있었다.

```text
sysbus_init_mmio()
= Device가 MMIO Region을 제공한다고 등록

sysbus_mmio_map()
= 해당 MMIO Region을 실제 Guest Physical Address에 배치
```

마지막으로 Guest Linux의 `devmem`을 이용해 직접 Register를 읽고 쓰면서:

```text
Guest Linux
→ ARM64 MMIO Access
→ QEMU Memory System
→ Custom Device Callback
→ Virtual Register State
```

전체 경로가 실제로 동작하는 것을 검증했다.

Phase 1에서는 Driver나 Device Tree 없이 물리주소를 직접 알고 접근했다.

다음 Phase에서는 Device Tree를 추가하여 Guest Linux가 하드코딩된 주소가 아니라 하드웨어 기술 정보를 통해 `vperiph`의 주소와 리소스를 인식할 수 있도록 확장한다.
