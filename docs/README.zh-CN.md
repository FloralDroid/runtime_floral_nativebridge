# Floral 聚合 NativeBridge

`libmixbridge.so` 是 x86 Android 镜像使用的单一 NativeBridge 入口，负责
在每个进程内选择一个 ARM 转译后端，并转发 Android 12 NativeBridge v6 回调。
NDK Translation 和 Houdini 二进制不放入本仓库。

x86 Floral 产品应继承 `system/floral/nativebridge/nativebridge.mk`，由产品
片段安装库并设置 `ro.dalvik.vm.native.bridge=libmixbridge.so`；纯 ARM 产品
不要继承该片段。

Android 12 源码还需要三个 ART 配套补丁：
`redroid-patches/android-12.0.0_r32/art/0001-expose-nativebridge-headers-to-floral.patch`，
`0002-support-hybrid-nativebridge-elf-loading.patch` 和
`0003-pass-Floral-NativeBridge-process-context.patch`。它们分别开放 ART 回调头文件、
让桥接 classloader 按 ELF 架构选择 namespace，并在 zygote 降权前传入真实进程
身份。frameworks/base 配套补丁负责保存选择和处理早期 native crash。

## 策略文件

宿主可选策略文件为 `/ipc/floral_stream/nativebridge.json`。init 在每次开机将其
复制到 Android 私有缓存。文件缺失或
格式错误时使用内置 `auto` 策略：先尝试 NDK Translation，再尝试 Houdini。
进程规则先匹配完整进程名，再匹配 `:` 前的包名。修改宿主策略后需要重启容器
刷新缓存，Android 应用无需读取宿主文件。

```json
{
  "default_backend": {
    "mode": "auto",
    "candidates": ["ndk", "houdini"]
  },
  "packages": {
    "com.example.game": {
      "mode": "auto",
      "candidates": ["houdini", "ndk"],
      "selected_backend": "houdini"
    },
    "com.example.game:remote": "ndk"
  }
}
```

原有字符串规则继续兼容。对象规则用于保存候选顺序和可选的显式
`selected_backend`；存在该字段时运行时只使用已选后端，Android 不会覆盖。
`auto` 规则由 Android 保存进程版本和候选结果。ARM 转译进程在启动后 15 秒内
发生 native crash 时，Android 会切换到尚未失败的下一候选并恢复原任务；普通
Java 崩溃、ANR、非 ARM 进程及窗口外崩溃不参与回退。每个候选在同一应用版本
中只尝试一次，应用版本变化时重置该进程状态。

可以通过只读属性覆盖后端路径：
`ro.floral.nativebridge.ndk` 和 `ro.floral.nativebridge.houdini`。
默认路径按 ABI 使用 `/system/lib64/` 或 `/system/lib/`。

路由器不会监看进程，后端初始化后也不会在进程内切换。应用生命周期、早期
native crash 回退和任务恢复均由 Android 内部负责，不依赖宿主程序。运行状态
通过 `AtomicFile` 写入 `/data/system/floral/nativebridge-state.json`，不使用
SQLite。后端必须提供 Android 12 使用的
NativeBridge v3 namespace 接口；加载、接口检查或初始化失败会直接报告给
ART，避免两个转译运行时共享进程状态。策略读取和后端加载都会延迟到 zygote
fork 之后；ART 在降权前明确传入 nice name 和应用数据目录，不再依赖初始化
阶段的 `/proc/self/cmdline`。ABI 环境沿用后端返回值，不修改全局
`ro.product.cpu.*`。

产品片段默认启用 `ro.floral.nativebridge.hybrid_elf=1`。配套 ART 补丁会为
桥接 classloader 同时建立宿主和桥接 namespace；应用通过绝对路径加载动态
解包的 x86/x86_64 ELF 时走宿主 namespace，ARM/ARM64 和无法直接识别的
路径仍交给已选择的转译后端。该能力不会在同一个 ELF 依赖图中混合架构。

## 测试

`floral_nativebridge_policy_test` 检查策略优先级；Android 目标
`floral_nativebridge_probe` 只打开库并检查 `NativeBridgeItf` 导出，不会在
一个进程内初始化两个后端。
