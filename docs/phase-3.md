# Phase 3 - User-space Device Control Interface

## 1. 목표

Phase 2에서 Linux Platform Driver가 QEMU `vperiph`의 MMIO Resource를 매핑하고 Driver 내부에서 Register를 읽고 쓰는 흐름까지 구현했다.

Phase 3의 목표는 Driver 내부의 테스트 동작을 제거하고, Guest Linux의 User Space에서 `/dev/vperiph`를 통해 장치를 제어할 수 있도록 하는 것이다.

최종 흐름:

```text
User-space C++ CLI
        ↓
/dev/vperiph
        ↓
Linux Driver
        ↓
MMIO
        ↓
QEMU vperiph
```

CLI는 입력값 하나를 받아 다음 순서를 자동 수행한다.

```text
write()
→ DATA 입력

ioctl()
→ CONTROL = 1
→ 연산 실행

read()
→ DATA / STATUS 결과 확인
```

---

## 2. 시스템 구조

```text
~/projects/qemu-linux/user/vperiph_cli.cpp
└─ main()
   ├─ open("/dev/vperiph")
   ├─ write()
   ├─ ioctl()
   └─ read()

        ↓

Guest Linux
/dev/vperiph

        ↓

~/projects/qemu-linux/driver/vperiph_driver.c
├─ vperiph_write()
├─ vperiph_ioctl()
├─ vperiph_read()
└─ vperiph_probe()

        ↓

MMIO
0x090d0000 ~ 0x090d0fff

        ↓

~/projects/qemu-linux/qemu/hw/misc/edu-mmio.c
├─ edu_mmio_write()
└─ edu_mmio_read()
```

---

## 3. Character Device Interface

### 3.1 `/dev/vperiph`

Linux의 `/dev` 아래 device file은 일반 파일처럼 데이터를 저장하는 파일이 아니라, User Space와 Kernel Driver를 연결하는 인터페이스다.

Phase 3에서는 `miscdevice`를 이용해 다음 device file을 생성했다.

```text
/dev/vperiph
```

Guest Linux 확인 결과:

```text
crw------- 1 root root 10, 259 /dev/vperiph
```

앞의 `c`는 Character Device를 의미한다.

### 3.2 miscdevice

기존 프로젝트에서는 `register_chrdev()`를 이용해 Character Device를 직접 등록했지만, 이번 프로젝트에서는 minor 번호 관리와 device file 등록을 간단하게 처리하기 위해 `miscdevice`를 사용했다.

```c
struct vperiph_dev {
    void __iomem *base;
    struct miscdevice miscdev;
};
```

`~/projects/qemu-linux/driver/vperiph_driver.c`의 `vperiph_probe()`에서:

```c
vdev->miscdev.minor = MISC_DYNAMIC_MINOR;
vdev->miscdev.name = "vperiph";
vdev->miscdev.fops = &vperiph_fops;
vdev->miscdev.parent = &pdev->dev;

ret = misc_register(&vdev->miscdev);
if (ret)
    return ret;
```

이를 통해 Guest Linux에 `/dev/vperiph`가 생성된다.

---

## 4. User Space ↔ Driver Interface

`file_operations`를 통해 세 가지 인터페이스를 제공한다.

| User-space 호출 | Driver 함수 | 역할 |
|---|---|---|
| `write()` | `vperiph_write()` | DATA Register에 입력값 전달 |
| `ioctl()` | `vperiph_ioctl()` | CONTROL Register에 START 명령 전달 |
| `read()` | `vperiph_read()` | DATA / STATUS 결과 반환 |

```c
static const struct file_operations vperiph_fops = {
    .owner = THIS_MODULE,
    .read = vperiph_read,
    .write = vperiph_write,
    .unlocked_ioctl = vperiph_ioctl,
};
```

---

## 5. write() 구현

파일:

```text
~/projects/qemu-linux/driver/vperiph_driver.c
```

함수:

```c
vperiph_write()
```

```c
static ssize_t vperiph_write(struct file *file,
                             const char __user *buf,
                             size_t count,
                             loff_t *ppos)
{
    struct miscdevice *misc = file->private_data;
    struct vperiph_dev *vdev =
        container_of(misc, struct vperiph_dev, miscdev);

    u32 value;

    if (count < sizeof(value))
        return -EINVAL;

    if (copy_from_user(&value, buf, sizeof(value)))
        return -EFAULT;

    writel(value, vdev->base + REG_DATA);

    return sizeof(value);
}
```

흐름:

```text
User-space write()
        ↓
vperiph_write()
        ↓
copy_from_user()
        ↓
writel(value, REG_DATA)
        ↓
QEMU DATA Register
```

초기 검증:

```bash
printf '\x0a\x00\x00\x00' > /dev/vperiph
devmem 0x090d0008 32
```

결과:

```text
0x0000000A
```

---

## 6. ioctl() 구현

입력 데이터는 `write()`, 장치 제어 명령은 `ioctl()`로 분리했다.

```c
#define VPERIPH_IOC_MAGIC 'v'
#define VPERIPH_START _IO(VPERIPH_IOC_MAGIC, 0)
```

```c
static long vperiph_ioctl(struct file *file,
                          unsigned int cmd,
                          unsigned long arg)
{
    struct miscdevice *misc = file->private_data;
    struct vperiph_dev *vdev =
        container_of(misc, struct vperiph_dev, miscdev);

    switch (cmd) {
    case VPERIPH_START:
        writel(1, vdev->base + REG_CONTROL);
        return 0;

    default:
        return -ENOTTY;
    }
}
```

