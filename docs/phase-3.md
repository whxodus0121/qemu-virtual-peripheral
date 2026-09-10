# Phase 3 - User-space Device Control Interface

## 1. 목표

Phase 2에서 Linux Platform Driver가 Device Tree를 통해 `vperiph`의 MMIO Resource를 인식하고, Driver 내부에서 Register를 읽고 쓰는 흐름까지 구현했다.

Phase 3의 목표는 Driver 내부에 있던 테스트 동작을 제거하고, Guest Linux의 User Space에서 `/dev/vperiph`를 통해 장치를 제어할 수 있도록 Character Device Interface를 추가하는 것이다.

기존 구조:

```text
Linux Platform Driver
    ↓
MMIO
    ↓
QEMU vperiph
```

Phase 3 이후 구조:

```text
User-space C++ CLI
    ↓
/dev/vperiph
    ↓
Linux Platform Driver
    ↓
MMIO
    ↓
QEMU vperiph
```

최종적으로 CLI에서 하나의 입력값을 받아 다음 순서를 자동 수행하도록 구현한다.

```text
write()
→ DATA 입력

ioctl()
→ CONTROL = 1
→ 연산 실행

read()
→ DATA / STATUS 결과 확인
```

최종적으로 다음 기능을 구현한다.

- `/dev/vperiph` Character Device 생성
- `write()`를 통한 DATA Register 입력
- `ioctl()`을 통한 장치 제어 명령 전달
- `read()`를 통한 결과 반환
- C++ User-space CLI 구현
- ARM64 Cross Compile
- Guest Linux에서 전체 제어 경로 검증

---

## 2. 배경 개념

### 2.1 Character Device

Linux에서 Character Device는 데이터를 byte stream 형태로 주고받는 Device Interface이다.

User Space에서는 일반 파일과 유사하게:

```text
open()
read()
write()
ioctl()
close()
```

등의 System Call을 사용하지만, 실제로는 파일에 데이터를 저장하는 것이 아니라 Kernel Driver의 `file_operations`에 연결된다.

이번 프로젝트에서는:

```text
/dev/vperiph
```

를 User Space와 `vperiph` Driver 사이의 인터페이스로 사용한다.

### 2.2 `/dev` Device File

`/dev` 아래의 Device File은 일반 파일처럼 데이터를 저장하는 파일이 아니다.

예를 들어:

```text
/dev/vperiph
```

에 `write()`를 수행하면 실제 동작은 다음과 같다.

```text
User Space write()
    ↓
/dev/vperiph
    ↓
Kernel VFS
    ↓
vperiph_write()
    ↓
Driver
```

즉 `/dev/vperiph`는 User Space가 Driver에 접근하기 위한 진입점이다.

### 2.3 `file_operations`

Linux Character Driver는 User Space의 System Call과 Driver 함수를 `file_operations` 구조체를 통해 연결한다.

이번 프로젝트에서는:

```c
static const struct file_operations vperiph_fops = {
    .owner = THIS_MODULE,
    .read = vperiph_read,
    .write = vperiph_write,
    .unlocked_ioctl = vperiph_ioctl,
};
```

를 사용했다.

연결 관계는 다음과 같다.

| User Space | Driver |
|---|---|
| `read()` | `vperiph_read()` |
| `write()` | `vperiph_write()` |
| `ioctl()` | `vperiph_ioctl()` |

### 2.4 miscdevice

Character Device를 등록하는 방법 중 하나는 `register_chrdev()` 등을 이용해 Major/Minor Number와 Device File 생성을 직접 관리하는 것이다.

이번 프로젝트에서는 단순한 하나의 Character Device Interface만 필요했기 때문에 `miscdevice`를 사용했다.

```c
struct miscdevice miscdev;
```

`miscdevice`는 Dynamic Minor Number 할당과 Device File 등록을 간단하게 처리할 수 있다.

### 2.5 User Space와 Kernel Space

User Space Pointer를 Kernel에서 직접 역참조해서는 안 된다.

따라서 User Space와 Kernel Space 사이의 데이터 이동에는 다음 API를 사용한다.

```text
copy_from_user()
= User Space → Kernel Space

copy_to_user()
= Kernel Space → User Space
```

이번 Phase에서는:

```text
write()
→ copy_from_user()

read()
→ copy_to_user()
```

구조를 사용했다.

---

## 3. 왜 필요한가

Phase 2까지는 Driver가 정상적으로 QEMU Peripheral의 MMIO Register에 접근할 수 있었지만, Register 동작 검증을 Driver의 `probe()` 내부에서 직접 수행했다.

즉:

```text
Driver Load
    ↓
probe()
    ↓
MMIO Register Write
    ↓
결과 확인
```

형태였다.

이 방식은 Driver와 Device 연결을 검증하기에는 충분하지만 실제 Application이 장치를 사용하는 구조는 아니다.

일반적인 Linux Device Control 구조에서는 User Space Application이 Driver가 제공하는 Interface를 사용하고, Hardware의 Physical Address나 Register Map은 Driver 내부에 숨겨진다.

