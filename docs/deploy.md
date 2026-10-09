# Deploying on a PC (reconstructed from the public tree)

中文版：[deploy.zh-CN.md](deploy.zh-CN.md)

**Read this first.** Upstream's own deployment notes (`INSTALL.md`, `NATIVE-*.md`, `pc-protocol.md`) are not in the
public tree. Everything below was reconstructed from the scripts under `tools/pc/`, the kext's boot-arg parsing and the
test files, each step with its source as `path:line` (branch `dc-port`). None of it has been run by the author of this
document. The upstream README says "Can I install this? Not reliably." The bring-up kext writes GPU registers directly:
a wrong build or a different card can hang or restart the machine. Keep a way to boot without it (section 4.4).

## 0. What the public tree can and cannot deploy today

| Piece | Status | Why |
|---|---|---|
| Bring-up kext `Navi48Bringup.kext` (GPU init to stage 17, no desktop) | **deployable** | builds from a clean clone (CI does); OpenCore-injected |
| `navi48test` CLI (verify, drive the display verbs) | **deployable** | `tools/build-navi48test.sh` |
| Display while bringing up | external | RDNA4FB.kext (display-only, separate project) owns the screen; its install is not described here |
| Aux accelerator kext `Navi48Accel.kext` | **blocked** | its Makefile needs `tools/native/ioaccel-layout/`, which is not in the tree (`tools/native/navi48accel/Makefile:12,20,24-32`) |
| `n48nub` (publishes the Metal nub; every arming step starts with it) | **blocked** | `tools/native/n48nub.c` and `tools/native/build.sh` are not in the tree; its contract is known (6.3) |
| Metal bundle `Navi48Metal.bundle` | **blocked** | needs a RADV build (no meson config given), the LGPL `libn48xlate` (buildable), and `spvcache/` (Apple-derived, not shipped; `build.sh:31` fails without it) |
| Shader-translate daemon | **blocked** | the `metal2vulkan` binary and the daemon's install are not scripted |
| OpenCore configs (`variants/configs/*.plist`) | missing | boot-args reconstructed in 4.3 and 6.1; the Kernel > Add entry is standard OpenCore |
| Auto-arm, multi-display | depend on the blocked pieces | |

So the realistic first goal on a fresh PC is **sections 1-5**: the kext loads through OpenCore, runs the bring-up
ladder to stage 17 and `navi48test info` reports it, while RDNA4FB keeps driving the display. Section 6 onward is what
the GPU desktop needs once the blocked pieces exist.

## 1. Requirements

- x86_64 PC, Radeon RX 9070 / 9070 XT (PCI 1002:7550) or AI PRO R9700 (1002:7551); the kext probes nothing else
  (`src/navi48-bringup/Info.plist:78-82`, `src/navi48-bringup/src/Navi48Bringup.cpp:7328-7338`).
- macOS Tahoe 26.6.2, booting through OpenCore. Everything upstream ran on one PC with exactly this build; other
  builds are untested (`README.txt:37-46`).
- RDNA4FB.kext installed in `/Library/Extensions` (the display-only kext; the everyday "production" config relies on it:
  `tools/pc/esp-config.sh:5`, `tools/native/navi48accel/verify.sh:16`). Not covered here.
- Displays: only three fixed modes exist, the test PC's: DP 2560x1440, HDMI 2560x1440@60, HDMI 1920x1080@60
  (`tools/pc/navi48test.c:1113-1124`, `tools/pc/autoarm/n48-autoarm.sh:26`). EDID-driven modes are "coming soon".
- The scripts assume a PC account `testuser` (uid 501) with passwordless `sudo -n`, reachable from a build Mac as the
  ssh host alias `navi48` (`n48-autoarm.sh:19-21`, `tools/stage-to-pc.sh:8-9`). With another account, edit the paths.
- A second way to boot: the `production` config (kext present but inert) or a rescue USB stick (section 4.4).

## 2. Get the kext

