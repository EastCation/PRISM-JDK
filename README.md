# PRISM-MCJDK

PRISM-MCJDK 是面向 Minecraft 模组客户端的 OpenJDK 21 定制发行版，发行商为 **MoeRain Software Developments**。

这不是简单重打包的通用 JDK，而是针对 Minecraft 模组客户端做了默认值、启动体验和 Profile/JIT 缓存设计的发行版。本 README 依据测试实例的 `latest.log` 整理。该日志时间为 **2026-11-10 00:14:42 至 00:25:53**，实际由本 JDK 启动并进入单人世界 `模组测试`，退出时正常保存，没有 JVM 崩溃。

## 这是什么

PRISM-MCJDK 的目标不是替代通用 JDK，而是让 Minecraft 模组客户端“开箱即用”：

- 默认 JIT 阈值、代码缓存、Windows 计时器精度、Integer 自动装箱缓存按 MC 场景调整
- 内置方法 Profile 记录与 MC Native Cache（持久化 JIT 缓存，实验性）
- 客户端启动时自带“正在优化 Java”Swing 进度窗口
- 可选内置控制台：实时日志、堆内存、退出/崩溃后保存日志
- 针对大型 NeoForge 整合包验证：Sodium、Lithium、Iris、Create、Mekanism、AE2 等

## 与其他 JDK 的对比

| 维度 | PRISM-MCJDK | Adoptium Temurin | Azul Zulu | IBM Semeru | OpenJDK 参考实现 |
| --- | --- | --- | --- | --- | --- |
| 虚拟机 | HotSpot | HotSpot | HotSpot | OpenJ9 | HotSpot |
| 定位 | Minecraft 模组客户端 | 通用/社区 LTS | 通用/企业 | 云原生/低内存 | 参考实现 |
| MC 专用默认值 | **有** | 无 | 无 | 无 | 无 |
| JIT / CodeCache 调优 | **有** | 无 | 无 | OpenJ9 自有 JIT | 无 |
| Profile / 持久化 JIT 缓存 | **内置（实验性）** | 无 | ReadyNow（商业） | Shared Classes / AOT | 无 |
| 启动进度 / 控制台 | **内置** | 无 | 无 | 无 | 无 |
| Windows 计时器优化 | **有** | 无 | 无 | 无 | 无 |
| 安全更新 / LTS | 目前基于 21+35，未跟踪 21.0.x | 有 | 有 | 有 | 有 |
| 平台覆盖 | Windows x64（当前验证） | 多平台 | 多平台 | 多平台 | 多平台 |
| 商业支持 | 无 | 社区/商业 | 有 | 有 | 无 |
| 最佳用途 | MC 整合包 / 客户端 | 通用生产 | 企业/多平台 | 低内存/云 | 参考/构建基础 |

说明：Zulu 的 ReadyNow 属于商业特性；Semeru 的 OpenJ9 与 HotSpot 在 GC、JIT 和 JNI 行为上不同，部分模组/Agent 可能需要额外适配；OpenJDK 24+ 的 Leyden/AOT 属于后续版本，不在 21 基线内。

### 主要优点

1. **开箱即用的 MC 默认值**：Tier3/4 编译阈值更低、CodeCache 512M、`ForceTimeHighResolution=true`、`AutoBoxCacheMax=131072`，减少大型整合包的预热和定时器抖动。
2. **面向 MC 的 Profile / AOT 路线**：已内置方法 Profile 记录和 Native Cache 框架，后续可做 profile-guided JIT 与持久化机器码缓存；通用 JDK 通常不提供这类能力。
3. **启动体验**：客户端启动自动显示进度窗口；可选控制台直接查看日志、堆内存，并在退出/崩溃后保存日志。
4. **实测大型整合包**：在 79 个 mod jar、142 个日志 mod 文件（含 JarJar）、NeoForge 21.1.265 的实例上完成启动、进入世界、保存退出。
5. **Windows 构建链适配**：支持 VS 2026/v145，修复 MSYS2 make 识别、gawk 5.3 正则和中文 `/showIncludes` 依赖生成问题。
6. **品牌与可维护性**：发行商、启动器 RC 名称、崩溃报告中的 VM 名称均为 PRISM-MCJDK，便于区分和定位问题。

### 缺点与限制

1. **安全更新节奏**：当前基于 OpenJDK 21+35，尚未跟踪 21.0.x 安全补丁；公网服务器或高安全要求场景应优先 Temurin/Zulu/Semeru 的最新 LTS。
2. **Native Cache 仍属实验性**：`MCNativeCacheMode=restore` 会恢复机器码，但 inline cache / runtime stub 等场景尚未完全稳定，生产环境不要开启。
3. **Profile 子系统处于第一阶段**：目前主要完成记录/加载/查询，尚未把 Profile 回灌到 `MethodData` 实现完整的 profile-guided JIT。
4. **平台覆盖有限**：当前只验证 Windows x64；Linux/macOS/ARM 需要单独构建和测试。
5. **没有 TCK/正式认证**：属于自定义发行版，未做完整 JCK/TCK，模组兼容性目前只在单一整合包上验证。
6. **自定义默认值可能带来行为差异**：例如扩大 Integer 缓存会改变部分 `==` 身份语义；虽然符合规范，但极端依赖对象身份的代码需要回归测试。
7. **内置 Swing UI 依赖 java.desktop 与图形环境**：无头服务器应避免启用；控制台本身是可选参数，进度窗口只在客户端启动目标下自动出现。