흐름:

```text
User-space ioctl(VPERIPH_START)
        ↓
vperiph_ioctl()
        ↓
writel(1, REG_CONTROL)
        ↓
QEMU CONTROL Register
        ↓
DATA *= 2
STATUS = 1
```

---

## 7. read() 구현

연산 결과를 User Space로 전달하기 위해 DATA와 STATUS를 함께 반환하는 구조체를 정의했다.

```c
struct vperiph_result {
    u32 data;
    u32 status;
};
```

`vperiph_read()`:

```c
static ssize_t vperiph_read(struct file *file,
                            char __user *buf,
                            size_t count,
                            loff_t *ppos)
{
    struct miscdevice *misc = file->private_data;
    struct vperiph_dev *vdev =
        container_of(misc, struct vperiph_dev, miscdev);

    struct vperiph_result result;

    if (count < sizeof(result))
        return -EINVAL;

    result.data = readl(vdev->base + REG_DATA);
    result.status = readl(vdev->base + REG_STATUS);

    if (copy_to_user(buf, &result, sizeof(result)))
        return -EFAULT;

    return sizeof(result);
}
```

흐름:

```text
QEMU DATA / STATUS
        ↓
readl()
        ↓
vperiph_result
        ↓
copy_to_user()
        ↓
User-space C++ CLI
```

---

## 8. C++ User-space CLI

파일:

```text
~/projects/qemu-linux/user/vperiph_cli.cpp
```

CLI는 다음 순서로 동작한다.

```text
1. /dev/vperiph open
2. write()로 DATA 입력
3. ioctl()로 START
4. read()로 결과 확인
5. 결과 출력
```

결과 구조체:

```cpp
struct VperiphResult {
    std::uint32_t data;
    std::uint32_t status;
};
```

핵심 코드:

```cpp
write(fd, &value, sizeof(value));

ioctl(fd, VPERIPH_START);

VperiphResult result{};
read(fd, &result, sizeof(result));

std::cout << "input=" << value << '\n';
std::cout << "result=" << result.data << '\n';
std::cout << "status=" << result.status << '\n';
```

---

## 9. ARM64 Cross Compile

Buildroot Toolchain에서 C++ 지원을 활성화했다.

```text
BR2_TOOLCHAIN_BUILDROOT_CXX=y
```

생성된 Cross Compiler:

```text
output/host/bin/aarch64-buildroot-linux-gnu-g++
```

CLI 빌드:

```bash
~/projects/qemu-linux/buildroot/output/host/bin/aarch64-buildroot-linux-gnu-g++   user/vperiph_cli.cpp   -o user/vperiph_cli
```

확인 결과:

```text
ELF 64-bit LSB pie executable, ARM aarch64
```

---

## 10. Guest RootFS 배치

QEMU를 종료한 상태에서 root filesystem image를 mount하고 Driver와 CLI를 복사했다.

```bash
sudo mount -o loop   buildroot/output/images/rootfs.ext4   mnt-rootfs

sudo cp -f driver/vperiph_driver.ko   mnt-rootfs/root/vperiph_driver.ko

sudo cp -f user/vperiph_cli   mnt-rootfs/root/vperiph_cli

sync
sudo umount mnt-rootfs
```

Guest Linux에서 Driver 로드:

```bash
insmod /root/vperiph_driver.ko
```

로그:

```text
vperiph 90d0000.vperiph: vperiph probe called
vperiph 90d0000.vperiph: registered /dev/vperiph
```

---

## 11. 실행 및 검증

CLI 실행:

```bash
/root/vperiph_cli 10
```

최종 결과:

```text
input=10
result=20
status=1
```

검증된 전체 흐름:

```text
~/projects/qemu-linux/user/vperiph_cli.cpp
└─ main()
   ├─ open("/dev/vperiph")
   ├─ write(DATA=10)
   ├─ ioctl(VPERIPH_START)
   └─ read()
        ↓

~/projects/qemu-linux/driver/vperiph_driver.c
├─ vperiph_write()
│  └─ writel(REG_DATA)
├─ vperiph_ioctl()
│  └─ writel(REG_CONTROL)
└─ vperiph_read()
   ├─ readl(REG_DATA)
   ├─ readl(REG_STATUS)
   └─ copy_to_user()
        ↓

~/projects/qemu-linux/qemu/hw/misc/edu-mmio.c
├─ edu_mmio_write()
│  ├─ DATA = 10
│  ├─ CONTROL = 1
│  ├─ DATA = 20
│  └─ STATUS = 1
└─ edu_mmio_read()
```

---

## 12. 배운 점

Phase 3에서는 Linux Driver가 Kernel 내부에서 장치를 제어하는 것에서 끝나지 않고, User Space에 Character Device 기반 인터페이스를 제공하는 과정까지 구현했다.

역할을 다음과 같이 구분할 수 있었다.

```text
/dev/vperiph
= User Space가 Driver에 접근하는 Character Device Interface

write()
= 입력 데이터 전달

ioctl()
= 장치 제어 명령

read()
= 결과 반환
```

User Space는 QEMU의 물리주소나 Register Map을 직접 알 필요 없이 `/dev/vperiph`만 사용하도록 구조를 분리했다.

Phase 2:

```text
Driver
→ MMIO
→ QEMU Device
```

Phase 3:

```text
User Space
→ Driver
→ MMIO
→ QEMU Device
```

다음 Phase에서는 Hardware Interrupt를 추가하여 polling/즉시 확인 방식이 아니라 장치 완료 이벤트를 Interrupt로 전달하는 구조로 확장한다.
