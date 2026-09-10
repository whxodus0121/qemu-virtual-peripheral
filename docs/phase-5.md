# Phase 5 - Buildroot / BSP Integration

## 1. 목표

Phase 4까지는 QEMU `vperiph`, Linux Platform Driver, User-space CLI, Hardware Interrupt까지 기능을 구현하고 검증했다.

하지만 Driver와 CLI를 Guest Linux에 넣기 위해 다음 작업을 수동으로 수행해야 했다.

```text
Driver 직접 Cross Compile
        ↓
CLI 직접 Cross Compile
        ↓
RootFS Image Mount
        ↓
Driver / CLI 수동 복사
        ↓
QEMU Boot
        ↓
modprobe 수동 실행
```

Phase 5의 목표는 이 과정을 Buildroot에 통합하여, Buildroot의 빌드 결과만으로 Driver와 CLI가 포함된 Root Filesystem을 생성하고 부팅 시 Driver가 자동으로 로드되도록 만드는 것이다.

최종 흐름:

```text
Buildroot make
        ↓
vperiph CLI Cross Compile
        ↓
vperiph Driver Kernel Module Build
        ↓
RootFS 자동 설치
        ↓
QEMU Boot
        ↓
Driver 자동 Load
        ↓
/dev/vperiph 자동 생성
        ↓
vperiph_cli 실행
```

최종적으로 다음 기능을 구현한다.

- `BR2_EXTERNAL` 기반 프로젝트 전용 Buildroot Tree 구성
- `vperiph-cli` Buildroot Package 등록
- `vperiph-driver` Kernel Module Package 등록
- RootFS Overlay를 이용한 Init Script 포함
- 부팅 시 Driver 자동 로드
- 프로젝트용 `defconfig` 저장
- Clean Build를 통한 재현성 검증

---

## 2. 배경 개념

### 2.1 Buildroot

Buildroot는 Embedded Linux System을 구성하기 위한 Build System이다.

Target Architecture와 필요한 Package를 설정하면 Cross Toolchain, Linux Kernel, User-space Program, Library, Root Filesystem 등을 하나의 빌드 과정에서 생성할 수 있다.

이번 프로젝트에서는 기존에 이미 Buildroot를 이용해 ARM64 Linux Kernel과 Root Filesystem을 생성하고 있었으며, Phase 5에서는 직접 만든 Driver와 CLI도 Buildroot 관리 대상으로 통합한다.

### 2.2 Buildroot Package

Buildroot에서는 프로그램이나 라이브러리의 빌드 및 설치 방법을 Package 단위로 정의한다.

일반적으로 다음 두 파일을 사용한다.

```text
Config.in
= menuconfig에서 Package를 선택할 수 있도록 정의

<package-name>.mk
= Source 위치, Build 방법, Target 설치 방법 정의
```

이번 Phase에서는 다음 두 Package를 추가한다.

```text
vperiph-cli
vperiph-driver
```

### 2.3 BR2_EXTERNAL

`BR2_EXTERNAL`은 Buildroot 본체를 직접 수정하지 않고 외부 프로젝트의 Package, Board 설정, Config 등을 별도 Tree에서 관리하기 위한 기능이다.

이번 프로젝트에서는:

```text
buildroot/
= Buildroot 원본

br2-external/
= vperiph 전용 Buildroot 설정
```

으로 분리한다.

이를 통해 Buildroot Source와 프로젝트 전용 설정을 섞지 않고 관리할 수 있다.

### 2.4 output/target

Buildroot의:

```text
output/target/
```

은 최종 Root Filesystem Image를 생성하기 전의 Target File System Tree이다.

예를 들어 Phase 5에서 CLI가 정상적으로 설치되면:

```text
output/target/usr/bin/vperiph_cli
```

가 생성되고, Kernel Module은:

```text
output/target/lib/modules/<kernel-version>/...
```

아래에 설치된다.

이후 이 내용을 기반으로 `rootfs.ext4`가 생성된다.

### 2.5 RootFS Overlay

