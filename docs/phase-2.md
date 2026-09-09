# Phase 2 - Device Tree & Linux Platform Driver

## 1. 목표

Phase 1에서 구현한 QEMU Custom MMIO Peripheral `vperiph`를 Device Tree에 기술하고, Guest Linux에서 해당 장치를 Platform Device로 인식시킨 뒤 Linux Platform Driver와 연결한다.

이번 Phase의 목표는 다음 흐름을 완성하는 것이다.

```text
QEMU Virtual Peripheral
        ↓
Device Tree
        ↓
Linux Platform Device
        ↓
Platform Driver
        ↓
MMIO Resource Mapping
        ↓
readl()/writel()
        ↓
QEMU Device Callback
```

최종적으로 Driver가 하드코딩된 물리주소를 사용하지 않고 Device Tree의 `reg` 정보를 통해 MMIO Resource를 획득한 뒤 `vperiph`를 직접 제어하도록 구현한다.

---

## 2. 배경 개념

### 2.1 Device Tree

Device Tree는 보드에 어떤 하드웨어가 존재하고, 각 하드웨어가 어떤 자원을 사용하는지 Linux Kernel에 전달하는 하드웨어 기술 정보다.

```dts
vperiph@90d0000 {
    compatible = "qemu,vperiph";
    reg = <0x0 0x090d0000 0x0 0x1000>;
};
```

- `compatible`: 장치를 식별하는 문자열이며 Driver의 `of_match_table`과 매칭된다.
- `reg`: 장치의 MMIO base address와 영역 크기를 기술한다.

### 2.2 MMIO

MMIO(Memory-Mapped I/O)는 장치 Register를 CPU의 주소 공간에 배치하여 일반 메모리 접근처럼 load/store 방식으로 장치를 제어하는 방식이다.

```text
Base Address : 0x090d0000
Size         : 0x00001000
Range        : 0x090d0000 ~ 0x090d0fff
```

| Offset | Register | Access | Description |
|---:|---|---|---|
| `0x00` | CONTROL | R/W | `1`을 쓰면 연산 실행 |
| `0x04` | STATUS | R | `0 = IDLE`, `1 = DONE` |
| `0x08` | DATA | R/W | 입력 및 결과 데이터 |

### 2.3 Platform Device / Platform Driver

Device Tree의 node를 기반으로 Linux가 `platform_device`를 생성하고, 등록된 `platform_driver` 중 `compatible`이 일치하는 Driver를 찾아 연결한다.

```text
Device Tree node
        ↓
platform_device 생성
        ↓
platform_driver 등록
        ↓
compatible 비교
        ↓
match
        ↓
probe()
```

### 2.4 `of_match_table`

```c
static const struct of_device_id vperiph_of_match[] = {
    { .compatible = "qemu,vperiph" },
    { }
};
```

Device Tree의 `compatible = "qemu,vperiph"`와 일치하면 Linux가 해당 Driver를 장치와 매칭한다.

### 2.5 `probe()`

매칭이 성공하면 Linux가 다음 함수를 호출한다.

```c
static int vperiph_probe(struct platform_device *pdev)
```

`pdev`는 Device Tree node를 바탕으로 생성된 Linux 내부의 장치 객체이며 MMIO Resource 같은 장치 정보를 포함한다.

### 2.6 Resource와 I/O Mapping

Device Tree의 `reg` 정보는 Linux 내부에서 Resource로 변환되고, Driver는 이를 Kernel Virtual Address에 매핑한다.

```text
Device Tree reg
0x090d0000 / 0x1000
        ↓
platform_device resource
        ↓
devm_platform_ioremap_resource()
        ↓
Kernel Virtual Address
        ↓
readl() / writel()
```

---

## 3. 시스템 구조

```text
QEMU ARM64 virt
    └── vperiph
         ├── MMIO: 0x090d0000 ~ 0x090d0fff
         ├── CONTROL
         ├── STATUS
         └── DATA

Guest Linux
    ├── Device Tree
    │    ├── compatible = "qemu,vperiph"
    │    └── reg = 0x090d0000 / 0x1000
    │
    └── vperiph Platform Driver
         ├── of_match_table
         ├── probe()
         ├── devm_platform_ioremap_resource()
         └── readl()/writel()
```

---

## 4. 구현 과정

### 4.1 QEMU에서 Device Tree Node 생성

```c
static void create_vperiph(VirtMachineState *vms)
{
    char *nodename;
    hwaddr base = vms->memmap[VIRT_VPERIPH].base;
    hwaddr size = vms->memmap[VIRT_VPERIPH].size;
    DeviceState *dev;
    SysBusDevice *sbd;
    MachineState *ms = MACHINE(vms);

    dev = qdev_new(TYPE_EDU_MMIO);
    sbd = SYS_BUS_DEVICE(dev);

    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, base);

    nodename = g_strdup_printf("/vperiph@%" PRIx64, base);

    qemu_fdt_add_subnode(ms->fdt, nodename);
    qemu_fdt_setprop_string(ms->fdt, nodename,
                            "compatible", "qemu,vperiph");
    qemu_fdt_setprop_sized_cells(ms->fdt, nodename, "reg",
                                 2, base, 2, size);

    g_free(nodename);
}
```

실제 QEMU Memory Map과 Device Tree의 `reg` 정보가 동일한 `base`와 `size`를 사용하도록 구성했다.

### 4.2 Guest Linux에서 Device Tree 확인

```bash
ls /proc/device-tree/vperiph@90d0000
```

