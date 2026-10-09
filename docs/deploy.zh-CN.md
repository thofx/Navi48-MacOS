# 在 PC 上部署（从公开仓库反推）

English: [deploy.md](deploy.md)

**先读这一段。** 上游自己的部署文档（`INSTALL.md`、`NATIVE-*.md`、`pc-protocol.md`）不在公开仓库里。下面的内容全部是从
`tools/pc/` 下的脚本、kext 解析 boot-arg 的代码和测试文件里反推出来的，每一步都标了出处 `路径:行号`（分支 `dc-port`）。
本文作者**没有在真机上跑过任何一步**。上游 README 的原话是 "Can I install this? Not reliably."。bring-up kext 直接写
GPU 寄存器，构建有误或者显卡不对都可能让机器死机或重启。动手之前，先保证有一条不带它也能启动的路（4.4 节）。

## 0. 公开仓库今天能部署什么、不能部署什么

| 部件 | 状态 | 原因 |
|---|---|---|
| bring-up kext `Navi48Bringup.kext`（GPU 初始化到第 17 级，没有桌面） | **可以部署** | 干净 clone 就能构建（CI 已验证）；由 OpenCore 注入 |
| `navi48test` 命令行工具（验证、驱动显示相关的子命令） | **可以部署** | `tools/build-navi48test.sh` |
| bring-up 期间的显示输出 | 外部项目 | 由 RDNA4FB.kext（只做显示的独立项目）负责；它的安装本文不覆盖 |
| 辅助加速 kext `Navi48Accel.kext` | **卡住** | 它的 Makefile 需要 `tools/native/ioaccel-layout/`，仓库里没有（`tools/native/navi48accel/Makefile:12,20,24-32`） |
| `n48nub`（发布 Metal nub；每次启用桌面都从它开始） | **卡住** | `tools/native/n48nub.c` 和 `tools/native/build.sh` 不在仓库里；接口约定是清楚的（6.3 节） |
| Metal bundle `Navi48Metal.bundle` | **CI 能构建，但没有 spvcache** | `build` workflow 的 `bundle` job 构建 RADV（Mesa 加 `mesa-patches`，只用 ACO）和 `libn48xlate`，再跑 `build.sh`；缺 `spvcache/`（源自 Apple shader，没有发布），所以在翻译 daemon 把 shader 翻译完之前，WindowServer 的管线都是占位的（洋红色 / 空操作，`Navi48Device.m:3685`） |
| shader 翻译 daemon | **可执行文件 CI 能构建，安装没有脚本** | `metal2vulkan` 命令行工具是 `bundle` job 的第二个产物；`/usr/local/navi48/` 和 LaunchDaemon 要照 `tools/native/autotranslate/` 手工配（6.6 节） |
| OpenCore 配置文件（`variants/configs/*.plist`） | 缺失 | boot-arg 在 4.3 和 6.1 节反推出来；Kernel > Add 条目是标准 OpenCore 写法 |
| 开机自动启用、多显示器 | 依赖上面卡住的部件 | |

所以在一台新 PC 上，现实的第一个目标是 **第 1 到 5 节**：kext 经 OpenCore 加载，跑完 bring-up 阶梯到第 17 级，
`navi48test info` 能报告出来，期间显示仍由 RDNA4FB 负责。第 6 节往后是 GPU 桌面需要的东西；其中两件（辅助 kext 和 `n48nub`）
今天还无法从这个仓库做出来。

## 1. 要求

- x86_64 PC，Radeon RX 9070 / 9070 XT（PCI 1002:7550）或 AI PRO R9700（1002:7551）；kext 只认这两个设备
  （`src/navi48-bringup/Info.plist:78-82`，`src/navi48-bringup/src/Navi48Bringup.cpp:7328-7338`）。
- macOS Tahoe 26.6.2，经 OpenCore 启动。上游所有工作都在一台装了这个版本的 PC 上完成，其他版本没测过（`README.txt:37-46`）。
- RDNA4FB.kext 已装在 `/Library/Extensions`（只做显示的 kext；日常用的 "production" 配置依赖它：
  `tools/pc/esp-config.sh:5`，`tools/native/navi48accel/verify.sh:16`）。本文不覆盖它的安装。
