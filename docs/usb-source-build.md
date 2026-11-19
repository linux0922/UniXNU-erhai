# 尔海 USB 版的源码构建准备

目标机型为 OnePlus Pad 2 Pro OPD2413（erhai）。已实测的版本通过 Fastboot 临时启动后，用 USB 从电脑读取系统卷，内屏能从锁屏上滑进入主屏幕，并能横滑和点按。当前仓库可准备供盘材料、生成 Android DTB、来宾 ADT 和 RAM Data、编译板级 UEFI、打包本地组件并运行 USB 服务。完整启动镜像仍需要补齐 USB／触控内核的公开构建入口，并做整链实机验证。

## 交给 AI 或自己操作

把仓库首页发给 AI 时，可以附上这句话。

> 按 main 的 AGENTS.md 和 USB 构建说明，在我的 OPD2413 上复现 iPadOS 17.7.11 全 USB 临时启动。先核对构建输入，重启前问我。验收到内屏能从锁屏上滑进入主屏幕，并能横滑和点按。

原上游的 S1–S6 开机检查点属于不同的启动配置。这次的验收顺序是 USB 供盘开始、锁屏出现时间和底部横条、实际手指上滑进主屏幕、横滑和点按设置。等待期间在电脑端观察 USB 读取进度。

## 生成 USB 供盘材料

准备 Python 3.11+、Git、OpenSSL，以及 UniXNU 的 UDIF 解码依赖 LZFSE。使用 Linux 或 WSL，把缓存和输出放在同一文件系统上，SystemOS 通过硬链接从缓存复用。首次解包时还要留出 OS DMG 和约 6.08 GB 的解码输出空间。

```sh
git clone https://github.com/UniXNU/UniXNU.git ../UniXNU
git -C ../UniXNU checkout --detach c1ecc5b74c9dc4ced13bcfbd81a2c3998025a5e5
git clone https://github.com/IssacL04/UniXNU-Pagani-Drivers.git ../UniXNU-Pagani-Drivers
git -C ../UniXNU-Pagani-Drivers checkout --detach c888efd3ce1b817d13f28783a77cf689cb2255c9
mkdir -p ../erhai-local-inputs
(umask 077; openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out ../erhai-local-inputs/owner.pem)
python3 scripts/prepare-usb-inputs.py \
  --unixnu ../UniXNU --pagani-drivers ../UniXNU-Pagani-Drivers \
  --cache ../erhai-local-inputs/cache --output ../erhai-local-inputs/usb \
  --private-key ../erhai-local-inputs/owner.pem
```