**结论**：通用生产环境、公网服务器或需要长期安全更新的场景，优先选择 Temurin / Zulu / Semeru 的最新 LTS；本地 Minecraft 模组客户端、尤其是大型 NeoForge 整合包，PRISM-MCJDK 的优势在于 MC 默认值、启动体验和 Profile / AOT 路线。

## 验证结论

最新日志中的 JVM 信息：

```text
JVM identified as MoeRain Software Developments PRISM-MCJDK 64-Bit Server VM 21-internal-adhoc.Celestia.prismjdk
ModLauncher 11.0.5+main.901c6ea8 starting: java version 21-internal by MoeRain Software Developments; OS Windows 11 arch amd64
```

`java -version`：

```text
PRISM-MCJDK version "21-internal" 2023-09-19
PRISM-MCJDK Runtime Environment (build 21-internal-adhoc.Celestia.prismjdk)
PRISM-MCJDK 64-Bit Server VM (build 21-internal-adhoc.Celestia.prismjdk, mixed mode, sharing)
```

本次 PCL 启动使用的 Java 路径：

```text
F:\PRISMJDK\build\prism21-mcjdk\jdk\bin\java.exe
```

最终镜像路径：

```text
F:\PRISMJDK\build\prism21-mcjdk\images\jdk
```

## 测试环境

| 项目 | 值 |
| --- | --- |
| CPU | AMD Ryzen 7 8845H，8 核 / 16 线程 |
| 内存 | 32 GB DDR5-5600 |
| GPU | NVIDIA GeForce RTX 4060 Laptop / AMD Radeon 780M |
| 系统 | Windows 11 amd64 |
| Minecraft | 1.21.1 |
| NeoForge | 21.1.265 |
| FML | 4.0.45 |
| ModLauncher | 11.0.5 |
| LWJGL | 3.3.3 |
| mods 目录 | 79 个 jar |
| 日志发现 mod 文件 | 142 个（含 JarJar / 内嵌依赖） |
| 日志发现 library 文件 | 35 个 |
| Fabric-only 跳过 | 4 个 |

主要性能/内容模组包括 Sodium 0.8.13、Lithium 0.15.4、Iris 1.8.14、Create 6.0.10、Mekanism 10.7.19.85、AE2 19.2.18、JEI 19.70.0.467、Xaero 地图、Touhou Little Maid、Sable、Flywheel 等。

## 启动时间线（latest.log）

| 阶段 | 日志时间 | 相对启动 |
| --- | --- | --- |
| ModLauncher 启动 | 00:14:42.871 | 0.0 s |
| Datafixer 优化完成（866 ms） | 00:15:20.816 | 37.9 s |
| LWJGL Backend 初始化 | 00:15:44.656 | 61.8 s |
| Reloading ResourceManager | 00:17:01.272 | 138.4 s |
| Sound engine started | 00:17:42.891 | 180.0 s |
| 启动集成服务器 | 00:18:22.547 | 219.7 s |
| 世界加载完成（Time elapsed 1076 ms） | 00:18:27.624 | 224.8 s |
| 玩家登录世界 | 00:18:41.335 | 238.5 s |
| 退出并保存 | 00:25:53.834 | 671.0 s |

会话总时长约 **11 分 11 秒**。以上时间包含完整模组加载、资源重载和世界加载，不是受控微基准。

## 本次运行的核心 JVM 参数

来自 PCL 日志 `F:\PCL2\PCL\Log1.txt` 的最近一次启动：

```text
-XX:+UseG1GC
-XX:+ParallelRefProcEnabled
-XX:MaxGCPauseMillis=100
-XX:+UnlockExperimentalVMOptions
-XX:G1NewSizePercent=20
-XX:G1ReservePercent=20
-XX:InitiatingHeapOccupancyPercent=15
-XX:G1HeapRegionSize=16M
-Xmx10240m
-Dlog4j2.formatMsgNoLookups=true
-Dstdout.encoding=utf-8
-Dstderr.encoding=utf-8
-Dprism.launch.console=true
```

其中 `-Dprism.launch.console=true` 是 PRISM-MCJDK 内置控制台开关；`--launchTarget forgeclient` 会自动触发内置的“正在优化 Java”启动进度窗口，不需要额外参数。

说明：上面的 G1 参数来自测试实例的版本 JSON / PCL，不是 PRISM-MCJDK 硬编码的默认值；PRISM-MCJDK 自身负责的是 JIT、CodeCache、Windows 计时器和自动装箱缓存等默认值。

## JDK 内置优化与功能

### 运行时与 JIT 默认值