RootFS Overlay는 Buildroot가 생성하는 기본 Root Filesystem 위에 프로젝트 전용 파일을 추가하기 위한 구조이다.

이번 프로젝트에서는 부팅 시 Driver를 자동으로 로드하는 Init Script를:

```text
/etc/init.d/S50vperiph
```

로 포함하기 위해 사용한다.

### 2.6 Out-of-tree Kernel Module

`vperiph_driver.c`는 Linux Kernel Source의 `drivers/` 내부에 추가한 Driver가 아니라 프로젝트 외부에 존재한다.

```text
~/projects/qemu-linux/driver/
├── Makefile
└── vperiph_driver.c
```

따라서 Out-of-tree Kernel Module 형태이다.

Buildroot의 `kernel-module` Infrastructure를 사용하면 현재 Buildroot가 빌드하는 Linux Kernel과 Toolchain을 기준으로 이 Module을 자동으로 빌드하고 RootFS에 설치할 수 있다.

### 2.7 defconfig

Buildroot의 `.config`에는 매우 많은 설정이 포함된다.

`defconfig`는 현재 Target System을 다시 구성하는 데 필요한 주요 설정만 저장한 최소 Configuration이다.

이번 Phase에서는 `vperiph_defconfig`를 저장하여 Buildroot Output을 삭제한 뒤에도 같은 Target Configuration을 다시 구성할 수 있도록 한다.

---

## 3. 왜 필요한가

Phase 4까지 기능 자체는 모두 동작했지만 Build 결과를 Guest Linux에 반영하는 과정은 수동이었다.

```text
Kernel Module Build
        ↓
CLI Build
        ↓
rootfs.ext4 Mount
        ↓
파일 직접 Copy
        ↓
Unmount
        ↓
QEMU Boot
        ↓
modprobe
```

이 방식은 기능 검증에는 사용할 수 있지만 다음 문제가 있다.

```text
어떤 Binary를 RootFS에 넣었는지 수동 관리해야 함
Build 절차를 반복할 때 누락 가능성이 있음
다른 환경에서 동일한 System을 만들기 어려움
Boot 이후 추가 설정이 필요함
```

Embedded Linux System에서는 Driver, Application, RootFS 설정을 하나의 Build Configuration으로 관리하는 것이 중요하다.

따라서 Phase 5에서는:

```text
개별 기능의 수동 검증
```

에서:

```text
Target Linux Image 단위의 System Integration
```

으로 프로젝트를 마무리한다.

---

## 4. 시스템 구조

Phase 5에서 추가된 Buildroot 구조는 다음과 같다.

```text
~/projects/qemu-linux/

├── driver/
│   ├── Makefile
│   └── vperiph_driver.c
│
├── user/
│   └── vperiph_cli.cpp
│
├── buildroot/
│
└── br2-external/
    ├── external.desc
    ├── external.mk
    ├── Config.in
    │
    ├── configs/
    │   └── vperiph_defconfig
    │
    ├── package/
    │   ├── vperiph-cli/
    │   │   ├── Config.in
    │   │   └── vperiph-cli.mk
    │   │
    │   └── vperiph-driver/
    │       ├── Config.in
    │       └── vperiph-driver.mk
    │
    └── board/
        └── vperiph/
            └── rootfs-overlay/
                └── etc/
                    └── init.d/
                        └── S50vperiph
```

Build 흐름:

```text
BR2_EXTERNAL
        ↓
br2-external
        │
        ├── vperiph-cli Package
        │        ↓
        │    ARM64 C++ Binary
        │
        ├── vperiph-driver Package
        │        ↓
        │    ARM64 Kernel Module
        │
        └── RootFS Overlay
                 ↓
             Init Script
        ↓
output/target
        ↓
rootfs.ext4
        ↓
QEMU Boot
```

---

## 5. 구현 과정

### 5.1 BR2_EXTERNAL Tree 구성

프로젝트 전용 Buildroot 설정을 Buildroot Source와 분리하기 위해:

```text
~/projects/qemu-linux/br2-external
```

을 생성했다.

`external.desc`:

```text
name: VPERIPH
desc: QEMU vperiph external Buildroot tree
```

`name: VPERIPH`를 통해 Buildroot에서:

```text
BR2_EXTERNAL_VPERIPH_PATH
```

변수를 사용할 수 있다.

`external.mk`:

```makefile
include $(sort $(wildcard $(BR2_EXTERNAL_VPERIPH_PATH)/package/*/*.mk))
```

`br2-external/package/` 아래의 Package Makefile을 Buildroot 빌드 시스템에 포함한다.

최상위 `Config.in`:

```text
menu "VPERIPH packages"

source "$BR2_EXTERNAL_VPERIPH_PATH/package/vperiph-driver/Config.in"
source "$BR2_EXTERNAL_VPERIPH_PATH/package/vperiph-cli/Config.in"

endmenu
```

이를 통해 `menuconfig`의 External options에서 프로젝트 Package를 선택할 수 있도록 했다.

### 5.2 vperiph-cli Package

파일:

```text
br2-external/package/vperiph-cli/Config.in
```

```text
config BR2_PACKAGE_VPERIPH_CLI
    bool "vperiph-cli"
    depends on BR2_INSTALL_LIBSTDCPP
    help
      C++ user-space CLI for controlling the QEMU vperiph device.

comment "vperiph-cli needs a toolchain with C++ support"
    depends on !BR2_INSTALL_LIBSTDCPP
```

CLI는 C++로 작성했기 때문에 C++ Standard Library가 활성화된 Toolchain에서만 선택할 수 있도록 했다.

파일:

```text
br2-external/package/vperiph-cli/vperiph-cli.mk
```

```makefile
VPERIPH_CLI_VERSION = 1.0
VPERIPH_CLI_SITE = $(BR2_EXTERNAL_VPERIPH_PATH)/../user
VPERIPH_CLI_SITE_METHOD = local

define VPERIPH_CLI_BUILD_CMDS
	$(TARGET_CXX) $(TARGET_CXXFLAGS) \
		$(@D)/vperiph_cli.cpp \
		-o $(@D)/vperiph_cli \
		$(TARGET_LDFLAGS)
endef

define VPERIPH_CLI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 \
		$(@D)/vperiph_cli \
		$(TARGET_DIR)/usr/bin/vperiph_cli
endef

$(eval $(generic-package))
```

기존에는 ARM64 Compiler를 직접 호출했지만 이제 Buildroot의:

```text
$(TARGET_CXX)
```

를 사용한다.

흐름:

```text
user/vperiph_cli.cpp
        ↓
Buildroot TARGET_CXX
        ↓
ARM64 vperiph_cli
        ↓
output/target/usr/bin/vperiph_cli
```

### 5.3 vperiph-driver Package

파일:

```text
br2-external/package/vperiph-driver/Config.in
```

```text
config BR2_PACKAGE_VPERIPH_DRIVER
    bool "vperiph-driver"
    depends on BR2_LINUX_KERNEL
    help
      Linux platform driver for the QEMU vperiph device.

comment "vperiph-driver needs a Linux kernel to be built"
    depends on !BR2_LINUX_KERNEL
```

파일:

```text
br2-external/package/vperiph-driver/vperiph-driver.mk
```

```makefile
VPERIPH_DRIVER_VERSION = 1.0
VPERIPH_DRIVER_SITE = $(BR2_EXTERNAL_VPERIPH_PATH)/../driver
VPERIPH_DRIVER_SITE_METHOD = local

$(eval $(kernel-module))
$(eval $(generic-package))
```

기존에는 다음과 같이 Kernel Build Directory, Architecture, Cross Compiler를 직접 지정했다.

```text
make -C <linux-build-dir>
M=<driver-dir>
ARCH=arm64
CROSS_COMPILE=<toolchain>
modules
```

Phase 5에서는 Buildroot `kernel-module` Infrastructure가 해당 정보를 현재 Kernel Build 환경에 맞춰 처리한다.

