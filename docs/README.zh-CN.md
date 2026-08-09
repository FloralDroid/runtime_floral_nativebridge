# Floral 聚合 NativeBridge

`libmixbridge.so` 是 x86 Android 镜像使用的单一 NativeBridge 入口，负责
在每个进程内选择一个 ARM 转译后端，并转发 Android 12 NativeBridge v6 回调。
NDK Translation 和 Houdini 二进制不放入本仓库。

x86 Floral 产品应继承 `system/floral/nativebridge/nativebridge.mk`，由产品
片段安装库并设置 `ro.dalvik.vm.native.bridge=libmixbridge.so`；纯 ARM 产品
不要继承该片段。

Android 12 源码还需要先应用
`redroid-patches/android-12.0.0_r32/art/0001-expose-nativebridge-headers-to-floral.patch`，
否则 Soong 不允许这个 system 库引用 ART 的回调头文件。

## 策略文件

可选策略文件为 `/ipc/floral_stream/nativebridge.json`。文件缺失或
格式错误时使用内置 `auto` 策略：先尝试 NDK Translation，再尝试 Houdini。
进程规则先匹配完整进程名，再匹配 `:` 前的包名。

```json
{
  "default_backend": "auto",
  "packages": {
    "com.example.game": "houdini",
    "com.example.game:remote": "ndk"
  }
}
```

可以通过只读属性覆盖后端路径：
`ro.floral.nativebridge.ndk` 和 `ro.floral.nativebridge.houdini`。
默认路径按 ABI 使用 `/system/lib64/` 或 `/system/lib/`。

后端初始化后不会在同一进程内切换或回退。加载、接口检查或初始化失败会
直接报告给 ART，避免两个转译运行时共享进程状态。NativeBridge 回调没有
提供包名、签名和版本信息，因此当前版本只按进程名选择；更细的规则需要
后续 ART 配合。ABI 环境沿用后端返回值，不修改全局 `ro.product.cpu.*`。

## 测试

`floral_nativebridge_policy_test` 检查策略优先级；Android 目标
`floral_nativebridge_probe` 只打开库并检查 `NativeBridgeItf` 导出，不会在
一个进程内初始化两个后端。
