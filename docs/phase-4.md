# Phase 4 - Hardware Interrupt

## 1. 목표

Phase 3에서 User Space → Linux Driver → MMIO → QEMU Peripheral까지 제어 경로를 구현했다.

이번 Phase에서는 `vperiph`가 연산 완료를 Linux Driver에 능동적으로 알릴 수 있도록 Hardware Interrupt를 추가한다.

최종 목표는 다음 흐름을 구현하고 검증하는 것이다.

```text
User Space
    ↓
Linux Driver
    ↓
MMIO CONTROL Write
    ↓
QEMU vperiph
    ↓
DATA Processing
    ↓
IRQ Raise
    ↓
ARM GIC
    ↓
Linux IRQ Handler
    ↓
IRQ ACK / Clear
```

최종적으로 다음 기능을 갖도록 확장한다.

- QEMU Device에 IRQ Output 추가
- ARM `virt` Machine의 GIC에 IRQ 연결
- Device Tree에 Interrupt Resource 기술
- Linux Platform Driver에서 IRQ Resource 획득
- Interrupt Handler 등록
- Level-triggered IRQ의 ACK / Clear 처리
- 실제 CLI 실행 시 Interrupt 발생 검증

---

## 2. 배경 개념

### 2.1 IRQ

IRQ(Interrupt Request)는 Peripheral이 CPU에 이벤트 발생을 알리는 신호이다.

Polling 방식에서는 CPU가 장치 상태를 반복해서 확인해야 한다.

```text
CPU
├── 완료됐는가?
├── 완료됐는가?
├── 완료됐는가?
└── 완료됐는가?
```

Interrupt 방식에서는 장치가 이벤트가 발생한 시점에 CPU에 알린다.

```text
CPU
└── 다른 작업 수행

Peripheral
└── 작업 완료
     ↓
    IRQ
     ↓
    CPU
```

이번 프로젝트에서는 `vperiph`가 CONTROL 명령을 처리한 뒤 연산이 끝났음을 IRQ로 알리도록 구현한다.

### 2.2 ARM GIC

GIC(Generic Interrupt Controller)는 ARM 시스템에서 Peripheral의 Interrupt를 모아 CPU에 전달하는 Interrupt Controller이다.

```text
UART ───┐
RTC  ───┤
GPIO ───┤
vperiph ┤
        ↓
       GIC
        ↓
      ARM CPU
```

QEMU ARM `virt` Machine에도 GIC Device가 존재하며, 각 Peripheral의 IRQ Output을 GIC Input에 연결할 수 있다.

### 2.3 SGI / PPI / SPI

GIC의 Interrupt는 대표적으로 다음과 같이 구분된다.

| 종류 | 의미 | 주요 용도 |
|---|---|---|
| SGI | Software Generated Interrupt | CPU 간 소프트웨어 Interrupt |
| PPI | Private Peripheral Interrupt | 특정 CPU에 종속된 Timer 등 |
| SPI | Shared Peripheral Interrupt | UART, GPIO, 일반 Peripheral |

`vperiph`는 특정 CPU 전용 장치가 아닌 일반 Peripheral이므로 SPI를 사용한다.

### 2.4 Edge / Level Trigger

SGI/PPI/SPI는 Interrupt의 종류를 나타내고, Edge/Level은 Interrupt 신호를 어떤 방식으로 감지하는지를 나타낸다.

이번 구현에서는:

```text
Interrupt Type : SPI
SPI Index      : 11
Trigger        : Level High
```

를 사용했다.

Level High 방식에서는 IRQ Line이 HIGH인 동안 Interrupt가 계속 Active 상태로 유지될 수 있으므로 처리 후 IRQ를 LOW로 내려주는 ACK/Clear 과정이 필요하다.

---

## 3. 왜 필요한가

Phase 3까지는 User Space가 장치에 명령을 전달하고 결과를 직접 읽는 구조였다.

```text
User Space
    ↓
write / ioctl
    ↓
Linux Driver
    ↓
MMIO
    ↓
QEMU Device
    ↓
read()
```

이 구조에서는 장치가 언제 작업을 완료했는지 CPU에 능동적으로 알리는 경로가 없다.

실제 Peripheral은 작업 완료, 데이터 수신, 오류 발생 등 비동기 이벤트를 Interrupt로 알리는 경우가 많다.

따라서 이번 Phase에서는:

```text
Device 상태를 CPU가 확인
```

