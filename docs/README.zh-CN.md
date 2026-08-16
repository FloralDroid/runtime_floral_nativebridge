# Floral 聚合 NativeBridge

[English](README.md)

`libmixbridge.so` 是 x86 Android 镜像使用的单一 NativeBridge 入口，负责
在每个进程内选择一个 ARM 转译后端，并转发 Android 12 NativeBridge v6 回调。
NDK Translation 和 Houdini 二进制不放入本仓库。

x86 Floral 产品应继承 `system/floral/nativebridge/nativebridge.mk`，由产品
片段安装库并设置 `ro.dalvik.vm.native.bridge=libmixbridge.so`；纯 ARM 产品
不要继承该片段。

Android 12 源码还需要 `redroid-patches/android-12.0.0_r32/art/` 下的 ART
配套补丁组。它们开放 ART 回调头文件、让桥接 classloader 按 ELF 架构选择
namespace、在 zygote 降权前传入真实进程身份、暴露每进程兼容加载模式，并提供
受包名约束的 JNI 加载诊断。frameworks/base 配套补丁负责保存选择并处理早期
native 和 JNI 加载失败。

## 策略文件

宿主可选策略文件为 `/ipc/floral_stream/nativebridge.json`。init 在每次开机将其
复制到 Android 私有缓存。文件缺失或
格式错误时使用内置 `auto` 策略：先尝试 NDK Translation，再尝试 Houdini。
进程规则先匹配完整进程名，再匹配 `:` 前的包名。修改宿主策略后需要重启容器
刷新缓存，Android 应用无需读取宿主文件。

```json
{
  "version": 1,
  "preferred_backend": "houdini",
  "abi": {
    "public": {
      "64": ["arm64-v8a"],
      "32": ["armeabi-v7a", "armeabi"]
    }
  },
  "executables": {
    "/vendor/bin/arm-helper": "ndk"
  },
  "packages": {
    "com.example.game": {
      "mode": "auto",
      "candidates": ["houdini", "ndk"],
      "executables": {
        "libzs-local.so": "houdini"
      }
    },
    "com.example.game:remote": "ndk"
  }
}
```

`preferred_backend` 保持自动回退，并把指定的 `ndk` 或 `houdini` 放在候选首位；
每个后端内部依次包含 `hybrid`、`compat` 两种候选。`compat` 会先把应用私有
原生 ELF 交给选定转译后端，只有后端拒绝时才交给宿主 linker。`default_backend`
仍表示显式默认后端，两者同时存在时以它为准。`abi.public`
控制应用通过 `Build.SUPPORTED_ABIS`
看到的 ABI；PackageManager 等框架内部仍使用构建时的 loader ABI，因此不会
把 x86 主机 ABI 从本地加载路径中移除。缺少 `abi.public` 时使用 ARM 兼容默认值。

原有字符串规则继续兼容。对象规则用于保存候选顺序和可选的显式
`selected_backend`；存在该字段时运行时只使用已选后端，Android 不会覆盖。
`auto` 规则由 Android 保存进程版本和候选结果。ARM 转译进程在启动后 60 秒内
发生 native crash，或出现特定的 JNI `UnsatisfiedLinkError` 时，Android 会切换到
尚未失败的下一候选。只有包含 Activity
的前台主进程才恢复原任务；推送等 helper/service 进程只重启自身，不再销毁前台
任务。普通 Java 崩溃、ANR、非 ARM 进程及窗口外崩溃不参与回退。每个候选在
同一应用和系统版本中只尝试一次；候选通过 60 秒稳定窗口后会固定选择，避免把
后续普通应用崩溃误判成转译失败。应用或系统版本变化时重置对应状态。

顶层 `executables` 为系统级完整路径规则；包对象内的 `executables` 支持完整路径、
文件名或 `*`，且包规则优先。Android 12 的镜像生成脚本会把 ARM32/ARM64
`binfmt_misc` 注册统一指向 `/system/bin/floral_nativebridge_runner`，再由它按 ELF
位数和规则执行 NDK 或 Houdini。无法从路径唯一确认包归属时不会套用其他包规则。

