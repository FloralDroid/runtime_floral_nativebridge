# Floral 聚合 NativeBridge

`libmixbridge.so` 是 x86 Android 镜像使用的单一 NativeBridge 入口，负责
在每个进程内选择一个 ARM 转译后端，并转发 Android 12 NativeBridge v6 回调。
NDK Translation 和 Houdini 二进制不放入本仓库。

x86 Floral 产品应继承 `system/floral/nativebridge/nativebridge.mk`，由产品
片段安装库并设置 `ro.dalvik.vm.native.bridge=libmixbridge.so`；纯 ARM 产品
不要继承该片段。

Android 12 源码还需要先应用两个配套补丁：
`redroid-patches/android-12.0.0_r32/art/0001-expose-nativebridge-headers-to-floral.patch`，
以及 `0002-support-hybrid-nativebridge-elf-loading.patch`。前者开放 ART 回调头文件，
后者让桥接 classloader 能按 ELF 架构选择宿主或转译 namespace。

## 策略文件

可选策略文件为 `/ipc/floral_stream/nativebridge.json`。文件缺失或
格式错误时使用内置 `auto` 策略：先尝试 NDK Translation，再尝试 Houdini。
进程规则先匹配完整进程名，再匹配 `:` 前的包名。策略在每个应用进程初始化
NativeBridge 时重新读取，修改后需要结束并重新启动目标应用进程。宿主应将
文件设为应用可读但不可写，例如权限 `0644`。

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
`selected_backend`；存在该字段时运行时只使用已选后端，否则应用进程初始化
期间由 `auto` 按 `candidates` 顺序尝试。

可以通过只读属性覆盖后端路径：
`ro.floral.nativebridge.ndk` 和 `ro.floral.nativebridge.houdini`。
默认路径按 ABI 使用 `/system/lib64/` 或 `/system/lib/`。

路由器不会监看进程，后端初始化后也不会切换或回退。应用进程生命周期、
崩溃和 ANR 仍由 Android 负责。后端必须提供 Android 12 使用的
NativeBridge v3 namespace 接口；加载、接口检查或初始化失败会直接报告给
ART，避免两个转译运行时共享进程状态。策略读取和后端加载都会延迟到
`initialize`、zygote fork 之后才加载，因而进程规则在应用进程内评估，
不会被 zygote 提前固定。NativeBridge 回调没有
提供包名、签名和版本信息，因此当前版本只按进程名选择；更细的规则需要
后续 ART 配合。ABI 环境沿用后端返回值，不修改全局 `ro.product.cpu.*`。

产品片段默认启用 `ro.floral.nativebridge.hybrid_elf=1`。配套 ART 补丁会为
桥接 classloader 同时建立宿主和桥接 namespace；应用通过绝对路径加载动态
解包的 x86/x86_64 ELF 时走宿主 namespace，ARM/ARM64 和无法直接识别的
路径仍交给已选择的转译后端。该能力不会在同一个 ELF 依赖图中混合架构。

## 测试

`floral_nativebridge_policy_test` 检查策略优先级；Android 目标
`floral_nativebridge_probe` 只打开库并检查 `NativeBridgeItf` 导出，不会在
一个进程内初始化两个后端。