하는 구조에서:

```text
Device가 CPU에 이벤트를 통지
```

하는 구조로 확장한다.

이를 통해 단순 MMIO Register 제어뿐 아니라 Peripheral → Interrupt Controller → CPU → Linux Driver까지 이어지는 실제 임베디드 시스템의 Interrupt 흐름을 구현하는 것을 목표로 했다.

---

## 4. 시스템 구조

Phase 4 완료 후 구조는 다음과 같다.

```text
Windows 11
    ↓
WSL2 Ubuntu
    │
    └── QEMU ARM64 virt Machine
         │
         ├── ARM CPU
         ├── GIC
         │    ↑
         │    │ SPI 11
         │    │
         └── vperiph
              ├── CONTROL
              ├── STATUS
              ├── DATA
              ├── IRQ_ACK
              └── IRQ Output

Guest Linux
    │
    ├── Device Tree
    │    ├── reg
    │    └── interrupts
    │
    └── vperiph Platform Driver
         ├── platform_get_irq()
         ├── devm_request_irq()
         └── vperiph_irq_handler()
```

파일 기준으로 보면 다음과 같다.

```text
qemu/hw/misc/edu-mmio.c
└── IRQ 발생 / Clear

qemu/hw/arm/virt.c
└── GIC 연결 / Device Tree interrupts 생성

driver/vperiph_driver.c
└── IRQ Resource 획득 / Handler 등록 / ACK
```

---

## 5. 구현 과정

### 5.1 QEMU Device에 IRQ Output 추가

파일:

```text
qemu/hw/misc/edu-mmio.c
```

`EduMmioState`에 `qemu_irq`를 추가했다.

```c
struct EduMmioState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t control;
    uint32_t status;
    uint32_t data;
};
```

`edu_mmio_init()`에서는 `sysbus_init_irq()`를 이용해 SysBusDevice가 IRQ Output 하나를 제공하도록 등록했다.

```c
static void edu_mmio_init(Object *obj)
{
    EduMmioState *s = EDU_MMIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, OBJECT(s), &edu_mmio_ops, s,
                          TYPE_EDU_MMIO, EDU_MMIO_SIZE);

    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);
}
```

이 단계에서는 IRQ Output의 존재만 정의되며 아직 GIC에는 연결되지 않는다.

### 5.2 ARM virt Machine의 IRQ Map에 추가

파일:

```text
qemu/hw/arm/virt.c
```

기존 `a15irqmap[]`을 확인한 뒤 사용되지 않는 SPI Index 11을 `vperiph`에 할당했다.

```c
[VIRT_VPERIPH] = 11,
```

`create_vperiph()`에서는 해당 IRQ 번호를 가져온다.

```c
int irq = vms->irqmap[VIRT_VPERIPH];
```

### 5.3 vperiph IRQ를 GIC에 연결

같은 `create_vperiph()`에서 Device의 첫 번째 IRQ Output을 GIC Input에 연결했다.

```c
sysbus_connect_irq(sbd, 0,
                   qdev_get_gpio_in(vms->gic, irq));
```

이를 통해 QEMU 내부 배선은 다음과 같이 구성된다.

```text
vperiph IRQ Output
    ↓
GIC SPI 11
    ↓
ARM CPU
```

### 5.4 Device Tree에 Interrupt Resource 추가

Linux가 해당 IRQ Resource를 알 수 있도록 `vperiph` Device Tree Node에 `interrupts` Property를 추가했다.

```c
qemu_fdt_setprop_cells(ms->fdt,
                       nodename,
                       "interrupts",
                       GIC_FDT_IRQ_TYPE_SPI,
                       irq,
                       GIC_FDT_IRQ_FLAGS_LEVEL_HI);
```

Guest Linux에서 확인:

```bash
hexdump -C /proc/device-tree/vperiph@90d0000/interrupts
```

결과:

```text
00000000  00 00 00 00 00 00 00 0b  00 00 00 04
```

해석:

```text
0  = SPI
11 = SPI Index
4  = Level High
```

### 5.5 Linux Driver에서 IRQ Resource 획득

파일:

```text
driver/vperiph_driver.c
```

Driver State에 IRQ 번호를 저장할 필드를 추가했다.

```c
struct vperiph_dev {
    void __iomem *base;
    struct miscdevice miscdev;
    int irq;
};
```

`vperiph_probe()`에서 Device Tree를 기반으로 생성된 첫 번째 IRQ Resource를 가져왔다.