可以通过只读属性覆盖后端路径：
`ro.floral.nativebridge.ndk` 和 `ro.floral.nativebridge.houdini`。
默认路径按 ABI 使用 `/system/lib64/` 或 `/system/lib/`。
独立 ELF runner 路径分别由 `ro.floral.nativebridge.runner.ndk32`、`ndk64`、
`houdini32` 和 `houdini64` 覆盖。

路由器不会监看进程，后端初始化后也不会在进程内切换。应用生命周期、早期
native crash 回退和任务恢复均由 Android 内部负责，不依赖宿主程序。运行状态
通过 `AtomicFile` 写入 `/data/system/floral/nativebridge-state.json`，不使用
SQLite。后端必须提供 Android 12 使用的
NativeBridge v3 namespace 接口；加载、接口检查或初始化失败会直接报告给
ART，避免两个转译运行时共享进程状态。NativeBridge wrapper 在 zygote 打开时
会用 `RTLD_NOW|RTLD_LOCAL` 预加载两个已配置的后端 DSO。fork 后 ART 在降权前
明确传入 nice name 和应用数据目录，子进程只选择并初始化其中一个已加载后端，
不再改变后端 loader/JNI 构造函数的启动顺序。ABI 环境沿用后端返回值，不修改全局
`ro.product.cpu.*`。

产品片段默认启用 `ro.floral.nativebridge.hybrid_elf=1`。配套 ART 补丁会为
桥接 classloader 同时建立宿主和桥接 namespace；系统提供的 x86/x86_64 ELF
在 `hybrid` 模式下，系统提供的 x86/x86_64 ELF 和应用私有 native ELF 保持
宿主优先；`compat` 模式仍让系统路径由宿主负责，但应用私有 native ELF 先交给
已选择的转译后端，后端拒绝时再回退到宿主 namespace。加载结果会记录真实句柄
归属，确保 JNI 和卸载使用同一个所有者。ARM/ARM64 和无法直接识别的路径仍交给
已选择的转译后端。该能力不会在同一个 ELF 依赖图中混合架构。

框架集成默认给 32 位 WebView RELRO 创建预留 256 MiB 地址空间。使用超大
WebView provider 的产品可以通过 `ro.floral.webview.vmsize32` 和
`ro.floral.webview.vmsize64` 按字节覆盖；非正数会回退到默认值。

## 诊断

统一诊断默认关闭。必须先指定完整包名；可选 SO 过滤接受完整路径或文件名，
包名会同时匹配该包的 `:remote` 等子进程。`debug.*` 属性不会持久化到重启后。

```shell
adb shell setprop debug.floral.nbdiag.package com.example.app
adb shell setprop debug.floral.nbdiag.library libsample.so
adb logcat -s FloralNBDiag
```

日志按顺序覆盖后端选择和初始化、`loadLibrary`/`loadLibraryExt`、trampoline、
`JNI_OnLoad` 返回值以及 `RegisterNatives` 的方法表和结果。清空包名立即关闭诊断：

```shell
adb shell setprop debug.floral.nbdiag.package ''
adb shell setprop debug.floral.nbdiag.library ''
```

`RegisterNatives` 通常在 `JNI_OnLoad` 线程中执行，因此日志可以标明所属 SO；若保护
壳在其他线程延迟注册，所属 SO 显示为 `<unknown>`，但包级过滤仍会保留注册记录。

## 测试

`floral_nativebridge_policy_test` 检查策略优先级，
`floral_nativebridge_diagnostics_test` 检查包和 SO 过滤；Android 目标
`floral_nativebridge_probe` 只打开库并检查 `NativeBridgeItf` 导出；实际运行时
会先校验两个已配置后端，再在子进程中选择一个初始化。