以下是 PRISM-MCJDK 相对通用 OpenJDK 的主要默认差异：

- `Tier3CompileThreshold`：2000 → 1000
- `Tier4InvocationThreshold`：5000 → 3000
- `Tier4CompileThreshold`：15000 → 8000
- `Tier3BackEdgeThreshold`：60000 → 30000
- `Tier4BackEdgeThreshold`：40000 → 20000
- `ReservedCodeCacheSize`：240M → 512M（64 位 tiered）
- `ForceTimeHighResolution`：false → true
- `AutoBoxCacheMax`：128 → 131072

### Profile 子系统

新增 `-XX:+MCProfile` 系列参数，用于记录和复用方法热度：

```text
-XX:+MCProfile
-XX:MCProfilePath=<profile 文件>
-XX:MCProfileMode=off|record|use|both|auto
-XX:+MCProfileVerbose
```

Profile 文件记录方法身份、字节码指纹、invocation/backedge 计数、最高编译级别和 code size。本地基准测试中可导出约 3400 个活跃方法。

### MC Native Cache

新增 `-XX:+MCNativeCache` 系列参数：

```text
-XX:+MCNativeCache
-XX:MCNativeCachePath=<cache 文件>
-XX:MCNativeCacheMode=off|record|use|both|auto|restore
-XX:+MCNativeCacheVerbose
```

当前 `record` / `use` / `both` 安全可用；`restore` 会尝试恢复 nmethod 机器码，但仍属于实验功能，**不要在生产整合包中开启**。

### 内置启动界面

- 客户端启动时自动显示“正在优化 Java...”Swing 进度窗口
- 窗口不可关闭，进度完成后自动关闭
- 触发条件：`--launchTarget ...client`，或显式 `-Dprism.launch.ui=true`

### 内置控制台

启用方式：

```text
-Dprism.launch.console=true
```

功能：

- 捕获 `System.out` / `System.err` 并显示在 Swing 控制台
- 状态栏显示 PID、Java 堆内存、运行时长
- “保存日志”按钮
- 正常退出时询问保存日志或直接关闭
- `System.exit` / JVM 关闭时由 shutdown hook 自动保存到  
  `%LOCALAPPDATA%\PRISM-MCJDK\logs\game-<时间>.log`
- 保存时附加游戏目录下的 `logs/latest.log`

可选自动化参数：

```text
-Dprism.console.autoclose=true
-Dprism.console.log=D:\logs\mc.log
```

## 日志中的已知问题

这些是测试实例的 **mod / 资源兼容性问题**，不是 JVM 崩溃：

- 4 个 Fabric-only jar 被 NeoForge 跳过：`litematica-fabric`、`fabric-language-kotlin`、`linuxssh`、`malilib-fabric`
- `Model loader 'athena:athena' not found` 出现 3 次
- Sable 报告未知方块 `copycats:copycat_catwalk`（4 次）
- Touhou Little Maid 的 YSM 属性解析失败（2 次）
- Railways 的战利品表引用 Quark / Nature's Spirit 注册键失败
- `Unknown variable: BIOME_PALE_GARDEN`（2 次）
- SSL handshake 失败（3 次，网络原因）

本次 `latest.log`：

- `ERROR` 行 329 条，`WARN` 行 1247 条，绝大多数来自 mod 资源与兼容性
- `OutOfMemory`：0
- 未发现 PRISM-MCJDK 产生的 `hs_err` 文件

实例目录中 2026-11-02 至 2026-11-04 的旧 `hs_err_pid*.log` 来自 Zulu / Temurin JDK，不是本 JDK。

## 构建

源码基线：OpenJDK 21+35（2023-09-19）。构建配置名：`prism21-mcjdk`。

关键配置参数：

```text
--with-vendor-name='MoeRain Software Developments'
--with-jdk-rc-name=PRISM-MCJDK
--with-toolchain-version=2026
--with-freetype=bundled
```

在 MSYS2 中构建：

```bash
make CONF=prism21-mcjdk images JOBS=4
```

构建链补丁包括：

- VS 2026 / v145 工具链支持
- MSYS2 `msys` / `cygwin` make 标识兼容
- gawk 5.3 对 `??_7` 正则的修复
- MSVC 中文 `/showIncludes` 依赖文件生成修复

## 使用建议

1. PCL 中选择 Java：`F:\PRISMJDK\build\prism21-mcjdk\jdk\bin`
2. 版本选择 NeoForge 1.21.1（本实例为 NeoForge 21.1.265）
3. 启动时会自动显示优化进度窗口
4. 需要游戏内控制台时，在 JVM 参数中加入：
   ```text
   -Dprism.launch.console=true
   ```
5. 如需启用 Profile 或 Native Cache，请自行追加对应 `-XX` 参数；不要开启 `MCNativeCacheMode=restore`

## 许可

本项目基于 OpenJDK，遵循 GPLv2 + Classpath Exception。PRISM-MCJDK 的品牌与定制修改由 MoeRain Software Developments 维护。ART 相关内容仅借鉴架构与 Profile/编译器设计，未直接复制 ART 代码。