```c
vdev->irq = platform_get_irq(pdev, 0);
if (vdev->irq < 0)
    return vdev->irq;
```

여기서 `0`은 IRQ 번호가 아니라 첫 번째 IRQ Resource의 Index이다.

### 5.6 Interrupt Handler 등록

`devm_request_irq()`를 이용해 해당 IRQ 발생 시 실행할 Handler를 등록했다.

```c
ret = devm_request_irq(&pdev->dev,
                       vdev->irq,
                       vperiph_irq_handler,
                       0,
                       "vperiph",
                       vdev);
if (ret)
    return ret;
```

Interrupt Handler는 다음과 같이 구현했다.

```c
static irqreturn_t vperiph_irq_handler(int irq, void *dev_id)
{
    struct vperiph_dev *vdev = dev_id;

    dev_info(vdev->miscdev.parent,
             "vperiph interrupt received\n");

    writel(1, vdev->base + REG_IRQ_ACK);

    return IRQ_HANDLED;
}
```

### 5.7 QEMU Device에서 IRQ Raise

CONTROL Register에 `1`이 기록되어 연산이 완료되면 IRQ Line을 HIGH로 변경한다.

```c
case REG_CONTROL:
    s->control = value;

    if (value == 1) {
        s->data *= 2;
        s->status = 1;
        qemu_set_irq(s->irq, 1);
    }
    break;
```

### 5.8 IRQ ACK Register 추가

Level High Interrupt를 정상 종료하기 위해 IRQ 전용 ACK Register를 추가했다.

```c
#define REG_IRQ_ACK 0x0C
```

Register Map은 다음과 같이 확장되었다.

| Offset | Register | Access | 의미 |
|---:|---|---|---|
| `0x00` | CONTROL | R/W | `1`을 쓰면 연산 수행 |
| `0x04` | STATUS | R | `0 = IDLE`, `1 = DONE` |
| `0x08` | DATA | R/W | 입력 및 결과 데이터 |
| `0x0C` | IRQ_ACK | W | `1`을 쓰면 IRQ Clear |

QEMU Device는 ACK Write를 받으면 IRQ Line을 LOW로 내린다.

```c
case REG_IRQ_ACK:
    if (value == 1)
        qemu_set_irq(s->irq, 0);
    break;
```

---

## 6. 핵심 코드

Phase 4의 핵심 흐름을 단순화하면 다음과 같다.

QEMU Device:

```c
case REG_CONTROL:
    if (value == 1) {
        s->data *= 2;
        s->status = 1;
        qemu_set_irq(s->irq, 1);
    }
    break;

case REG_IRQ_ACK:
    if (value == 1)
        qemu_set_irq(s->irq, 0);
    break;
```

Linux Driver:

```c
vdev->irq = platform_get_irq(pdev, 0);

ret = devm_request_irq(&pdev->dev,
                       vdev->irq,
                       vperiph_irq_handler,
                       0,
                       "vperiph",
                       vdev);
```

```c
static irqreturn_t vperiph_irq_handler(int irq, void *dev_id)
{
    struct vperiph_dev *vdev = dev_id;

    dev_info(vdev->miscdev.parent,
             "vperiph interrupt received\n");

    writel(1, vdev->base + REG_IRQ_ACK);

    return IRQ_HANDLED;
}
```

전체 동작:

```text
CONTROL = 1
    ↓
QEMU DATA Processing
    ↓
STATUS = DONE
    ↓
IRQ HIGH
    ↓
GIC
    ↓
Linux IRQ Handler
    ↓
IRQ_ACK = 1
    ↓
IRQ LOW
```

---

## 7. 실행 및 검증

### 7.1 Device Tree Interrupt 확인

Guest Linux에서:

```bash
hexdump -C /proc/device-tree/vperiph@90d0000/interrupts
```

결과:

```text
00000000  00 00 00 00 00 00 00 0b  00 00 00 04
```

SPI 11, Level High 설정이 정상적으로 전달된 것을 확인했다.

### 7.2 Driver 로드

```bash
insmod /root/vperiph_driver.ko
```

결과:

```text
vperiph 90d0000.vperiph: vperiph probe called
vperiph 90d0000.vperiph: registered /dev/vperiph
```

### 7.3 IRQ 동작 검증

User-space CLI를 실행했다.

```bash
/root/vperiph_cli 10
```

결과:

```text
vperiph 90d0000.vperiph: vperiph interrupt received
input=10
result=20
status=1
```

Kernel Log에서도 Handler 호출을 확인했다.

```text
vperiph 90d0000.vperiph: vperiph interrupt received
```

이를 통해:

```text
User Space
→ Driver
→ QEMU Device
→ IRQ
→ GIC
→ Linux IRQ Handler
→ ACK
```

전체 경로가 정상적으로 동작함을 확인했다.

---

## 8. 발생한 문제

### Level High IRQ가 계속 발생하는 문제

초기 구현에서는 QEMU Device가 연산 완료 시 다음 코드로 IRQ를 발생시켰다.

```c
qemu_set_irq(s->irq, 1);
```

그러나 Interrupt Handler가 실행된 뒤에도 IRQ Line을 다시 LOW로 내리는 처리가 없었다.

이번 Interrupt는 Level High 방식으로 설정되어 있었기 때문에:

```text
IRQ HIGH
    ↓
GIC가 Interrupt 전달
    ↓
Linux Handler 실행
    ↓
Handler 종료
    ↓
IRQ Line은 여전히 HIGH
    ↓
다시 Interrupt 발생
```

이 반복되면서 Guest가 정상적으로 진행되지 않고 Interrupt가 계속 발생하는 현상을 확인했다.

이를 통해 Level-triggered Interrupt에서는 Handler 호출 자체만으로 Interrupt 처리가 끝나는 것이 아니라, Device 측 Interrupt 상태를 명시적으로 해제해야 한다는 점을 확인했다.

해결을 위해 `IRQ_ACK` Register를 추가했다.

```text
0x0C : IRQ_ACK
```

Driver Handler에서:

```c
writel(1, vdev->base + REG_IRQ_ACK);
```

을 수행하고, QEMU Device에서는:

```c
case REG_IRQ_ACK:
    if (value == 1)
        qemu_set_irq(s->irq, 0);
    break;
```

로 IRQ Line을 LOW로 변경했다.

수정 후에는 CLI 한 번 실행 시 Interrupt가 한 번만 발생하고 정상적으로 종료되는 것을 확인했다.

```text
IRQ Raise
    ↓
Handler
    ↓
ACK
    ↓
IRQ Clear
```

이 문제를 통해 Interrupt 처리에서는 단순히 IRQ를 발생시키는 것뿐 아니라 **발생 조건과 Clear/Acknowledge 방식까지 Register 설계에 포함되어야 한다**는 점을 확인했다.

---

## 9. 배운 점

Phase 4를 통해 Peripheral의 Interrupt 경로를 QEMU Device Model부터 Linux Driver까지 직접 연결했다.

Phase 3까지는:

```text
User Space
    ↓
Linux Driver
    ↓
MMIO
    ↓
QEMU Peripheral
```

구조였다.

이번 Phase에서는:

```text
User Space
    ↓
Linux Driver
    ↓
MMIO
    ↓
QEMU Peripheral
    ↓
IRQ
    ↓
ARM GIC
    ↓
Linux IRQ Handler
```

까지 확장했다.

특히 다음 요소들의 역할을 구분할 수 있었다.

```text
sysbus_init_irq()
= Device가 IRQ Output을 제공한다고 등록

sysbus_connect_irq()
= Device의 IRQ Output을 GIC Input에 연결

Device Tree interrupts
= Linux에 Interrupt Resource를 기술

platform_get_irq()
= Platform Driver가 IRQ Resource를 획득

devm_request_irq()
= Linux IRQ Handler 등록

qemu_set_irq(..., 1)
= Device IRQ Raise

IRQ_ACK
= Level-triggered Interrupt Clear
```

또한 SGI/PPI/SPI와 Edge/Level은 서로 다른 Interrupt 분류라는 점을 이해했고, 일반 Peripheral인 `vperiph`에는 SPI + Level High 방식을 적용했다.

가장 중요하게는 초기 IRQ 반복 문제를 통해 Level-triggered Interrupt가:

```text
Raise
→ Handle
→ Acknowledge
→ Clear
```

의 전체 수명주기를 가져야 한다는 점을 실제 동작으로 확인했다.

다음 Phase에서는 Buildroot 단계에서 Driver와 User-space CLI를 Root Filesystem에 자동 포함시키고, 수동 `mount → copy → insmod` 과정을 제거해 재현 가능한 임베디드 Linux 이미지로 통합한다.
