# update-module.sh

一条命令产出「针对目标设备」的可刷模块包：编译 → 组装 → 打包，一步到位。

## 两种用法

**A. 自动采集（推荐）**——设备连着就行：

```bash
./tools/update-module.sh --auto
```

脚本自己 `adb shell uname -r`，自己 `dd` 设备上的 BTF 并 pull 回来，然后编译打包。
**你不需要手动记任何东西。**

**B. 手动给参数**——设备不在手边，或要用别的机器的数据：

```bash
./tools/update-module.sh 6.6.89-android15-8-g7e1f3c083cc6-abogki467167594-4k vmlinux.btf
```

## 可以省掉哪些文件

| 文件 | 能否省 | 条件 |
|---|---|---|
| `uname -r` | **不能** | 它是 vermagic 的一半，没有就无法产出能匹配的模块。但 `--auto` 让脚本自己读 |
| `vmlinux.btf` | **视后端而定** | 走 **RVH** 时完全不需要；走 **proc_ops** 时必须 |

**RVH 还是 proc_ops 由源码树决定**：

```
有 include/trace/hooks/cpuinfo.h  →  RVH   →  不需要 BTF
没有                              →  proc  →  必须 BTF
```

标准 GKI 分支里只有 `android15-6.6` 带 `trace/hooks/cpuinfo.h`。
5.10 / 5.15 / 6.1 / 6.18 的公开树都没有，所以那几个版本必须提供 BTF。

**如果 `--auto` 拉不到 BTF 且目标不是 6.6 系内核**，脚本会在编译阶段明确报错，不会静默产出半成品。

## 为什么不做到「完全零采集」

零采集意味着连 `uname -r` 都不要，那就只剩两条路：

**patch 内核绕过 vermagic 检查**——`check_modinfo` 被编译器内联进 `load_module`，
符号表里没有，无法按符号 patch；只能反汇编 `load_module` 找 `bl check_modinfo` 调用点逐个改。
需要改 `boot.img`，改坏会进不了系统。

**改用 KernelPatch / KPM**——它不走 LKM 路径，不需要 vermagic/CRC。但需要 APatch 环境，
与当前 KernelSU 方案不兼容。

两条路都比「采两个文件」成本高得多，所以没有实现。

## 产出

```
dist/cpuinfo-redirect-provider-<uname-r>.zip
```

内含：

```
module.prop                              版本与描述已更新（含产物 sha256 前 16 位）
post-fs-data.sh                          六因子匹配，从不 insmod
service.sh / uninstall.sh / customize.sh
META-INF/                                 Magisk / recovery 兼容
payload/cpuinfo_redirect-<主版本>.ko      本次为目标设备编译的产物
targets/<ko_sha256>.env                  精确匹配元数据（release + page + btf hash + ko hash）
tools/make-target-env.sh                 设备侧补全 config/btf 指纹用
```

## 选项

```
--out DIR        输出目录（默认 dist）
--template DIR   模块模板（默认 ../cpuinfo-redirect-driver）
--version VER    覆盖版本号（默认 UTC 时间戳）
--keep-all       保留模板里其它版本的 .ko（默认只留本次目标的那一个）
--keep-crc       保留 CRC（默认 --no-crc）
```

## 部署后验证

```bash
cat /data/adb/modules/cpuinfo-redirect-driver/state/status
# state=exact-match       六因子精确命中
# state=prefix-fallback   回退到前缀匹配（元数据没补齐）
# state=unsupported       这台机器没有对应产物
# runtime_test=not-run    固定值——本模块从不加载

cat /data/adb/modules/cpuinfo-redirect-driver/state/ko.path   # 命中的 .ko 路径
```

要补齐 `config_sha256` 让匹配升级到六因子：

```bash
su -c 'sh /data/adb/modules/cpuinfo-redirect-driver/tools/make-target-env.sh'
```