- 显示器：只有三个固定模式，就是测试 PC 那三台：DP 2560x1440、HDMI 2560x1440@60、HDMI 1920x1080@60
  （`tools/pc/navi48test.c:1113-1124`，`tools/pc/autoarm/n48-autoarm.sh:26`）。按 EDID 选模式还在 "coming soon"。
- 脚本默认 PC 上的账号是 `testuser`（uid 501），`sudo -n` 免密，构建用的 Mac 通过 ssh 别名 `navi48` 访问它
  （`n48-autoarm.sh:19-21`，`tools/stage-to-pc.sh:8-9`）。账号不同就改脚本里的路径。
- 一条备用的启动路径：`production` 配置（kext 在但不动作）或救援 U 盘（4.4 节）。

## 2. 取得 kext

两种方式：下载 CI 产物（`build` workflow，保留 14 天：`kext` job 上传 `Navi48Bringup.kext-<sha>.tar`，里面嵌入了
linux-firmware tag 20260622 的 10 个 AMD 固件文件，AMD 的许可证 `LICENSE.amdgpu` 在旁边，另有 `navi48test-<sha>`；
`bundle` job 上传 `Navi48Metal.bundle-<sha>` 和 `metal2vulkan-<sha>`，见 6.4 节），或者按 `BUILDING.md` 在 Mac 上自己构建：

```
git clone --recurse-submodules https://github.com/somestupidgirl/RDNA4FB        # MacKernelSDK 是它的 submodule
git clone https://gitlab.com/kernel-firmware/linux-firmware.git
tools/fetch-firmware.sh /path/to/linux-firmware                                   # 打印 sha256 并和开发时的版本比较
make -C src/navi48-bringup MKSDK=/path/to/RDNA4FB/MacKernelSDK all
dwarfdump --uuid src/navi48-bringup/build/Navi48Bringup.kext/Contents/MacOS/Navi48Bringup
```

产物是 x86_64、ad-hoc 签名、macOS 11 ABI（`BUILDING.txt:31-35`）。命令行工具：`tools/build-navi48test.sh` 把
`tools/pc/navi48test.c` 交叉编译成 `tools/pc/navi48test`（`tools/build-navi48test.sh:6-9`）。

## 3. 把文件放到 PC 上

`tools/stage-to-pc.sh` 本来是在构建 Mac 上做这件事的，但在公开仓库上它会直接失败（它要 `variants/`、三个 dtrace
脚本和 `re/`，这些都没发布：`tools/stage-to-pc.sh:37,43-46,131-150`）。改成手工复制：

```
ssh navi48 'mkdir -p ~/navi48-staging/configs ~/navi48-staging/backup'
scp -r Navi48Bringup.kext tools/pc/navi48test tools/pc/esp-kext.sh tools/pc/esp-config.sh \
       tools/pc/read-result.sh tools/pc/reboot-pc.sh navi48:~/navi48-staging/
ssh navi48 'chmod +x ~/navi48-staging/*.sh ~/navi48-staging/navi48test'
```

脚本期望的目录结构：`~/navi48-staging/`（kext、命令行工具、辅助脚本、`configs/<名字>.plist`、`backup/`），之后
`~/n48-metal/` 放 bundle 和 `n48nub`（`tools/stage-to-pc.sh:35-46`，`tools/pc/m6-stage1a-deploy.sh:153-165`）。

## 4. OpenCore：注入 kext，选择 boot-arg

### 4.1 为什么放 ESP 而不是 /Library/Extensions
放在 `/Library/Extensions` 的 kext 要等人在系统设置里点一次 Allow 才会进辅助内核集合，而且每次重新构建都要再点一次；
OpenCore 在内核启动前就把 ESP 上的 kext 注入进去，不需要确认。两个地方不能同时放，否则会加载两次
（`tools/pc/esp-kext.sh:9-34,65-69`）。