Either download the CI artifact (`build` workflow, job `kext`: `Navi48Bringup.kext-<sha>.tar`, kept 14 days; it embeds
the ten AMD firmware files, AMD's licence `LICENSE.amdgpu` is copied next to them) or build on a Mac as `BUILDING.md`
says:

```
git clone --recurse-submodules https://github.com/somestupidgirl/RDNA4FB        # MacKernelSDK is its submodule
git clone https://gitlab.com/kernel-firmware/linux-firmware.git
tools/fetch-firmware.sh /path/to/linux-firmware                                   # prints sha256 vs the developed-against copy
make -C src/navi48-bringup MKSDK=/path/to/RDNA4FB/MacKernelSDK all
dwarfdump --uuid src/navi48-bringup/build/Navi48Bringup.kext/Contents/MacOS/Navi48Bringup
```

The result is x86_64, ad-hoc signed, macOS 11 ABI (`BUILDING.txt:31-35`). The CLI: `tools/build-navi48test.sh`
cross-compiles `tools/pc/navi48test.c` to `tools/pc/navi48test` (`tools/build-navi48test.sh:6-9`).

## 3. Stage on the PC

`tools/stage-to-pc.sh` does this from the build Mac, but it hard-fails on the public tree (it wants `variants/`, three
dtrace scripts and `re/` that are not shipped: `tools/stage-to-pc.sh:37,43-46,131-150`). Stage by hand instead:

```
ssh navi48 'mkdir -p ~/navi48-staging/configs ~/navi48-staging/backup'
scp -r Navi48Bringup.kext tools/pc/navi48test tools/pc/esp-kext.sh tools/pc/esp-config.sh \
       tools/pc/read-result.sh tools/pc/reboot-pc.sh navi48:~/navi48-staging/
ssh navi48 'chmod +x ~/navi48-staging/*.sh ~/navi48-staging/navi48test'
```

Layout the scripts expect: `~/navi48-staging/` (kext, CLI, helpers, `configs/<name>.plist`, `backup/`), later
`~/n48-metal/` for the bundle and `n48nub` (`tools/stage-to-pc.sh:35-46`, `tools/pc/m6-stage1a-deploy.sh:153-165`).

## 4. OpenCore: inject the kext, choose the boot-args

### 4.1 Why the ESP and not /Library/Extensions
A kext in `/Library/Extensions` enters the auxiliary kernel collection only after a human clicks Allow in System
Settings, once per rebuilt binary; OpenCore injects ESP kexts before the kernel starts, with no consent step. The kext
must not be in both places or it loads twice (`tools/pc/esp-kext.sh:9-34,65-69`).

### 4.2 Put it on the ESP
```
sudo ~/navi48-staging/esp-kext.sh install     # -> EFI/OC/Kexts/Navi48Bringup.kext
~/navi48-staging/esp-kext.sh show             # must name the staged CFBundleVersion before any reboot
```
The script finds the ESP of the disk `/` lives on, mounts it at `/Volumes/N48-ESP`, refuses an ESP without
`EFI/OC/OpenCore.efi`, copies the kext and checks its executable and Info.plist (`esp-kext.sh:37-81`). `remove` and
`show` exist (`:83-93`).

### 4.3 config.plist
The staged configs (`production`, `bringup`, `bringup-psp`, `stage<N>`, the native variants) are **not in the tree**.
Edit `EFI/OC/config.plist` by hand (keep a copy as `EFI/OC/config.prev.plist`, which is what `esp-config.sh` would do:
`tools/pc/esp-config.sh:11,47`):

1. `Kernel > Add`: a standard OpenCore entry (reconstructed, not quoted in the tree): `BundlePath Navi48Bringup.kext`,
   `ExecutablePath Contents/MacOS/Navi48Bringup`, `PlistPath Contents/Info.plist`, `Enabled true`.
2. `NVRAM > Add > 7C436110-...` `boot-args`, read by `esp-config.sh show` (`esp-config.sh:40`). The named configs
   meant (`esp-config.sh:4-8`):

| Config | boot-args to add | What happens |
|---|---|---|
| production | (nothing) | kext inert: without `navi48bringup=1` its probe returns nullptr (`Navi48Bringup.cpp:7329`); RDNA4FB drives the display |
| bringup | `navi48bringup=1 rdna4-off=1` | read-only survey of the IP blocks, PSP and SMU; `Navi48,Stage = survey-readonly` (`:7362-7366,7487`) |
| bringup-psp | `navi48bringup=1 rdna4-off=1 navi48-psp=1` | first writing stage: PSP bootloader chain to SOS (`:573,7380`) |
| stage\<N\> | `navi48bringup=1 rdna4-off=1 navi48-stage=N` | the ladder to stage N (section 5) |

`rdna4-off=1` is RDNA4FB's switch, not this kext's: with both installed RDNA4FB owns the display unless it is set
(`src/navi48-bringup/src/Navi48Bringup.hpp:5-7`). The variants also carried `-v keepsyms=1 npci=0x2000 alcid=7`
(`src/navi48-bringup/tests/native_rebar_plant.sh:140`); `npci=0x2000` was dropped in the `-rebar` variants.

### 4.4 Keep a way back
- Boot `production` (no `navi48bringup`): the kext is present but does nothing. A broken build cannot hurt that boot.
- A rescue USB stick whose OpenCore config boots with `rdna4-off=1` and no bring-up (its config is not in the tree:
  `src/navi48-bringup/tests/native_rebar_plant.sh:22-24`).
- `sudo ~/navi48-staging/esp-kext.sh remove` takes the kext off the ESP.
- The previous `config.plist` as `config.prev.plist`; if the picker entry is hidden, spacebar at the OpenCore picker
  (`esp-config.sh:50`).

## 5. The bring-up ladder

Stages (`src/navi48-bringup/src/amd/amdgpu_init.h:33-53`): 1 IPDiscovery, 2 IHInit, 3 GMCInit, 4 PSPInit, 5 PSPLoadSOS,
6 PSPRingCreate, 7 TMRSetup, 8 PSPFwLoad, 9 SMUInit, 10 IMUInit, 11 RLCInit, 12 CPInit, 13 MESInit, 14 GFXInit,
15 SDMAInit, 16 PM4Test, 17 ComputeDispatch (the finish line). Climb it one config at a time: `navi48-stage=5`, then
higher. Options the ladder reads (`Navi48Bringup.cpp:6914-7035`): `navi48-smu=1` (read-only SMU test), `navi48-interrupts=0`
(polled path), `navi48-smu-full` / `navi48-smu-basic` (default full from stage 11), the `*-test` self-tests at stage 15.

Each boot:
```
~/navi48-staging/reboot-pc.sh            # refuses with a console user logged in, or a stuck process (reboot-pc.sh:5-21)
tools/pc/wait-for-driver.sh 300          # from the Mac: waits for sshd, then for "Stage reached" (wait-for-driver.sh:27-36)
sudo ~/navi48-staging/navi48test info    # "Stage reached 17 (ComputeDispatch)" is the goal (navi48test.c:63-75)
~/navi48-staging/read-result.sh          # this boot's kernel log lines, Navi48,* properties, kern.bootargs, loaded kexts
```
`navi48test` also has `counters`, `reg <dword>`, `log`, `metrics`, `power <0-4>` (`navi48test.c:8-21`). A premature
"Navi48Bringup not found in the IORegistry" right after sshd answers is the boot race, not a failure
(`wait-for-driver.sh:2-10`). If the PC still answers ssh 180 s after a reboot request, or never comes back: power-cycle
by hand, no second reboot (`tools/pc/m6-stage1a-deploy.sh:220-233`).

## 6. The GPU desktop (native route) — blocked today, procedure for when the pieces exist

### 6.1 Boot-args
The everyday variant was `stage17-native-1440-metal-disp-amfi-ms-apps`; its plist is missing and its boot-args are
reconstructed from fragments (`navi48test.c:1142`, `m6-stage1a-deploy.sh:137,203`, `native_disp_plant.sh:449`):
```
navi48bringup=1 navi48-stage=17 navi48-native=1 navi48-metal=1 navi48-metal-ws=1 navi48-metal-disp=1
amfi_get_out_of_my_way=1 navi48-dmubcmd=1 navi48-disp2=1 navi48-multisession=1 navi48-apps=1
```
No `rdna4-off`: RDNA4FB stays the framebuffer and the accelerator is adopted on top of it (`navi48test.c:1781`).
What they do (all off by default): `navi48-native` = the native route gate + VM self-test after stage 17
(`Navi48Bringup.cpp:6862-6912`); `navi48-metal` = the Metal nub may be published (`src/navi48-bringup/src/amd/native_metal_pure.h:43`);
`navi48-metal-ws` = WindowServer may open the kext's client (`src/navi48-bringup/src/Navi48NativeClient.cpp:60-75`);
`navi48-metal-disp` = the display-pipe verbs (`src/navi48-bringup/src/amd/native_disp.cpp:569-578`); `navi48-dmubcmd`,
`navi48-disp2` = DMUB and second-display verbs (`src/navi48-bringup/src/dcn/navi48_dcn.cpp:190-219`);
`navi48-multisession` = up to four GPU sessions, `navi48-apps` = allow-listed apps may open one
(`src/navi48-bringup/src/amd/native_s1c.cpp:1842-1872`). Why `amfi_get_out_of_my_way=1` is needed is not stated.

### 6.2 Aux kext (needs `tools/native/ioaccel-layout/`, not shipped)
As `m6-stage1a-deploy.sh:174-186` does it: copy each version to a **new** path `/Library/Extensions/Navi48Accel-<ver>.kext`
(same-path swaps are ignored at boot), `chown -R root:wheel`, `chmod -R go-w`, `xattr -cr`; keep exactly one bundle
with id `com.navi48.accelprobe` there; back up `/Library/KernelCollections/AuxiliaryKernelExtensions.kc`; check
`kmutil libraries -p <dir> -a x86_64`; run `sudo kmutil load -p <dir>` **once** ("not approved" is expected; never
`kmutil install --update-all`); reboot; the first install needs the Allow click in System Settings > Privacy & Security.
Kill switch: boot-arg `navi48-aux=0` (`tools/native/navi48accel/src/n48accel_pure.h:19-21`).

### 6.3 `n48nub` (source not shipped)
Its contract, if it has to be rewritten: open the kext's user client (type `'N48N'`), Hello (selector 0), then publish
(19) / withdraw (20); print `publish: 0 (Success)`, and for `status` print `Navi48MetalNub present, registry ID 0x...`
(exit 0) or `NO Navi48MetalNub in the registry` (exit 1) (`src/navi48-bringup/src/Navi48NativeABI.h:9,27,40,67-68`,
`tools/pc/autoarm/test/fake-n48nub`). Publishing is refused unless `navi48-metal=1`, the native self-test passed and
the GPU is not hung (`native_metal_pure.h:40-49`).

### 6.4 Metal bundle (needs RADV, libn48xlate and spvcache)
Build inputs: `tools/native/navi48metal/build.sh:13-31` (RADV dylib, `libn48xlate.dylib` from
`third-party/metal2vulkan-n48/README.txt`, `spvcache/`). Install as `m6-stage1a-deploy.sh:190`: untar into
`~/n48-metal/unpack.<ts>/`, move the old `/Library/GPUBundles/Navi48Metal.bundle` to `~/navi48-staging/backup/`,
`cp -R` the new one there, `chown -R root:wheel`, `xattr -cr`, `codesign --verify --strict -v`. No reboot. The bundle
hands a Metal device only to WindowServer, a root tool with `N48M_ALLOW=1`, or an app the kext admits; it declines when
`/private/tmp/n48m-off` exists or after three abnormal WindowServer starts in 300 s (`tools/native/navi48metal/Navi48Device.m:116-199`).

### 6.5 Arm the desktop (per boot, 0 users, fresh boot; `tools/pc/arm-gpu-desktop.sh:8-38`)
```
sudo ~/n48-metal/n48nub publish                      # then wait for kmutil showloaded to list accelprobe
sudo navi48test accel pipeadopt                      # "status : 0 (OK)", pipes ours / all equal
sudo navi48test accel pipeagdc 1                     # "agdc status : 0 (PUBLISHED)"
sudo navi48test accel fbname 1                       # "class name NOW : AMDRDNA4"
sudo touch /private/tmp/n48m-headless-no; sudo chmod 644 /private/tmp/n48m-headless-no
sudo navi48test accel pipearm 1                      # "0xccf now : 1"
sudo navi48test accel pipereload; sudo killall -9 WindowServer    # within the 15 s window
```
then the watcher `n48-autoarm-watch.sh` and `launchctl asuser 501 launchctl setenv CI_USE_MTL_DAG_FOR_CIKL_SRC 0`.
The arm is one-way for the boot; never restart WindowServer with a user logged in (`arm-gpu-desktop.sh:9`).

### 6.6 Apps and the translate daemon
`sudo navi48test accel appallow add <name>` per boot (`navi48test.c:1842-1845`; auto-arm reads
`/Library/Application Support/Navi48/apps.txt`). Admitted apps translate shaders in-process (`n48_xlate.h:4-8`);
WindowServer's misses go to the `com.navi48.translate` daemon (`/usr/local/navi48/`, user `nobody`,
`tools/native/autotranslate/pc-translate.sh:2-25`), which needs the `metal2vulkan` binary — not shipped.

