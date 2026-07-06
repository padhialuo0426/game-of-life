# 康威生命游戏

*[English](README.md)*

一个在终端里运行的交互式康威生命游戏，使用 C 语言编写、CMake 构建。这是一个
**无界沙盒**：世界没有墙，采用**稀疏方式**存储（只保留活细胞，放在一个哈希集合里），
因此内存和每代的计算量只正比于**存活细胞数**，与面积无关——滑翔机会永远飞下去。
你用鼠标**拖动平移**、**滚轮缩放**，可以在任意位置投放或绘制图案。棋盘以 **Sixel**
真像素图形绘制，界面下方有按钮栏。

> **需要支持 Sixel 的终端**（如 iTerm2、Konsole、WezTerm、foot、`xterm -ti vt340`、
> mlterm、较新的 Windows Terminal）。不支持 Sixel 的终端——包括 macOS 自带的
> Terminal.app——无法运行本程序；启动时会打印提示并退出。

## 构建

提供两个 CMake 预设：`debug` 和 `release`。

```sh
cmake --preset release
cmake --build --preset release
# 或：cmake --preset debug && cmake --build --preset debug
```

可执行文件生成在 `build/<preset>/game-of-life`。

如果系统有 **OpenMP**，构建时会自动启用，把每一代的计算分摊到多个 CPU 核心上
（对大图案帮助明显）；没有 OpenMP 则以单线程构建运行。可用 `OMP_NUM_THREADS`
环境变量调整使用的核心数。

## 安装 / 卸载

默认安装到用户主目录（无需 root）：

- 可执行文件 → `~/.local/bin/game-of-life`
- 默认图案 → `~/.config/game-of-life/default.cells`
  （遵循 `XDG_CONFIG_HOME`；不带 `-f` 运行时程序会自动加载它）

```sh
cmake --build --preset release          # 先构建
cmake --install build/release           # 安装
# 或进构建目录：cd build/release && make install

cmake --build build/release --target uninstall
# 或进构建目录：cd build/release && make uninstall
```

说明：

- 确保 `~/.local/bin` 在你的 `PATH` 中，才能直接敲 `game-of-life` 运行。
- 安装绝不会覆盖你已自定义的默认配置。
- **卸载**会删除可执行文件、默认图案和 `settings.json`（若配置目录
  `game-of-life` 变空则一并删除）。
- 系统级安装可覆盖前缀：
  `cmake --preset release -DCMAKE_INSTALL_PREFIX=/usr/local`，再
  `sudo cmake --install build/release`。

## 运行

```sh
./build/release/game-of-life                              # 默认 / 上次记住的配置
./build/release/game-of-life -f patterns/glider.cells    # 加载图案文件
./build/release/game-of-life -f patterns/pulsar.cells -w 40 -h 24  # 种子区域大小
./build/release/game-of-life --help                      # 全部选项
```

需要支持 Sixel 的交互式终端（见顶部的**要求**说明）。

## 操作

图像下方有六个按钮：
**Start / Pause / Step / Reset / Edit / Jump**
（开始 / 暂停 / 单步 / 重置 / 编辑 / 跳转）。没有 Quit 按钮——在任何界面按 `q`
（或 `Ctrl-C`）即可退出。

棋盘以 Sixel 位图绘制：每个细胞是一块方形像素，大小即当前缩放级别。

### 普通模式（按钮栏）

| 按键 | 作用 |
| --- | --- |
| **拖动**（左键） | 平移视口（抓取式） |
| **鼠标滚轮** | 以光标为锚点缩放 |
| `+` / `-` | 加速 / 减速模拟 |
| `c` | 把视口回中到图案上 |
| `f` | 切换跟随模式（每代自动回中） |
| `j` | 跳转到某一代（见下） |
| `x` | 清空世界为空白 |
| `s` / `l` | 保存 / 加载图案文件（RLE，见下） |
| `Tab` 或 `→` | 选择下一个按钮 |
| `←` | 选择上一个按钮 |
| `Space` 或 `Enter` | 触发选中的按钮 |
| `q` / `Ctrl-C` | 立即退出 |