### 4.2 放到 ESP 上
```
sudo ~/navi48-staging/esp-kext.sh install     # -> EFI/OC/Kexts/Navi48Bringup.kext
~/navi48-staging/esp-kext.sh show             # 重启前必须确认它报出的 CFBundleVersion 就是刚放上去的那个
```
脚本会找到 `/` 所在磁盘的 ESP，挂载到 `/Volumes/N48-ESP`，没有 `EFI/OC/OpenCore.efi` 的 ESP 会被拒绝，然后复制 kext
并检查可执行文件和 Info.plist（`esp-kext.sh:37-81`）。还有 `remove` 和 `show`（`:83-93`）。

### 4.3 config.plist
预先准备好的配置文件（`production`、`bringup`、`bringup-psp`、`stage<N>`、各个 native 变体）**不在仓库里**。
手工编辑 `EFI/OC/config.plist`（先复制一份为 `EFI/OC/config.prev.plist`，`esp-config.sh` 就是这么做的：
`tools/pc/esp-config.sh:11,47`）：

1. `Kernel > Add`：标准的 OpenCore 条目（反推，仓库里没有原文）：`BundlePath Navi48Bringup.kext`、
   `ExecutablePath Contents/MacOS/Navi48Bringup`、`PlistPath Contents/Info.plist`、`Enabled true`。
2. `NVRAM > Add > 7C436110-...` 下的 `boot-args`，`esp-config.sh show` 读的就是这里（`esp-config.sh:40`）。
   各个命名配置的含义（`esp-config.sh:4-8`）：

| 配置 | 要加的 boot-args | 效果 |
|---|---|---|
| production | （不加） | kext 不动作：没有 `navi48bringup=1` 时 probe 返回 nullptr（`Navi48Bringup.cpp:7329`）；显示由 RDNA4FB 负责 |
| bringup | `navi48bringup=1 rdna4-off=1` | 只读地巡视各 IP 块、PSP 和 SMU；发布 `Navi48,Stage = survey-readonly`（`:7362-7366,7487`） |
| bringup-psp | `navi48bringup=1 rdna4-off=1 navi48-psp=1` | 第一个会写寄存器的阶段：PSP bootloader 链到 SOS（`:573,7380`） |
| stage\<N\> | `navi48bringup=1 rdna4-off=1 navi48-stage=N` | 阶梯跑到第 N 级（第 5 节） |

`rdna4-off=1` 是 RDNA4FB 的开关，不是这个 kext 的：两者都装时，除非设了它，否则显示归 RDNA4FB
（`src/navi48-bringup/src/Navi48Bringup.hpp:5-7`）。那些变体还带着 `-v keepsyms=1 npci=0x2000 alcid=7`
（`src/navi48-bringup/tests/native_rebar_plant.sh:140`）；`-rebar` 变体去掉了 `npci=0x2000`。

### 4.4 留好退路
- 用 `production` 启动（没有 `navi48bringup`）：kext 在，但什么都不做。构建坏了也伤不到这条路。
- 一个救援 U 盘，它的 OpenCore 配置带 `rdna4-off=1`、不带 bring-up（这份配置不在仓库里：
  `src/navi48-bringup/tests/native_rebar_plant.sh:22-24`）。
- `sudo ~/navi48-staging/esp-kext.sh remove` 把 kext 从 ESP 上拿掉。
- 上一份 `config.plist` 保存为 `config.prev.plist`；启动项被隐藏时，在 OpenCore 选单按空格（`esp-config.sh:50`）。

## 5. bring-up 阶梯

各级（`src/navi48-bringup/src/amd/amdgpu_init.h:33-53`）：1 IPDiscovery、2 IHInit、3 GMCInit、4 PSPInit、
5 PSPLoadSOS、6 PSPRingCreate、7 TMRSetup、8 PSPFwLoad、9 SMUInit、10 IMUInit、11 RLCInit、12 CPInit、13 MESInit、
14 GFXInit、15 SDMAInit、16 PM4Test、17 ComputeDispatch（终点）。一次升一级：先 `navi48-stage=5`，再往上。
阶梯会读的选项（`Navi48Bringup.cpp:6914-7035`）：`navi48-smu=1`（只读的 SMU 测试）、`navi48-interrupts=0`（轮询路径）、
`navi48-smu-full` / `navi48-smu-basic`（第 11 级起默认 full）、第 15 级末尾的各 `*-test` 自检。

