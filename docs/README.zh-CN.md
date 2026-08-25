# Floral 聚合 NativeBridge

`libmixbridge.so` 是 x86 Android 镜像使用的单一 NativeBridge 入口，负责
在每个进程内选择一个 ARM 转译后端，进程内不会同时驻留两个转译运行时。
`hybrid` 模式转发 Android 12 NativeBridge v6 回调；`direct` 模式由 ART 直接绑定
所选后端的 callback 表。
NDK Translation 和 Houdini 二进制不放入本仓库。

x86 Floral 产品应继承 `system/floral/nativebridge/nativebridge.mk`，由产品
片段安装库并设置 `ro.dalvik.vm.native.bridge=libmixbridge.so`；纯 ARM 产品
不要继承该片段。

Android 12 源码还需要 `redroid-patches/android-12.0.0_r32/art/` 下的 ART
配套补丁组。它们开放 ART 回调头文件、让桥接 classloader 按 ELF 架构选择
namespace、在 zygote 降权前传入真实进程身份，并把混合加载增强限制在明确选择
`hybrid` 的进程内。frameworks/base 配套补丁负责保存选择并处理早期 native 和
JNI 加载失败。

## 策略文件

宿主可选策略文件为 `/ipc/floral_stream/nativebridge.json`。init 在每次开机将其
复制到 Android 私有缓存。文件缺失或
格式错误时使用内置 `auto` 策略：64 位先尝试 NDK Translation，再尝试 Houdini；
32 位只使用 Houdini。
包规则优先于同包的进程规则，同包子进程继承包级后端选择。修改宿主策略后需要重启容器
刷新缓存，Android 应用无需读取宿主文件。

```json
{
  "version": 1,
  "preferred_backend": "ndk",
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

`preferred_backend` 保持自动回退，并把指定的 `ndk` 或 `houdini` 放在后端首位。
自动回退的完整候选序列保持同一种增强加载模型；默认顺序为
`ndk/hybrid`、`houdini/hybrid`。`direct` 仅允许显式对象规则同时设置
`selected_backend` 和 `selected_loader_mode` 时使用；它只负责选择后端，
NativeLoader namespace、库所有权和进程身份均保持 AOSP 与该后端的原始行为；
ART 直接调用所选后端的 callbacks，不再经过 `libmixbridge` 转发。
`default_backend`
仍表示显式默认后端，两者同时存在时以它为准。`abi.public`
控制应用通过 `Build.SUPPORTED_ABIS`
看到的 ABI；PackageManager 等框架内部仍使用构建时的 loader ABI，因此不会
把 x86 主机 ABI 从本地加载路径中移除。缺少 `abi.public` 时使用 ARM 兼容默认值。

原有字符串规则继续兼容。对象规则用于保存候选顺序和可选的显式
`selected_backend`；存在该字段时运行时只使用已选后端，Android 不会覆盖。
显式对象规则可通过 `selected_loader_mode` 选择 `hybrid` 或 `direct`，缺省为
`hybrid`。`direct` 不参与自动监控、学习或崩溃回退。
`auto` 规则由 system_server 在有界内存表中按包保存版本、后端和 loader mode 候选结果。ARM 转译进程在启动后 60 秒内
发生 native crash，或出现特定的 JNI `UnsatisfiedLinkError` 时，Android 会切换到
尚未失败的下一候选。只有包含 Activity
的前台主进程才恢复原任务；推送等 helper/service 进程只重启自身，不再销毁前台
任务。普通 Java 崩溃、ANR、非 ARM 进程及窗口外崩溃不参与回退。每个 hybrid 后端候选在
同一应用和系统版本中只尝试一次；只有应用包的全部进程退出后，成功候选才缓存 10 分钟。
缓存不写入 `/data/system`，system_server 重启后清空，且有固定容量上限。

顶层 `executables` 为系统级完整路径规则；包对象内的 `executables` 支持完整路径、
文件名或 `*`，且包规则优先。Android 12 的镜像生成脚本会把 ARM32/ARM64
`binfmt_misc` 注册统一指向 `/system/bin/floral_nativebridge_runner`，再由它按 ELF
位数和规则执行 NDK 或 Houdini。无法从路径唯一确认包归属时不会套用其他包规则。

可以通过只读属性覆盖后端路径：
`ro.floral.nativebridge.ndk` 和 `ro.floral.nativebridge.houdini`。
默认路径按 ABI 使用 `/system/lib64/` 或 `/system/lib/`。
独立 ELF runner 路径分别由 `ro.floral.nativebridge.runner.ndk64`、`houdini32`
和 `houdini64` 覆盖；32 位 NDK runner 不作为候选。

路由器不会监看进程，后端初始化后也不会在进程内切换。应用生命周期、早期
native crash 回退和任务恢复均由 Android 内部负责，不依赖宿主程序。候选结果由
system_server 通过 zygote 专用启动参数一次性传入，不使用运行状态文件。后端必须提供 Android 12 使用的
NativeBridge v3 namespace 接口；加载、接口检查或初始化失败会直接报告给
ART，避免两个转译运行时共享进程状态。后端 DSO 在 zygote fork 后通过 ART 的
system linker namespace 和 `RTLD_NOW | RTLD_LOCAL` 按需加载，失败候选关闭后再尝试下一个。
策略选择仍延迟到 zygote fork 之后，两种模式都只初始化所选后端。ART 在降权前明确传入
nice name、应用数据目录和一次性候选选择，不再依赖初始化阶段的
`/proc/self/cmdline`。ABI 环境沿用后端返回值，不修改全局
`ro.product.cpu.*`。

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

`floral_nativebridge_policy_test` 检查策略优先级；Android 目标
`floral_nativebridge_probe` 只打开库并检查 `NativeBridgeItf` 导出，不会在
一个进程内初始化两个后端。