- **Start 开始** —— 开始，或从暂停继续。
- **Pause 暂停** —— 冻结模拟（按 Start 从暂停处继续）。
- **Step 单步** —— 前进恰好一代然后暂停。可从任意暂停/重置状态使用，便于逐代观察。
- **Reset 重置** —— 重新加载初始配置（第 0 代）。
- **Edit 编辑** —— 进入编辑模式（见下）。
- **Jump 跳转** —— 跳到任意一代，向前或回溯（见下）。

状态栏显示状态、代数、摄像机位置 `Cam: (x,y)`、存活细胞数、当前缩放
（`Zoom: Npx`，每格像素数），以及跟随模式是否开启。

### 探索世界（平移、缩放、回中、跟随）

世界是无界的，终端显示的是它的一个**视口**：

- **按住左键拖动**平移（光标下的点保持不动）。
- **滚动滚轮**缩放，以光标为锚点——从放大后的大方块，缩到每格 1 像素，再进一步进入
  **亚像素缩放**：此时每个屏幕像素代表一整块细胞（块内只要有活细胞该像素就点亮）。
  亚像素缩放让远大于屏幕的图案也能一屏看全。状态栏显示 `Zoom: Npx`（每格 N 像素）
  或 `Zoom: 1px=Nc`（每像素 N 格）。
- **`c`** 把视口回中到活细胞的包围盒上——图案飞出屏幕时很有用。
- **`f`** 切换**跟随模式**，每代自动回中，这样你能盯着一个飞船一直看它飞而不跑出屏幕。

### 编辑模式

手动绘制或修改配置。一个闪烁的方框轮廓（细胞四周的黄色边框）标记当前格；它会闪烁，
这样你仍能看清该格当前是死是活。光标用方向键在无界世界里自由移动，视口会跟随它。

| 按键 | 作用 |
| --- | --- |
| 方向键 | 移动光标（视口跟随） |
| `Space` 或 `Enter` | 切换光标所在格的生死 |
| `Tab` 或 `Esc` | 退出编辑模式 |

退出时，编辑后的世界成为新的重启配置，因此之后按 **Reset** 会回到你画的图案。

### 跳转（回溯与穿越）

按 **`j`**（或 **Jump** 按钮），输入目标**代数**，回车即可跳过去——可前进也可回溯。
`Esc` 取消。

- **回溯**之所以可行，是因为最近若干代被保存在一个历史环里,回溯到附近的代是**瞬间**的。
  超出历史环范围的回溯会从第 0 代重放（康威生命游戏**不可逆**——前一代无法反算,只能
  被记住或重新推演出来）。