每次启动：
```
~/navi48-staging/reboot-pc.sh            # 有人登录在控制台、或有进程卡死时会拒绝（reboot-pc.sh:5-21）
tools/pc/wait-for-driver.sh 300          # 在 Mac 上：等 sshd，再等 "Stage reached"（wait-for-driver.sh:27-36）
sudo ~/navi48-staging/navi48test info    # 目标是 "Stage reached 17 (ComputeDispatch)"（navi48test.c:63-75）
~/navi48-staging/read-result.sh          # 本次启动的内核日志、Navi48,* 属性、kern.bootargs、已加载的 kext
```
`navi48test` 还有 `counters`、`reg <dword>`、`log`、`metrics`、`power <0-4>`（`navi48test.c:8-21`）。sshd 刚起来时报的
"Navi48Bringup not found in the IORegistry" 是启动时序问题，不是失败（`wait-for-driver.sh:2-10`）。发出重启后 180 秒
PC 还在响应 ssh、或者再也不回来：手动断电重启，不要再发第二次重启（`tools/pc/m6-stage1a-deploy.sh:220-233`）。

## 6. GPU 桌面（native 路线）——今天还卡着，这是部件齐了之后的流程

### 6.1 boot-arg
日常用的变体叫 `stage17-native-1440-metal-disp-amfi-ms-apps`；它的 plist 缺失，boot-arg 是从碎片拼出来的
（`navi48test.c:1142`，`m6-stage1a-deploy.sh:137,203`，`native_disp_plant.sh:449`）：
```
navi48bringup=1 navi48-stage=17 navi48-native=1 navi48-metal=1 navi48-metal-ws=1 navi48-metal-disp=1
amfi_get_out_of_my_way=1 navi48-dmubcmd=1 navi48-disp2=1 navi48-multisession=1 navi48-apps=1
```
不带 `rdna4-off`：RDNA4FB 仍然是 framebuffer，加速器是在它之上接管的（`navi48test.c:1781`）。各参数的作用（默认都关）：
`navi48-native` = native 路线总开关加第 17 级后的 VM 自检（`Navi48Bringup.cpp:6862-6912`）；`navi48-metal` = 允许发布
Metal nub（`src/navi48-bringup/src/amd/native_metal_pure.h:43`）；`navi48-metal-ws` = 允许 WindowServer 打开 kext 的
client（`src/navi48-bringup/src/Navi48NativeClient.cpp:60-75`）；`navi48-metal-disp` = 显示管线相关的子命令
（`src/navi48-bringup/src/amd/native_disp.cpp:569-578`）；`navi48-dmubcmd`、`navi48-disp2` = DMUB 和第二显示器的子命令
（`src/navi48-bringup/src/dcn/navi48_dcn.cpp:190-219`）；`navi48-multisession` = 最多四个 GPU 会话，`navi48-apps` = 白名单里的
App 可以开会话（`src/navi48-bringup/src/amd/native_s1c.cpp:1842-1872`）。为什么需要 `amfi_get_out_of_my_way=1`，没有写明。

### 6.2 辅助 kext（需要 `tools/native/ioaccel-layout/`，没有发布）
按 `m6-stage1a-deploy.sh:174-186` 的做法：每个版本都复制到一个**新**路径 `/Library/Extensions/Navi48Accel-<版本>.kext`
（同路径替换在启动时会被忽略），`chown -R root:wheel`、`chmod -R go-w`、`xattr -cr`；那里只能留一个 id 为
`com.navi48.accelprobe` 的 bundle；先备份 `/Library/KernelCollections/AuxiliaryKernelExtensions.kc`；用
`kmutil libraries -p <目录> -a x86_64` 检查；`sudo kmutil load -p <目录>` **只执行一次**（报 "not approved" 是正常的；
绝不要 `kmutil install --update-all`）；重启；第一次安装要在"系统设置 > 隐私与安全性"里点 Allow。
总开关：boot-arg `navi48-aux=0`（`tools/native/navi48accel/src/n48accel_pure.h:19-21`）。