```text
Application
    ↓
Device File
    ↓
Driver
    ↓
Hardware Register
```

따라서 Phase 3에서는 User Space가 QEMU의 Physical Address를 직접 알지 않고:

```text
/dev/vperiph
```

만을 통해 장치를 사용할 수 있도록 구조를 분리한다.

이를 통해:

```text
User Application
Kernel Driver
Virtual Hardware
```

각 계층의 역할을 명확하게 나누는 것을 목표로 했다.

---

## 4. 시스템 구조

전체 구조는 다음과 같다.

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

각 계층의 역할은 다음과 같다.

```text
C++ CLI
= User 입력 및 결과 출력

/dev/vperiph
= User Space ↔ Driver Interface

Linux Driver
= System Call 처리 및 MMIO 접근

QEMU vperiph
= 실제 Register 동작을 구현한 Virtual Peripheral
```

---

## 5. 구현 과정

### 5.1 miscdevice 추가

파일:

```text
~/projects/qemu-linux/driver/vperiph_driver.c
```

기존 `vperiph_dev`에 `miscdevice`를 추가했다.

```c
struct vperiph_dev {
    void __iomem *base;
    struct miscdevice miscdev;
};
```

`vperiph_probe()`에서 Device File 정보를 설정했다.

```c
vdev->miscdev.minor = MISC_DYNAMIC_MINOR;
vdev->miscdev.name = "vperiph";
vdev->miscdev.fops = &vperiph_fops;
vdev->miscdev.parent = &pdev->dev;
```

이후:

```c
ret = misc_register(&vdev->miscdev);
if (ret)
    return ret;
```

를 호출하여 Character Device를 등록했다.

Guest Linux에서:

```text
/dev/vperiph
```

가 생성된다.

실제 확인 결과:

```text
crw------- 1 root root 10, 259 /dev/vperiph
```

앞의 `c`는 Character Device를 의미한다.

### 5.2 write() 구현

파일:

```text
~/projects/qemu-linux/driver/vperiph_driver.c
```

함수:

```text
vperiph_write()
```

User Space에서 전달한 32bit 값을 DATA Register에 기록한다.

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

### 5.3 ioctl() 구현

입력 데이터 전달과 장치 제어 명령을 구분하기 위해 START 명령은 `ioctl()`로 구현했다.

```c
#define VPERIPH_IOC_MAGIC 'v'
#define VPERIPH_START _IO(VPERIPH_IOC_MAGIC, 0)
```

파일:

```text
~/projects/qemu-linux/driver/vperiph_driver.c
```

함수:

```text
vperiph_ioctl()
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

### 5.4 read() 구현

연산 결과를 User Space로 반환하기 위해 DATA와 STATUS를 함께 전달하는 구조체를 정의했다.

```c
struct vperiph_result {
    u32 data;
    u32 status;
};
```

파일:

```text
~/projects/qemu-linux/driver/vperiph_driver.c
```

함수:

```text
vperiph_read()
```

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
User-space Application
```

### 5.5 file_operations 연결

구현한 Driver 함수를 User Space System Call과 연결했다.

```c
static const struct file_operations vperiph_fops = {
    .owner = THIS_MODULE,
    .read = vperiph_read,
    .write = vperiph_write,
    .unlocked_ioctl = vperiph_ioctl,
};
```

연결 관계:

```text
write()
→ vperiph_write()

ioctl()
→ vperiph_ioctl()

read()
→ vperiph_read()
```

### 5.6 C++ User-space CLI 구현

파일:

```text
~/projects/qemu-linux/user/vperiph_cli.cpp
```

CLI는 하나의 입력값을 받아 장치 제어 순서를 자동 수행하도록 구현했다.

```text
1. /dev/vperiph open
2. write()로 DATA 입력
3. ioctl()로 START 명령
4. read()로 DATA / STATUS 확인
5. 결과 출력
```

결과 구조체:

```cpp
struct VperiphResult {
    std::uint32_t data;
    std::uint32_t status;
};
```

핵심 흐름:

```cpp
write(fd, &value, sizeof(value));

ioctl(fd, VPERIPH_START);

VperiphResult result{};
read(fd, &result, sizeof(result));

std::cout << "input=" << value << '\n';
std::cout << "result=" << result.data << '\n';
std::cout << "status=" << result.status << '\n';
```

따라서 User가 `read`, `write`, `ioctl` 중 하나를 직접 선택하는 구조가 아니라 CLI 한 번 실행으로 전체 Device Control Sequence가 수행된다.

### 5.7 ARM64 Cross Compile

C++ CLI를 Guest ARM64 Linux에서 실행하기 위해 Buildroot Toolchain의 C++ 지원을 활성화했다.

```text
BR2_TOOLCHAIN_BUILDROOT_CXX=y
```

사용한 Cross Compiler:

```text
~/projects/qemu-linux/buildroot/output/host/bin/
aarch64-buildroot-linux-gnu-g++
```

빌드:

```bash
~/projects/qemu-linux/buildroot/output/host/bin/aarch64-buildroot-linux-gnu-g++ \
  user/vperiph_cli.cpp \
  -o user/vperiph_cli
```