## 7. Multi-display (M6)
Variants: `-m6` adds `navi48-fb2=1 navi48-m6=1`; `-m6flip` adds `navi48-m6flip=1`; `-m6flip3` adds `navi48-m6flip1=1`
(`m6-stage1a-deploy.sh:137`, `tools/pc/m6-stage2-run.sh:11`). The chains (`dmubsend`, `disp2 timing/connect/plane`,
`fbhold`, `fbpublish`) are what `n48-autoarm.sh:184-279` runs; `fbhold` and `fbpublish` are irreversible for the boot.
Per-display kill files: `/private/tmp/n48m-noflip1`, `-noflip2`, `-noflip` (`tools/native/navi48metal/n48_m6x.h:36-37`).

## 8. Auto-arm
`cd tools/pc/autoarm && sudo ./install.sh [--load]` installs a LaunchDaemon that runs the recipe once per boot at the
login window and stops at the first failing step (`install.sh:2-8`). Requires the m6 boot-args; refuses with a user
logged in or a 1080p DP raster. Kill switch `/Library/Application Support/Navi48/autoarm-off`; `uninstall.sh` removes
exactly what the manifest lists. Paths are hard-coded to `/Users/testuser` (`n48-autoarm.sh:19-21`).

## 9. Recovery

| Situation | Do | Source |
|---|---|---|
| Panic / unbootable with a bring-up config | boot `production` or the rescue stick; `esp-kext.sh remove` | `esp-kext.sh:22-31` |
| Wrong config.plist | restore `EFI/OC/config.prev.plist` | `esp-config.sh:11,47` |
| Aux kext misbehaves | `navi48-aux=0`, or move it out of `/Library/Extensions` and reboot | `n48accel_pure.h:19-21` |
| Bundle misbehaves | `sudo touch /private/tmp/n48m-off` (WindowServer falls back to the CPU renderer) | `Navi48Device.m:122-136` |
| HDMI flips fault | `touch /private/tmp/n48m-noflip1` / `-noflip2` / `-noflip` | `n48_m6x.h:36-37` |
| In-process translation faults | `touch /private/tmp/n48m-noinproc` | `n48_xlate.h:47` |
| Auto-arm must stop | `touch "/Library/Application Support/Navi48/autoarm-off"` | `install.sh:7` |
| A held plane / published framebuffer | reboot (irreversible for the boot) | `navi48test.c:1134,1432` |
| ESP full of OpenCore logs | archive to `~/esp-oc-logs-archive`, never delete | `m6-stage1a-deploy.sh:169` |

## 10. Not in the public tree (what section 0 rests on)
`variants/configs/*.plist` and the rescue config; `tools/native/n48nub.c`; `tools/native/ioaccel-layout/`;
`tools/native/navi48metal/spvcache/`; the `metal2vulkan` binary and the daemon's install; the RADV meson configuration;
`tools/pc/{trace-pageon,iopparse,wscomp}.d`; `re/`; both `INSTALL.md`; the `notes/*.md` the scripts cite; the first
Allow-click procedure; how RDNA4FB is installed.
