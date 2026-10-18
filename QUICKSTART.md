# 快速上手

三步：**采集 → 编译 → 刷入**。

## 第一步：采集（Windows，双击即可）

```
tools\grab-device-info.bat
```

设备连着、已 root，双击就跑。产物：`cr-device-info.tgz`（约 2 MB）。

里面只有三样东西：

| 文件 | 大小 | 作用 |
|---|---|---|
| `uname_r.txt` | 52 B | 构造 vermagic 的 `LOCALVERSION` |
| `vmlinux.btf` | 6 MB | 推导 `proc_dir_entry` 结构偏移 |
| `config.gz` | 45 KB | 判断 `MODVERSIONS` / `RANDSTRUCT` |

**不需要**：OEM 源码树、`Module.symvers`、厂商固件包。

## 第二步：编译（任意 x86_64 Linux / WSL2 / CI）

### 只要单个 .ko

```bash
tar xzf cr-device-info.tgz
./tools/adapt-new-device.sh "$(cat cr-device-info/uname_r.txt)" cr-device-info/vmlinux.btf
# 产物: dist/cpuinfo_redirect-<uname-r>.ko
```

### 要能直接刷的模块包

```bash
./tools/update-module.sh "$(cat cr-device-info/uname_r.txt)" cr-device-info/vmlinux.btf
# 产物: dist/cpuinfo-redirect-provider-<uname-r>.zip
```

### 依赖

```bash
sudo apt install -y build-essential bc flex bison libssl-dev libelf-dev \
                    gcc-aarch64-linux-gnu python3
```

## 第三步：刷入

把 zip 放进 KernelSU / Magisk 直接安装。

装完看状态：

```bash
cat /data/adb/modules/cpuinfo-redirect-driver/state/status
# state=exact-match       六因子精确命中，可用
# state=prefix-fallback   回退到主版本匹配
# state=unsupported       没有对应产物
# runtime_test=not-run    固定值，本模块从不加载

cat /data/adb/modules/cpuinfo-redirect-driver/state/ko.path   # 命中的 .ko
```

**验证伪装是否生效**：

```bash
su -c 'insmod $(cat /data/adb/modules/cpuinfo-redirect-driver/state/ko.path)'
grep -m1 'CPU implementer' /proc/cpuinfo
# 0x48 = Kirin（伪装成功）
# 0x51 = 原厂 Qualcomm（未生效）
```

## 没有 Linux 机器怎么办

两条路：

**① GitHub Actions** —— 仓库里已有 `.github/workflows/build-all.yml`，
把 `cr-device-info.tgz` 提交上去，手动触发 workflow，从 artifact 下载产物。

**② 设备端现场编译** —— 模块自带 `tools/adapt-on-device.sh`，
需要 Termux（clang + make）。刷入模块后如果发现没有本机产物，会自动在后台编译，
约 2–3 分钟，完成后重启即可。

```bash
# 手动触发一次
su -c 'sh /data/adb/modules/cpuinfo-redirect-driver/tools/adapt-on-device.sh'

# 看进度
cat /data/adb/modules/cpuinfo-redirect-driver/state/adapt.status
tail -f /data/adb/modules/cpuinfo-redirect-driver/state/adapt.log
```

## 工具清单

| 脚本 | 在哪跑 | 作用 |
|---|---|---|
| `tools/grab-device-info.bat` | Windows | 双击采集，产出 `cr-device-info.tgz` |
| `tools/grab-device-info.sh` | 设备 | bat 推上去执行的实际采集逻辑 |
| `tools/adapt-new-device.sh` | Linux | 编译单个 `.ko` |
| `tools/update-module.sh` | Linux | 编译并打包成可刷 zip |
| `tools/adapt-on-device.sh` | 设备 | 在设备上现场编译（Termux） |
| `tools/make-target-env.sh` | 设备 | 补全 config/btf 指纹让匹配升级到六因子 |
| `tools/build-for-device.sh` | Linux | 旧版：消费完整 `kx.tgz` |
| `tools/collect-for-build.sh` | 设备 | 旧版：采集完整 `kx.tgz` |
| `tools/gen_offsets.py` | 任意 | 从 BTF 生成 `cr_offsets.h` |

## 文档索引

| 文件 | 内容 |
|---|---|
| `QUICKSTART.md` | 本文——三步上手 |
| `COVERAGE.md` | 六版本覆盖矩阵、为什么不支持 5.5–5.9 |
| `KERNEL-ADAPTATION.md` | 五个兼容关卡的深度分析 |
| `HANDOVER.md` | 全部踩坑记录（编译补丁、属性事故、检测对抗） |
| `tools/README-adapt.md` | adapt-new-device.sh 细节 |
| `tools/README-update-module.md` | update-module.sh 细节 |