확인 결과:

```text
ELF 64-bit LSB pie executable, ARM aarch64
```

가 생성되었다.

### 5.8 Guest RootFS에 배치

QEMU를 종료한 상태에서 Buildroot Root Filesystem Image를 Host에서 Mount하고 Driver와 CLI를 복사했다.

```bash
sudo mount -o loop \
  buildroot/output/images/rootfs.ext4 \
  mnt-rootfs

sudo cp -f driver/vperiph_driver.ko \
  mnt-rootfs/root/vperiph_driver.ko

sudo cp -f user/vperiph_cli \
  mnt-rootfs/root/vperiph_cli

sync

sudo umount mnt-rootfs
```

Guest Linux에서 Driver를 로드했다.

```bash
insmod /root/vperiph_driver.ko
```

로그:

```text
vperiph 90d0000.vperiph: vperiph probe called
vperiph 90d0000.vperiph: registered /dev/vperiph
```

---

## 6. 핵심 코드

Phase 3의 핵심은 User Space System Call을 Driver의 MMIO Access와 연결하는 것이다.

```c
static const struct file_operations vperiph_fops = {
    .owner = THIS_MODULE,
    .read = vperiph_read,
    .write = vperiph_write,
    .unlocked_ioctl = vperiph_ioctl,
};
```

DATA 입력:

```c
copy_from_user(&value, buf, sizeof(value));
writel(value, vdev->base + REG_DATA);
```

장치 실행:

```c
case VPERIPH_START:
    writel(1, vdev->base + REG_CONTROL);
    return 0;
```

결과 반환:

```c
result.data = readl(vdev->base + REG_DATA);
result.status = readl(vdev->base + REG_STATUS);

copy_to_user(buf, &result, sizeof(result));
```

User-space CLI에서는:

```cpp
write(fd, &value, sizeof(value));
ioctl(fd, VPERIPH_START);
read(fd, &result, sizeof(result));
```

을 순서대로 호출한다.

전체 흐름:

```text
CLI
write(DATA=10)
    ↓
Driver
writel(REG_DATA)
    ↓
QEMU
DATA = 10

CLI
ioctl(START)
    ↓
Driver
writel(REG_CONTROL)
    ↓
QEMU
DATA = 20
STATUS = 1

CLI
read()
    ↓
Driver
readl(DATA / STATUS)
    ↓
copy_to_user()
    ↓
result=20
status=1
```

---

## 7. 실행 및 검증

### 7.1 Character Device 생성 확인

Driver를 로드한 뒤:

```bash
ls -l /dev/vperiph
```

결과:

```text
crw------- 1 root root 10, 259 /dev/vperiph
```

를 통해 Character Device가 정상적으로 생성된 것을 확인했다.

### 7.2 write() 경로 검증

Shell에서 32bit 값 `10`을 `/dev/vperiph`에 전달했다.

```bash
printf '\x0a\x00\x00\x00' > /dev/vperiph
```

이후 DATA Register를 직접 확인했다.

```bash
devmem 0x090d0008 32
```

결과:

```text
0x0000000A
```

이를 통해:

```text
User Space
→ /dev/vperiph
→ vperiph_write()
→ MMIO
→ QEMU DATA Register
```

경로가 정상 동작함을 확인했다.

### 7.3 C++ CLI 전체 흐름 검증

CLI 실행:

```bash
/root/vperiph_cli 10
```

결과:

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

이를 통해 User Space가 QEMU의 Physical Address를 직접 사용하지 않고 `/dev/vperiph`만으로 Virtual Peripheral을 제어할 수 있음을 확인했다.

---

## 8. 배운 점

Phase 3에서는 Linux Driver가 Kernel 내부에서 MMIO Register를 제어하는 것에서 끝나지 않고, User Space Application이 사용할 수 있는 Character Device Interface까지 구현했다.

기존 Phase 2의 구조:

```text
Linux Driver
    ↓
MMIO
    ↓
QEMU Device
```

에서:

```text
User Space
    ↓
/dev/vperiph
    ↓
Linux Driver
    ↓
MMIO
    ↓
QEMU Device
```

로 확장했다.

특히 다음 역할을 구분할 수 있었다.

```text
/dev/vperiph
= User Space가 Driver에 접근하는 Device Interface

file_operations
= System Call과 Driver 함수 연결

copy_from_user()
= User Space 입력을 Kernel로 전달

copy_to_user()
= Kernel 결과를 User Space로 반환

write()
= DATA 입력

ioctl()
= Device Control Command

read()
= DATA / STATUS 결과 반환

miscdevice
= Character Device 등록과 Minor Number 관리를 단순화
```

또한 User Space Application이 Hardware의 Physical Address나 Register Map을 직접 알지 않고 Driver가 제공하는 Interface만 사용하도록 계층을 분리했다.

```text
Application
→ Device Interface
→ Driver
→ Hardware
```

라는 Linux Device Control 구조를 직접 구현하고 검증했다.

다음 Phase에서는 QEMU `vperiph`가 연산 완료 이벤트를 Hardware Interrupt로 Linux Driver에 전달하도록 확장한다.