### 6.3 `n48nub`（源码没有发布）
如果要重写，它的约定是：打开 kext 的 user client（类型 `'N48N'`），先 Hello（selector 0），再 publish（19）/ withdraw（20）；
打印 `publish: 0 (Success)`；`status` 打印 `Navi48MetalNub present, registry ID 0x...`（退出码 0）或
`NO Navi48MetalNub in the registry`（退出码 1）（`src/navi48-bringup/src/Navi48NativeABI.h:9,27,40,67-68`，
`tools/pc/autoarm/test/fake-n48nub`）。没有 `navi48-metal=1`、native 自检没通过、或 GPU 已 hang 时，publish 会被拒绝
（`native_metal_pure.h:40-49`）。

### 6.4 Metal bundle
CI 能构建（`build` workflow 的 `bundle` job，在 Intel runner 上：Mesa `f5cb8ee0` 加 9 个 patch，`-Dllvm=disabled
-Dplatforms=macos -Dzstd=disabled`，再构建 metal2vulkan fork 并跑 `build.sh`；约 12 分钟）。构建输入见
`tools/native/navi48metal/build.sh:13-31`。**它没有 `spvcache/`**：那个目录是从 Apple 自己的 shader 库翻译出来的 SPIR-V，
没有发布，所以用这个仓库构建的 bundle 会让 WindowServer 的管线先用占位实现（渲染成洋红色、compute 空操作，
`Navi48Device.m:3685`），直到翻译 daemon（6.6 节）在 PC 上把每个 shader 翻译完；头几次开机看起来是坏的。安装按 `m6-stage1a-deploy.sh:190`：解包到 `~/n48-metal/unpack.<时间戳>/`，
把旧的 `/Library/GPUBundles/Navi48Metal.bundle` 移到 `~/navi48-staging/backup/`，`cp -R` 新的过去，`chown -R root:wheel`、
`xattr -cr`、`codesign --verify --strict -v`。不需要重启。bundle 只把 Metal 设备交给 WindowServer、带 `N48M_ALLOW=1` 的
root 工具、或 kext 准入的 App；存在 `/private/tmp/n48m-off` 或 300 秒内 WindowServer 异常启动 3 次后会拒绝
（`tools/native/navi48metal/Navi48Device.m:116-199`）。

### 6.5 启用桌面（每次启动一次，0 个用户登录，刚开机；`tools/pc/arm-gpu-desktop.sh:8-38`）
```
sudo ~/n48-metal/n48nub publish                      # 然后等 kmutil showloaded 列出 accelprobe
sudo navi48test accel pipeadopt                      # "status : 0 (OK)"，pipes ours / all 相等
sudo navi48test accel pipeagdc 1                     # "agdc status : 0 (PUBLISHED)"
sudo navi48test accel fbname 1                       # "class name NOW : AMDRDNA4"
sudo touch /private/tmp/n48m-headless-no; sudo chmod 644 /private/tmp/n48m-headless-no
sudo navi48test accel pipearm 1                      # "0xccf now : 1"
sudo navi48test accel pipereload; sudo killall -9 WindowServer    # 要在 15 秒窗口内
```
然后启动 `n48-autoarm-watch.sh`，并执行 `launchctl asuser 501 launchctl setenv CI_USE_MTL_DAG_FOR_CIKL_SRC 0`。
启用在本次开机内不可逆；有用户登录时绝不能重启 WindowServer（`arm-gpu-desktop.sh:9`）。

