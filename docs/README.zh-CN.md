# Floral 聚合 NativeBridge

`libmixbridge.so` 是 x86 Android 镜像使用的单一 NativeBridge 入口，负责
在每个进程内选择一个 ARM 转译后端，进程内不会同时驻留两个转译运行时。
`hybrid` 模式转发 Android 12 NativeBridge v6 回调；`direct` 模式由 ART 直接绑定
所选后端的 callback 表。
NDK Translation 和 Houdini 二进制不放入本仓库。

x86 Floral 产品应继承 `system/floral/nativebridge/nativebridge.mk`，由产品
片段安装库并设置 `ro.dalvik.vm.native.bridge=libmixbridge.so`；纯 ARM 产品
不要继承该片段。

Android 12 源码还需要 Floral Android 12 patch series 中的 ART、frameworks/base、
system/core 和 linkerconfig 配套修改。它们负责桥接 ELF 路由、在 zygote 降权前
传入所选后端、挂载 Houdini 的进程私有 guest sysroot，并处理早期 native 和 JNI
加载失败。NDK Translation 使用 AOSP 构建产出的 ARM guest userspace。

## 后端选择

系统不读取宿主 policy 或运行时 JSON 状态文件。ARM64 应用在
`ro.floral.nativebridge.default_backend=auto` 时先使用 NDK Translation，失败后
回退 Houdini；把该只读属性设为 `houdini` 会交换候选顺序。Android 12 NDK payload
只支持 ARM64，因此 ARM32 应用始终使用 Houdini。

每次 ARM 应用 fork 前，system_server 会选择一个 `backend/hybrid` 候选，并在有界
内存表中记录包版本、失败候选和成功候选。启动后 60 秒内发生 native crash 或受支持
的 JNI/ELF loader 错误时，下次从新的 zygote 启动会使用尚未失败的下一候选。只有
前台主进程恢复原任务；helper/service 进程独立重启。普通 Java 崩溃、ANR、非 ARM
进程和窗口外崩溃不参与回退。应用全部进程退出后，成功候选缓存 10 分钟；状态不持久化，
system_server 重启后清空。

镜像生成脚本把 ARM32/ARM64 `binfmt_misc` 注册统一指向
`/system/bin/floral_nativebridge_runner`。zygote 应用的子进程继承所选后端；其他
direct-binfmt 执行中，ARM64 使用全局默认值，ARM32 固定使用 Houdini。runner 的
`execv()` 回退只处理后端 runner 无法启动的情况，后端一旦启动就不能切换运行时。

后端路径由只读架构属性 `ro.floral.nativebridge.ndk64`、`houdini32` 和
`houdini64` 覆盖；默认 payload 根目录为 `/system/floral/ndk` 和
`/system/floral/houdini`。
独立 ELF runner 路径分别由 `ro.floral.nativebridge.runner.ndk64`、`houdini32`
和 `houdini64` 覆盖；32 位 NDK runner 不作为候选。

路由器不会监看进程，后端初始化后也不会在进程内切换。应用生命周期、早期
native crash 回退和任务恢复均由 Android 内部负责，不依赖宿主程序。候选结果由
system_server 通过 zygote 专用启动参数一次性传入，不使用运行状态文件。后端必须提供 Android 12 使用的
NativeBridge v3 namespace 接口；加载、接口检查或初始化失败会直接报告给
ART，避免两个转译运行时共享进程状态。后端 DSO 在 zygote fork 后通过 ART 的
system linker namespace 和 `RTLD_NOW | RTLD_LOCAL` 按需加载，且只初始化所选
后端。ART 在降权前传入 nice name、应用数据目录和一次性候选选择。NDK 使用 canonical
AOSP guest userspace；Houdini 才会在应用私有 mount namespace 中，把其 guest 库、
二进制、linker config 和 cpuinfo 挂到预建的 canonical target。ABI 环境沿用后端
返回值，不修改全局 `ro.product.cpu.*`。

产品片段默认启用 `ro.floral.bridge.hybrid_elf=1`。配套 ART 补丁会为
选择 `hybrid` 的桥接 classloader 同时建立宿主和桥接 namespace；系统提供的
x86/x86_64 ELF 保持宿主所有权。PackageManager 会先扫描整包的公开 ARM ABI；只要
任一 APK 或 split 包含 ARM native 库，就只提取并选择 ARM ABI。只有完全不含 ARM、
但包含 x86/x86_64 native 库的应用才使用宿主 ABI。桥接进程的应用私有 ELF 全部交给
已选择的转译后端，不再因 ELF 标记为 x86 而回退宿主 namespace。加载结果会记录
真实句柄归属，确保 JNI 和卸载使用同一个所有者。
`direct` 不创建 Floral 宿主 namespace、不执行 ELF 分流，也不启用 Floral guest
identity；ART 像独立 NativeBridge 一样直接持有所选后端的 handle 和 callback 表。
混合模式不会在同一个 ELF 依赖图中混合架构。

NativeLoader 只使用 Android 计算出的公共库契约，把 guest classloader 链接到后端
namespace。它不会通过裸 soname 预加载 `libart.so`、`libnativeloader.so` 或
`libdl_android.so` 来代替 namespace 关系；后端运行时库始终由后端自己的 linker
拓扑管理。

框架集成默认给 32 位 WebView RELRO 创建预留 256 MiB 地址空间。使用超大
WebView provider 的产品可以通过 `ro.floral.webview.vmsize32` 和
`ro.floral.webview.vmsize64` 按字节覆盖；非正数会回退到默认值。

## Namespace 审计

配套 ART 补丁为 userdebug 和 eng 构建提供默认关闭、可持久化的 namespace 审计。
审计不会执行额外 `dlopen`，不会拦截 JNI、采集调用栈、创建线程或维护 TLS 状态。
启用后只记录 exported namespace 句柄、classloader namespace 创建结果、host/guest
链接归属、列表项数量、库加载归属，以及失败时 linker 返回的原始错误。

在启动待测进程前启用：

```sh
adb shell setprop persist.floral.nb.audit 1
adb logcat -c
adb shell am force-stop PACKAGE
adb shell monkey -p PACKAGE 1
adb logcat -d -v threadtime -s nativeloader:I
```

该属性和日志量会跨重启保留，采集结束后应关闭：

```sh
adb shell setprop persist.floral.nb.audit 0
```

## 测试

`floral_nativebridge_policy_test` 保留对共享后端选择数据类型的 focused tests；
Android 目标 `floral_nativebridge_probe` 只打开库并检查 `NativeBridgeItf` 导出，
不会在一个进程内初始化两个后端。