생성된 Module:

```text
output/target/lib/modules/6.18.7/updates/vperiph_driver.ko
```

### 5.4 RootFS Overlay 구성

부팅 시 필요한 프로젝트 전용 Script를 RootFS에 포함하기 위해 다음 Overlay를 구성했다.

```text
br2-external/board/vperiph/rootfs-overlay/
└── etc/
    └── init.d/
        └── S50vperiph
```

Buildroot의 Root Filesystem Overlay 설정은 External Tree를 기준으로 지정했다.

```text
$(BR2_EXTERNAL_VPERIPH_PATH)/board/vperiph/rootfs-overlay
```

이를 통해 개발 환경의 절대경로에 의존하지 않도록 구성했다.

### 5.5 Driver 자동 로드

파일:

```text
br2-external/board/vperiph/rootfs-overlay/etc/init.d/S50vperiph
```

```sh
#!/bin/sh

case "$1" in
    start)
        echo "Loading vperiph driver..."
        modprobe vperiph_driver
        ;;

    stop)
        echo "Unloading vperiph driver..."
        modprobe -r vperiph_driver
        ;;

    restart)
        "$0" stop
        "$0" start
        ;;

    *)
        echo "Usage: $0 {start|stop|restart}"
        exit 1
        ;;
esac

exit 0
```

부팅 흐름:

```text
Linux Boot
        ↓
/sbin/init
        ↓
/etc/init.d/rcS
        ↓
S50vperiph start
        ↓
modprobe vperiph_driver
        ↓
vperiph_probe()
        ↓
/dev/vperiph 생성
```

Phase 4까지는 Guest에 로그인한 후 직접 `modprobe`를 실행했지만 Phase 5에서는 부팅 과정에서 자동으로 Driver가 로드된다.

### 5.6 프로젝트 defconfig 저장

현재 Buildroot Configuration을 프로젝트 전용 Defconfig로 저장했다.

```text
br2-external/configs/vperiph_defconfig
```

이를 이용하면 Buildroot Output을 삭제한 뒤에도:

```bash
make BR2_EXTERNAL=../br2-external vperiph_defconfig
```

로 프로젝트 Configuration을 다시 구성할 수 있다.

### 5.7 Clean Build 재현성 검증

기존 Build Output에 의존하지 않는지 확인하기 위해 Buildroot Output을 정리한 뒤 저장한 Defconfig로 다시 빌드했다.

```text
기존 Output 제거
        ↓
vperiph_defconfig 적용
        ↓
Buildroot make
        ↓
Kernel / RootFS 재생성
        ↓
QEMU Boot
        ↓
Driver 자동 Load
        ↓
CLI / IRQ 검증
```

Clean Build 이후에도 동일한 동작을 확인했다.

---

## 6. 핵심 코드

Phase 5의 핵심은 Driver와 CLI를 Buildroot의 Build Graph에 포함하고 Target RootFS에 자동 설치하는 것이다.

CLI Package:

```makefile
VPERIPH_CLI_SITE = $(BR2_EXTERNAL_VPERIPH_PATH)/../user
VPERIPH_CLI_SITE_METHOD = local

define VPERIPH_CLI_BUILD_CMDS
	$(TARGET_CXX) $(TARGET_CXXFLAGS) \
		$(@D)/vperiph_cli.cpp \
		-o $(@D)/vperiph_cli \
		$(TARGET_LDFLAGS)
endef

define VPERIPH_CLI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 \
		$(@D)/vperiph_cli \
		$(TARGET_DIR)/usr/bin/vperiph_cli
endef

$(eval $(generic-package))
```

Kernel Module Package:

```makefile
VPERIPH_DRIVER_SITE = $(BR2_EXTERNAL_VPERIPH_PATH)/../driver
VPERIPH_DRIVER_SITE_METHOD = local

$(eval $(kernel-module))
$(eval $(generic-package))
```

Boot-time Driver Load:

```sh
start)
    echo "Loading vperiph driver..."
    modprobe vperiph_driver
    ;;
```

결과적으로:

```text
Buildroot
├── CLI Build
├── Kernel Module Build
├── Target Install
├── RootFS Overlay
└── RootFS Image 생성
```

이 하나의 과정으로 통합되었다.

---

## 7. 실행 및 검증

### 7.1 CLI 자동 설치 확인

Guest Linux에서:

```bash
which vperiph_cli
```

결과:

```text
/usr/bin/vperiph_cli
```

Host Buildroot Output에서도 ARM64 Binary임을 확인했다.

```text
ELF 64-bit LSB pie executable, ARM aarch64
```

### 7.2 Kernel Module 자동 설치 확인

Guest Linux:

```bash
find /lib/modules -name 'vperiph_driver.ko'
```

결과:

```text
/lib/modules/6.18.7/updates/vperiph_driver.ko
```

수동으로 `.ko` 파일을 RootFS에 복사하지 않아도 Buildroot가 Module Directory에 설치함을 확인했다.

### 7.3 Boot-time Driver Load 확인

부팅 로그:

```text
Loading vperiph driver...
vperiph_driver: loading out-of-tree module taints kernel.
vperiph 90d0000.vperiph: vperiph probe called
vperiph 90d0000.vperiph: registered /dev/vperiph
```

로그인 후 별도 `modprobe` 없이:

```bash
lsmod
```

결과:

```text
Module                  Size  Used by
vperiph_driver         12288  0
```

Device File:

```bash
ls -l /dev/vperiph
```

결과:

```text
crw------- 1 root root 10, 259 /dev/vperiph
```

### 7.4 전체 동작 검증

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

이를 통해 Clean Build 이후에도:

```text
Buildroot Package
        ↓
RootFS
        ↓
Boot
        ↓
Driver Auto Load
        ↓
Device Tree Match
        ↓
MMIO
        ↓
QEMU vperiph
        ↓
Hardware IRQ
        ↓
Linux IRQ Handler
        ↓
User-space Result
```

전체 경로가 정상 동작함을 확인했다.

---

## 8. 배운 점

Phase 5에서는 지금까지 개별적으로 구현한 Driver와 User-space Application을 Buildroot 기반 Embedded Linux Image에 통합했다.

Phase 4까지:

```text
Source
→ 수동 Build
→ 수동 RootFS Copy
→ Boot
→ 수동 Driver Load
```

Phase 5:

```text
Source
→ Buildroot
→ 자동 Cross Compile
→ 자동 RootFS Install
→ Boot
→ 자동 Driver Load
```

로 변경했다.

각 요소의 역할을 다음과 같이 구분할 수 있었다.

```text
BR2_EXTERNAL
= Buildroot 원본과 프로젝트 전용 설정 분리

Config.in
= Package 선택 Configuration 정의

.mk
= Package의 Source / Build / Install 방법 정의

generic-package
= 일반 User-space Package Build Infrastructure

kernel-module
= Out-of-tree Kernel Module Build Infrastructure

output/target
= 최종 RootFS Image 생성 전 Target File System

RootFS Overlay
= 프로젝트 전용 설정과 Init Script 추가

modprobe
= /lib/modules 아래의 Module을 Module 관리 체계를 통해 Load

defconfig
= 동일 Target Configuration을 다시 구성하기 위한 최소 설정
```

Phase 5를 통해 프로젝트가 단순히 개별 기능을 수동으로 검증하는 상태에서 벗어나:

```text
QEMU Virtual Hardware
        ↓
Device Tree
        ↓
Linux Platform Driver
        ↓
Hardware Interrupt
        ↓
User-space Interface
        ↓
Buildroot Linux Image
```

까지 하나의 System으로 통합되었다.

저장된 Buildroot Configuration을 이용한 Clean Build 이후에도 Driver 자동 로드와 CLI/IRQ 동작을 다시 확인하여 현재 개발 환경에서 Build 과정의 재현성을 검증했다.