### 6.6 App 和翻译 daemon
每次开机 `sudo navi48test accel appallow add <名字>`（`navi48test.c:1842-1845`；自动启用脚本会读
`/Library/Application Support/Navi48/apps.txt`）。准入的 App 在进程内翻译 shader（`n48_xlate.h:4-8`）；
WindowServer 缺的 shader 交给 `com.navi48.translate` daemon（`/usr/local/navi48/`，以 `nobody` 运行，
`tools/native/autotranslate/pc-translate.sh:2-25`），它需要 `metal2vulkan` 可执行文件：`bundle` job 把它作为
`metal2vulkan-<sha>` 上传（x86_64，按 `navi48metal/add-air.py:5` 的要求带 `--features serde`）。daemon 自身的安装（把
`tools/native/autotranslate/{pc-translate.sh,pc-translate.py,n48-translate.sb,overrides.txt,n48-llvm-dis,n48-spirv-val-stub}`
和这个可执行文件放到 `/usr/local/navi48/`，plist 放到 `/Library/LaunchDaemons/`，再 `launchctl bootstrap system`）没有脚本。

## 7. 多显示器（M6）
变体：`-m6` 加 `navi48-fb2=1 navi48-m6=1`；`-m6flip` 再加 `navi48-m6flip=1`；`-m6flip3` 再加 `navi48-m6flip1=1`
（`m6-stage1a-deploy.sh:137`，`tools/pc/m6-stage2-run.sh:11`）。那一串操作（`dmubsend`、`disp2 timing/connect/plane`、
`fbhold`、`fbpublish`）就是 `n48-autoarm.sh:184-279` 做的事；`fbhold` 和 `fbpublish` 在本次开机内不可逆。
按显示器分的开关文件：`/private/tmp/n48m-noflip1`、`-noflip2`、`-noflip`（`tools/native/navi48metal/n48_m6x.h:36-37`）。

## 8. 开机自动启用
`cd tools/pc/autoarm && sudo ./install.sh [--load]` 装一个 LaunchDaemon，每次开机在登录界面跑一遍启用流程，第一步失败就停
（`install.sh:2-8`）。需要 m6 那组 boot-arg；有用户登录或 DP 落到 1080p 时会拒绝。总开关
`/Library/Application Support/Navi48/autoarm-off`；`uninstall.sh` 只删 manifest 里列出的文件。路径写死为
`/Users/testuser`（`n48-autoarm.sh:19-21`）。

## 9. 恢复

| 情况 | 做法 | 出处 |
|---|---|---|
| 用 bring-up 配置启动后 panic / 起不来 | 用 `production` 或救援 U 盘启动；`esp-kext.sh remove` | `esp-kext.sh:22-31` |
| config.plist 装错 | 恢复 `EFI/OC/config.prev.plist` | `esp-config.sh:11,47` |
| 辅助 kext 异常 | `navi48-aux=0`，或把它移出 `/Library/Extensions` 后重启 | `n48accel_pure.h:19-21` |
| bundle 异常 | `sudo touch /private/tmp/n48m-off`（WindowServer 退回 CPU 渲染） | `Navi48Device.m:122-136` |
| HDMI 翻页出错 | `touch /private/tmp/n48m-noflip1` / `-noflip2` / `-noflip` | `n48_m6x.h:36-37` |
| 进程内翻译出错 | `touch /private/tmp/n48m-noinproc` | `n48_xlate.h:47` |
| 要停掉自动启用 | `touch "/Library/Application Support/Navi48/autoarm-off"` | `install.sh:7` |
| plane 被 hold / framebuffer 已发布 | 重启（本次开机内不可逆） | `navi48test.c:1134,1432` |
| ESP 被 OpenCore 日志塞满 | 归档到 `~/esp-oc-logs-archive`，不要删 | `m6-stage1a-deploy.sh:169` |

## 10. 公开仓库里没有的东西（第 0 节的依据）
`variants/configs/*.plist` 和救援配置；`tools/native/n48nub.c`；`tools/native/ioaccel-layout/`；
`tools/native/navi48metal/spvcache/`；daemon 的安装；RADV 的 meson 配置（CI 用的那份是重建的）；
`tools/pc/{trace-pageon,iopparse,wscomp}.d`；`re/`；两份 `INSTALL.md`；脚本引用的 `notes/*.md`；第一次点 Allow 的步骤；
RDNA4FB 的安装方法。