결과:

```text
compatible  name  reg
```

```bash
cat /proc/device-tree/vperiph@90d0000/compatible
```

결과:

```text
qemu,vperiph
```

```bash
hexdump -C /proc/device-tree/vperiph@90d0000/reg
```

결과:

```text
00000000  00 00 00 00 09 0d 00 00  00 00 00 00 00 00 10 00
```

해석:

```text
Base Address = 0x00000000090d0000
Size         = 0x0000000000001000
```

### 4.3 Linux Platform Driver Skeleton

```c
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>

static int vperiph_probe(struct platform_device *pdev)
{
    dev_info(&pdev->dev, "vperiph probe called\n");
    return 0;
}

static const struct of_device_id vperiph_of_match[] = {
    { .compatible = "qemu,vperiph" },
    { }
};

MODULE_DEVICE_TABLE(of, vperiph_of_match);

static struct platform_driver vperiph_driver = {
    .probe = vperiph_probe,
    .driver = {
        .name = "vperiph",
        .of_match_table = vperiph_of_match,
    },
};

module_platform_driver(vperiph_driver);
```

### 4.4 Driver Cross Compile

Kernel tree:

```text
buildroot/output/build/linux-6.18.7
```

Cross Compiler:

```text
aarch64-buildroot-linux-gnu-
```

빌드:

```bash
make   -C ~/projects/qemu-linux/buildroot/output/build/linux-6.18.7   M=$PWD   ARCH=arm64   CROSS_COMPILE=~/projects/qemu-linux/buildroot/output/host/bin/aarch64-buildroot-linux-gnu-   modules
```

결과:

```text
LD [M]  vperiph_driver.ko
```

### 4.5 Guest RootFS에 Driver 배치

```bash
sudo mount -o loop   buildroot/output/images/rootfs.ext4   mnt-rootfs

sudo cp driver/vperiph_driver.ko   mnt-rootfs/root/

sudo umount mnt-rootfs
```

### 4.6 Device Tree와 Driver 매칭 검증

Guest Linux:

```bash
insmod /root/vperiph_driver.ko
```

Kernel Log:

```text
vperiph_driver: loading out-of-tree module taints kernel.
vperiph 90d0000.vperiph: vperiph probe called
```

### 4.7 MMIO Resource Mapping

```c
#include <linux/io.h>

#define REG_CONTROL 0x00
#define REG_STATUS  0x04
#define REG_DATA    0x08

struct vperiph_dev {
    void __iomem *base;
};
```

`probe()`:

```c
vdev = devm_kzalloc(&pdev->dev, sizeof(*vdev), GFP_KERNEL);
if (!vdev)
    return -ENOMEM;

vdev->base = devm_platform_ioremap_resource(pdev, 0);
if (IS_ERR(vdev->base))
    return PTR_ERR(vdev->base);

platform_set_drvdata(pdev, vdev);
```

### 4.8 Driver에서 Register 제어

```c
writel(10, vdev->base + REG_DATA);
writel(1, vdev->base + REG_CONTROL);

data = readl(vdev->base + REG_DATA);
status = readl(vdev->base + REG_STATUS);
```

최종 Kernel Log:

```text
vperiph 90d0000.vperiph: vperiph probe called
vperiph 90d0000.vperiph: DATA=20 STATUS=1
```

---

## 5. 실행 및 검증

- Device Tree node 존재: ✅
- `compatible = "qemu,vperiph"`: ✅
- `reg = 0x090d0000 / 0x1000`: ✅
- Platform Driver `probe()` 호출: ✅
- MMIO Resource Mapping: ✅
- Driver의 `readl()/writel()` 동작: ✅
- DATA `10 → 20`, STATUS `0 → 1`: ✅

---

## 6. 최종 데이터 흐름

```text
QEMU vperiph Device
MMIO: 0x090d0000 ~ 0x090d0fff
        ↓
Device Tree
compatible = "qemu,vperiph"
reg = 0x090d0000 / 0x1000
        ↓
Linux platform_device
        ↓
vperiph Platform Driver
        ↓
probe()
        ↓
devm_platform_ioremap_resource()
        ↓
Kernel Virtual Address
        ↓
writel() / readl()
        ↓
QEMU edu_mmio_write() / edu_mmio_read()
        ↓
DATA 10 → 20
STATUS 0 → 1
```

---

## 7. 배운 점

이번 Phase를 통해 다음 역할을 구분할 수 있게 되었다.

```text
QEMU Device Model
= 실제 동작하는 가상 하드웨어

Device Tree
= Linux에 전달되는 하드웨어 기술 정보

Platform Device
= Linux 내부에서 표현된 장치 객체

Platform Driver
= 해당 장치를 제어하는 Kernel Driver
```

또한 Device Tree의 `compatible`은 Driver 매칭에 사용되고, `reg`는 Driver가 접근해야 할 MMIO Resource를 제공한다는 점을 직접 검증했다.

Phase 1에서는 Guest에서 `devmem`으로 물리주소를 직접 알고 접근했지만, Phase 2에서는 Driver가 Device Tree를 통해 Resource를 획득하고 `devm_platform_ioremap_resource()`로 MMIO 영역을 매핑하도록 변경했다.

다음 Phase에서는 Kernel 내부에 하드코딩된 테스트 동작을 제거하고 User Space에서 장치를 제어할 수 있도록 Character Device 또는 miscdevice 기반 인터페이스와 C++ CLI를 구현할 예정이다.