密钥生成命令只在第一次执行。后续构建复用同一份私钥，保留在本机。脚本默认从 [苹果 CDN](https://updates.cdn-apple.com/2025SpringFCS/fullrestores/122-77166/DCA6CFE8-90E9-4B92-880D-3DE171798E40/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw) 提取 iPad7,1 的 iPadOS 17.7.11（21H461）组件。已有恢复包时可加 `--ipsw /path/to/restore.ipsw`。已有解码后的 OS 时可用 `--os-raw` 复用同一文件系统上的文件，脚本会校验内容。

USB 服务读取 `OS.raw`、`SystemOS.dmg`、`OS.manifest.bin` 和 `SystemOS.manifest.bin`。两份清单由本地 RSA 密钥签发。启动镜像的构建步骤要读取同一目录中的 `owner-public.pem` 与两份清单，校验签名，并把清单摘要和分页索引编进内核。USB 服务需要使用那次构建对应的清单。`trusted-key.bin` 是清单工具输出的公钥锚。脚本也会从同一份 IPSW 生成 `original-static-system.bundle`，用于打包启动镜像。

## 生成 RAM Data 和来宾 ADT

在 macOS 上运行下列命令。用准备供盘材料时缓存的原始压缩 OS DMG，路径里的 `CACHE` 指向该缓存，`BUILD` 指向本地构建目录。可以在 macOS 上访问同一份缓存，也可以使用自己的 IPSW 提取缓存。RAM Data 脚本挂载原始系统卷时使用只读模式。

```sh
sudo python3 scripts/prepare-ram-data.py \
  --source "$CACHE/ipsw/21H461/044-76040-038.dmg" \
  --output "$BUILD/ram-data" \
  --packer ../UniXNU/drivers/DarwinBoot/Test/packed_ramdisk.py
```

将 `ram-data/seed.json` 的 `volume_uuid` 写进本次内核构建配置的 `CONFIG_XNU_OWNER_DATA_VOLUME_UUID`。重新生成 RAM Data 时采用新 UUID，并用对应的 `ram-data/data.dbrdisk` 打包。原始 APFS 种子由 macOS 生成，公开 UniXNU 打包器已用实测种子复现逐字节相同的载荷。此处的新入口尚待用干净 macOS 环境跑一遍。

来宾 ADT 可在任意有 Python 3 的主机上生成。`CHIP_ECID` 是在自己的 Android 上用 `adb shell getprop ro.boot.chipecid` 读到的本机标识，构建时留在本地。下列参数生成的属性已与实测 USB 版逐项对照，设备标识和随机种子按轮次生成。

```sh
python3 scripts/make-erhai-adt.py --output "$BUILD/kernel.macho.adt" \
  --dram-size 0 --owned-memory-size 0xc0000000 --cpu-regs 0 \
  --unique-chip-id "$CHIP_ECID" --chip-id 0x207 --ipad-identity \
  --packed-data-ramdisk --omit-ephemeral-storage-property --trust-cache \
  --root-match-volume-uuid 04769D26-2CA0-4825-B443-E2E5F88300F2
```

## 提取 Android DTB

从 [同机型 Linux 项目的 v0.6.0 发布页](https://github.com/yzddmr6/onepluspad-2pro-mainline/releases/tag/v0.6.0) 取得 [`ubuntu-pad2pro-v0.6.0-setup-boot.img`](https://github.com/yzddmr6/onepluspad-2pro-mainline/releases/download/v0.6.0/ubuntu-pad2pro-v0.6.0-setup-boot.img)，作为文件输入运行。

```sh
python3 scripts/prepare-erhai-dtb.py \
  --setup-boot /path/to/ubuntu-pad2pro-v0.6.0-setup-boot.img \
  --output "$BUILD/erhai.dtb"
```

脚本校验发布文件的哈希，提取内嵌 DTB，并将超出本次实测机型内存范围的固定 ramoops 保留区替换为 FDT_NOP。输出与实测 USB 版用的 Android DTB 逐字节相同，SHA-256 为 `9201ea6c07983a8c9fc74d3414bf7fcea46aa200f32b20f942bb4fc64a99133c`。

## 编译和链接 USB／触控内核组件

以下步骤适用于已完成 XNU 引导编译、并有 21H461 存储 ABI 审查结果的构建树。引导编译和存储审查还需要接入公开入口。以已准备的源码树为 `XNU_TREE`，以本地 USB 输入目录为 `USB_INPUTS`。用上文固定的 UniXNU 检出初始化 `drivers/QcomGeniSpi` 和 `kits/ArmSmmu4K` 子模块，并取得这两份原始源码文件。

- [Pagani 编译辅助脚本](https://raw.githubusercontent.com/IssacL04/UniXNU-Pagani-Drivers/c888efd3ce1b817d13f28783a77cf689cb2255c9/drivers/PaganiBattery/scripts/build-pagani-battery.py)
- [NativeKeyStoreEvents.cpp](https://raw.githubusercontent.com/UniXNU/NativeKeyStoreEvents/e59bac4ad1ac8825919ae94da8440a64d8ab90b3/NativeKeyStoreEvents.cpp)

```sh
python3 scripts/build-usb-kernel-objects.py \
  --tree "$XNU_TREE" --unixnu ../UniXNU --clang "$LLVM_CLANG" \
  --compile-helper "$INPUTS/build-pagani-battery.py" \
  --keystore-source "$INPUTS/NativeKeyStoreEvents.cpp" \
  --touch-firmware "$INPUTS/nt36536-xn242.bin" \
  --storage-audit "$BUILD/storage-audit.json" --usb-inputs "$USB_INPUTS" \
  --output "$BUILD/usb-objects"

python3 scripts/link-usb-source-kernel.py \
  --tree "$XNU_TREE" --usb-objects "$BUILD/usb-objects" \
  --components "$BUILD/libErhaiUsbArmCpu.a" --linker "$APPLE_LD" \
  --output "$BUILD/source-kernel.macho"
```

对象构建会验证两个根卷清单的签名和块表，并把本机取得的 NT36536 固件嵌入触控对象。链接入口使用指定的配套 archive，检查目标内核走 USB 根卷等待路径。配套 archive 里的 USB 控制器、Native HID、CPU 和原版存储 ABI 绑定，以及把 `source-kernel.macho` 变成最终预链接内核的步骤，还待接入公开构建。当前从零复现会在这些阶段之前停下。

## 打包本地构建产物

已有对应构建产物时，可在 Linux 或 WSL 上安装 `dosfstools` 和 `mtools` 后运行。输入文件名可以自选，下例用 `BUILD` 指向本地构建目录。

```sh
python3 scripts/package-usb-boot.py \
  --builder ./unixnu --uefi "$UEFI" --fd "$BUILD/recovery-high-ram.fd" \
  --dtb "$BUILD/erhai.dtb" --kernel "$BUILD/kernel.compat.macho" \
  --adt "$BUILD/kernel.macho.adt" --trust ../erhai-local-inputs/usb/original-static-system.bundle \
  --data "$BUILD/ram-data/data.dbrdisk" --config "$BUILD/unixnu.toml" \
  --output "$BUILD/boot-usb-touch.img"
```

这里的 `kernel.compat.macho` 是已完成 USB、触控和两份本地清单集成的预链接内核。ADT 需要匹配本轮的 USB 系统卷和易失 Data 配置。TOML 默认项需使用 USB 根卷，并指定 RAM Data。脚本会校验原版信任缓存、压缩内核、逐文件读回 FAT 内容，并检查最终 Android v4 头和内嵌 payload。

## 运行 USB 供盘

用 Fastboot 临时启动使用配套信任锚的 USB 镜像。等 Windows 的 Apple USBMux 服务识别来宾后，在 Windows 上运行下面的命令，保持 USB 和脚本在线。路径可以指向 WSL 中的输出，无需再复制一份。

```powershell
python scripts/serve-usb-blocks.py --images-dir '<Windows 可访问的 USB 材料目录>'
```

## 其余启动输入

- 触控固件使用 Novatek NT36536 的 XN242 版，实测版本号为 `0x1d`。从 [同机型 Linux 项目固定提交](https://github.com/yzddmr6/onepluspad-2pro-mainline/blob/9762c0994e2b5acd672be2ddbda680fd61f1cad6/packaging/firmware/novatek/nt36536-xn242.bin) 下载 `nt36536-xn242.bin`，已验证与实测 ramdisk 内的文件一致。长度 258048 字节，SHA-256 为 `3e3596895975b28459b1937ea35c819b96386b82cbdcc9c61592af8488db702b`。[同机型原厂 ODM 固件来源记录](https://unixnu.com/devices/erhai-xingxing19035#firmware-and-boot)
- `upstream.rev` 锁定的是板级内核构建的上游。上方单独锁定的 UniXNU 版本供电脑端提取和解码脚本使用。

### UEFI 驱动输入

实测版使用 [UniXNU/uefi-sheng](https://github.com/UniXNU/uefi-sheng/tree/66421cb7bb5694843aec8de1ca3d1de4c20cca38) 的 Pagani 驱动目录作模板，再替换其中 46 份 PE32 为 OPD2413 原厂模块。安装 `uefi_firmware==1.16`，并在已初始化子模块的 UEFI 检出中使用下列命令重建输入目录。

```sh
python3 scripts/prepare-erhai-uefi-drivers.py \
  --stock-uefi /path/to/local/uefi_a.img \
  --pagani-binaries "$UEFI/Binaries/pagani" \
  --output "$UEFI/Binaries/erhai"
```

实测原厂 `uefi_a.img` 来自 OPD2413，SHA-256 为 `ca72c899177c54a6fa2876d65c9881ba1cbb4ef3dd3afcd584e33ec1194e2dac`。同机型公开记录指出原厂 `uefi` 分区位于一加官方固件包。此处还待确认与这个精确哈希对应的可下载包。提取工具和哈希清单已用本地样本验证，生成的 413 个文件全部与实测构建树一致。[原厂固件来源记录](https://unixnu.com/devices/erhai-xingxing19035#firmware-and-boot)

### 编译板级 UEFI

在 Linux 或 WSL 使用专门的 UEFI 检出，固定到 `66421cb7bb5694843aec8de1ca3d1de4c20cca38` 并准备它的子模块。安装该检出的 `pip-requirements.txt` 和 Mu-Silicium Linux 构建依赖，包括 Clang/LLD、AArch64 GNU binutils、NASM、CMake、Ninja。UEFI 构建会使用其 EDK2 工具链和 Stuart 管理的外部依赖。

```sh
git clone https://github.com/UniXNU/DarwinBoot.git ../UniXNU-DarwinBoot
git -C ../UniXNU-DarwinBoot checkout --detach 85332b317ee80898fc78939ea64e6ca52d3837a3
```

脚本从这个提交提取 DarwinBoot 源码，从前文的 UniXNU `c1ecc5b7…` 提取 UEFI package 源码，在新构建目录应用补丁。

`IOMEM` 指向从目标平板 Linux 以 root 读取的 `/proc/iomem` 文本。脚本检查实测固件使用的 4 GiB 高地址区间完全处于同一段未保留的 System RAM，输入 DTB 也必须匹配公开提取脚本的输出。

```sh
python3 scripts/build-erhai-uefi.py \
  --uefi "$UEFI" --unixnu ../UniXNU --darwinboot ../UniXNU-DarwinBoot \
  --dtb "$BUILD/erhai.dtb" --iomem "$IOMEM" --work "$BUILD/firmware"
```

用 `"$BUILD/firmware/recovery-high-ram.fd"` 作为打包时的 `--fd`。打包使用同一个 `"$UEFI"`，其中的 BootShim 负责将嵌入的 payload 交给 UEFI。可以先加 `--stage-only` 检查补丁，然后使用一个新工作目录运行完整编译。

已在独立 Linux 检出中用准备好的厂商驱动和 Stuart 工具缓存完成 DEBUG 编译，输出为 2,785,280 字节，构建工具校验了 53 个固件组件。本次输出 SHA-256 为 `033ca0f1516818d31aea6375ce9f99be363783fbdc96ff3d6ced218f42d64128`，新编固件尚待实机启动验证。
