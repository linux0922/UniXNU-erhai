# 尔海 USB 版的源码构建准备

目标机型为 OnePlus Pad 2 Pro OPD2413（erhai）。已实测的版本通过 Fastboot 临时启动后，用 USB 从电脑读取系统卷，内屏可进入主屏幕并使用触控。本分支的脚本目前覆盖电脑端供盘材料的生成与服务。完整启动镜像的构建还需要整理板级 UEFI、设备 DTB 和 USB／触控内核集成。

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

USB 服务读取 `OS.raw`、`SystemOS.dmg`、`OS.manifest.bin` 和 `SystemOS.manifest.bin`。两份清单由本地 RSA 密钥签发。构建启动镜像时，来宾必须信任同次生成的 `trusted-key.bin`，这样才会接受 USB 提供的清单。

## 运行 USB 供盘

用 Fastboot 临时启动使用配套信任锚的 USB 镜像。等 Windows 的 Apple USBMux 服务识别来宾后，在 Windows 上运行下面的命令，保持 USB 和脚本在线。路径可以指向 WSL 中的输出，无需再复制一份。

```powershell
python scripts/serve-usb-blocks.py --images-dir '<Windows 可访问的 USB 材料目录>'
```

## 其余启动输入

- 触控固件使用 Novatek NT36536 的 XN242 版，实测版本号为 `0x1d`。从 [同机型 Linux 项目固定提交](https://github.com/yzddmr6/onepluspad-2pro-mainline/blob/9762c0994e2b5acd672be2ddbda680fd61f1cad6/packaging/firmware/novatek/nt36536-xn242.bin) 下载 `nt36536-xn242.bin`，已验证与实测 ramdisk 内的文件一致。长度 258048 字节，SHA-256 为 `3e3596895975b28459b1937ea35c819b96386b82cbdcc9c61592af8488db702b`。[同机型原厂 ODM 固件来源记录](https://unixnu.com/devices/erhai-xingxing19035#firmware-and-boot)
- 设备 UEFI 在 `UniXNU/uefi-sheng` 的 `66421cb7bb5694843aec8de1ca3d1de4c20cca38` 基础上加入了本地 `erhaiPkg`、配置、DTB 和 BootShim 改动。还需要把该板级移植整理成可获取的源码，并记录原厂 DTB 的取法。
- `upstream.rev` 锁定的是板级内核构建的上游。上方单独锁定的 UniXNU 版本供电脑端提取和解码脚本使用。