- **前进**靠真正运行模拟来快进,过程中会显示进度,且全程可中断。跳转途中 **`q` / `Ctrl-C`
  会立即退出程序**,**`Esc`** 只中止本次跳转(输入会被持续响应,所以即便在超大图案上跳很远,
  退出也能被及时处理)。对人口无限增长的图案（滑翔机枪、繁殖器）跳很远仍会变慢、吃内存——
  这是当前引擎的固有限制(不过每一代的计算已是多核并行,见[构建](#构建)),所以跳转设计成可中断。

### 保存与加载图案（RLE）

按 **`s`** 保存或 **`l`** 加载,输入文件名,回车确认(`Esc` 取消;路径相对于你启动
程序时所在的目录)。

格式是社区标准的 **RLE**(Golly 和 [LifeWiki](https://conwaylife.com/wiki/) 用的
那种),所以你可以加载从 wiki 下载的枪、飞船等图案,也能把自己搭的存下来分享。加载会
替换当前世界并把图案居中放到视口里;保存会写出当前世界的所有活细胞。例如一个滑翔机
存出来是:

```
x = 3, y = 3, rule = B3/S23
bo$2bo$3o!
```

(`.cells` 格式仍用于默认图案和 `-f`;save/load 用 RLE,因为它对大图案更紧凑。)

### Sixel 渲染

棋盘以真实的 **Sixel** 位图绘制：每个细胞变成一块像素，于是视口只受终端**像素数**限制——
在任意缩放下都能铺满整个窗口。缩过每格 1 像素后进入亚像素缩放（每像素代表一块细胞），
于是远大于屏幕的图案也能一屏看全。

启动时会通过 Device Attributes 查询探测 Sixel 支持。如果你的终端支持 Sixel 却没被
探测到，可以用环境变量强制开启：

```sh
GOL_SIXEL=1 game-of-life   # 跳过查询，直接认定终端支持 Sixel
```

设置 `GOL_SIXEL=0` 会强制关闭探测，此时程序会提示"需要支持 Sixel 的终端"并退出。

## 设置持久化

参数会以 JSON 文件形式在多次运行之间被记住：

```
$XDG_CONFIG_HOME/game-of-life/settings.json
# 若未设置 XDG_CONFIG_HOME：
~/.config/game-of-life/settings.json
```

- 当前生效的设置——种子区域尺寸、帧延迟、密度——会在**每次运行**时写入该文件
  （首次运行时创建），因此配置总会带到下次运行。
- 与存储设置对应的命令行选项（尺寸、延迟、密度）不仅对本次运行生效，还会被持久化——
  所以你传入的选项（如 `-w 50`）会成为下次记住的值。`-s`/`-f` 不会被存储。

`settings.json` 示例：

```json
{
  "width": 30,
  "height": 20,
  "wrap": false,
  "world": 2,
  "delay_ms": 120,
  "density": 0.250
}
```

`width`/`height` 是随机/加载图案起始所在的种子区域大小（世界本身无界）。`world`/`wrap`
现在恒为 `2`/`false`，仅为兼容旧版本保留。

## 选项

| 选项 | 说明 | 默认值 |
| --- | --- | --- |
| `-w, --width N` | 种子区域宽度（格） | 30（或记住的值） |
| `-h, --height N` | 种子区域高度（格） | 20（或记住的值） |
| `-d, --delay MS` | 每代之间的延迟（毫秒） | 120 |
| `-p, --density F` | 随机初始存活概率（0..1） | 0.25 |
| `-s, --seed N` | 随机种子（默认按时间） | — |
| `-f, --file PATH` | 从图案文件加载初始配置 | 默认路径 |

### 默认图案

不带 `-f` 时，程序会查找默认图案文件
`~/.config/game-of-life/default.cells`（遵循 `XDG_CONFIG_HOME`）。若存在则加载，
否则回退到随机开局（密度由 `-p` 指定，默认 0.25）。显式 `-f PATH` 读不到是硬错误，
但默认文件缺失不是——只会触发随机回退。

`make install` 会在那里放一个滑翔机作为 `default.cells`。若想换默认图案，
覆盖它即可，例如：

```sh
mkdir -p ~/.config/game-of-life
cp patterns/pulsar.cells ~/.config/game-of-life/default.cells
```

### 内置图案

`patterns/` 目录里有一批经典生命游戏图案。可在启动时用 `-f patterns/<名字>.cells`
加载，或在程序内按 `l` 键加载对应的 `.rle` 文件（每个图案都提供了两种格式）：

| 文件 | 类型 |
| --- | --- |
| `default.cells` | 滑翔机（安装为默认图案） |
| `glider.cells` | 滑翔机——飞船，周期 4 |
| `lwss.cells` | 轻量级飞船（LWSS） |
| `blinker.cells` | 振荡器，周期 2 |
| `toad.cells` | 振荡器，周期 2 |
| `beacon.cells` | 振荡器，周期 2 |
| `pulsar.cells` | 振荡器，周期 3 |
| `pentadecathlon.cells` | 振荡器，周期 15 |
| `block.cells` | 静物 |
| `beehive.cells` | 静物 |
| `r-pentomino.cells` | 玛土撒拉（1103 代后稳定） |
| `acorn.cells` | 玛土撒拉（5206 代后稳定） |
| `diehard.cells` | 玛土撒拉（130 代后完全消失） |
| `glider-gun.cells` | 高斯帕滑翔机枪（需要较宽的画布） |

## 配置文件格式

经典 `.cells` 纯文本格式的一个子集（见 `patterns/`）：

- 以 `!` 或 `#` 开头的行是注释。
- 图案行中，`.` 或空格表示死细胞；其他任意字符（通常是 `O`）表示活细胞。
- 图案会居中放入画布；超出画布的格子被裁剪。

```
! Glider
.O.
..O
OOO
```

## 许可证

本程序是自由软件，采用 GNU 通用公共许可证第 3 版（GPLv3）授权。完整文本见
[LICENSE](LICENSE) 文件。
