# 康威生命游戏

*[English](README.md)*

一个在终端里运行的交互式康威生命游戏，使用 C 语言编写、CMake 构建。纯键盘操作，
方形细胞网格，界面下方有按钮栏。世界可以是**有限世界**（图案飞出边界即消失，默认）
或**环面世界**（边缘环绕）。画布大小、世界类型等参数会在多次运行之间被记住。

## 构建

提供两个 CMake 预设：`debug` 和 `release`。

```sh
cmake --preset release
cmake --build --preset release
# 或：cmake --preset debug && cmake --build --preset debug
```

可执行文件生成在 `build/<preset>/game-of-life`。

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
./build/release/game-of-life -f patterns/pulsar.cells -w 40 -h 24
./build/release/game-of-life --help                      # 全部选项
```

需要交互式终端。

## 操作

网格下方有七个按钮：
**Start / Step / Pause / Stop / Edit / Canvas / Quit**
（开始 / 单步 / 暂停 / 停止 / 编辑 / 画布 / 退出）。

每个细胞用两个字符宽绘制，以抵消终端字符“竖长”的比例，让网格看起来接近正方形。

### 普通模式（按钮栏）

| 按键 | 作用 |
| --- | --- |
| `Tab` 或 `→` | 选择下一个按钮 |
| `←` | 选择上一个按钮 |
| `Space` 或 `Enter` | 触发选中的按钮 |
| `q` / `Ctrl-C` | 立即退出 |

- **Start 开始** —— 开始，或从暂停继续。
- **Step 单步** —— 前进恰好一代然后暂停。可从任意暂停/停止状态使用，便于逐代观察。
- **Pause 暂停** —— 冻结模拟。
- **Stop 停止** —— 停止并重置回初始配置（第 0 代）。
- **Edit 编辑** —— 进入编辑模式（见下）。
- **Canvas 画布** —— 进入画布模式：调整尺寸和/或切换世界类型（见下）。
- **Quit 退出** —— 退出程序。

### 编辑模式

手动绘制或修改初始配置。一个闪烁的空心方块光标 `[]` 标记当前格；它会闪烁，
这样你仍能看清该格当前是死是活。

| 按键 | 作用 |
| --- | --- |
| 方向键 | 移动光标 |
| `Space` 或 `Enter` | 切换光标所在格的生死 |
| `Tab` 或 `Esc` | 退出编辑模式 |

退出时，编辑后的网格成为新的初始配置，因此之后按 **Stop** 会重置回你画的图案。

### 画布模式

修改画布尺寸和/或世界类型。已有图案会被保留，居中放入新尺寸中（超出新边界的
格子会被裁剪）。

| 按键 | 作用 |
| --- | --- |
| `←` / `→` | 减小 / 增大宽度 |
| `↓` / `↑` | 减小 / 增大高度 |
| `Space` | 切换世界类型：有限 ↔ 环面 |
| `Enter` | 应用（并记住新设置） |
| `Tab` 或 `Esc` | 取消 |

尺寸限制在 宽 3..120 / 高 3..60。只有当尺寸真的改变时，应用才会把模拟重置到
第 0 代，因此纯切换世界类型不会打断正在运行的模拟。应用后的改动会写入
`settings.json`（见下）。

### 世界类型：有限 vs 环面

- **有限（默认）** —— 边界之外一律视为死细胞。飞出边缘的图案撞墙后消失
  （滑翔机撞到角落会留下一个小的稳定图案残骸）。
- **环面** —— 边缘环绕（上↔下、左↔右），飞出一边的滑翔机会从对面重新出现。

用 `--wrap` 以环面模式启动，或随时在画布模式里切换。

## 设置持久化

参数会以 JSON 文件形式在多次运行之间被记住：

```
$XDG_CONFIG_HOME/game-of-life/settings.json
# 若未设置 XDG_CONFIG_HOME：
~/.config/game-of-life/settings.json
```

- 首次运行时（读取不到设置文件），会用当前生效的设置创建该文件。
- 在**画布**模式应用改动后，新的画布尺寸和世界类型会写回文件，于是下次运行就
  按同样的配置启动——不用每次重新调一遍。
- 命令行选项会为本次运行覆盖文件中存储的值。

`settings.json` 示例：

```json
{
  "width": 30,
  "height": 20,
  "wrap": false,
  "delay_ms": 120,
  "density": 0.250
}
```

## 选项

| 选项 | 说明 | 默认值 |
| --- | --- | --- |
| `-w, --width N` | 画布宽度（格） | 30（或记住的值） |
| `-h, --height N` | 画布高度（格） | 20（或记住的值） |
| `-d, --delay MS` | 每代之间的延迟（毫秒） | 120 |
| `-p, --density F` | 随机初始存活概率（0..1） | 0.25 |
| `-s, --seed N` | 随机种子（默认按时间） | — |
| `-f, --file PATH` | 从图案文件加载初始配置 | 默认路径 |
| `--wrap` | 以环面（环绕）世界启动 | 有限 |

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

`patterns/` 目录里有一批经典生命游戏图案，可用 `-f patterns/<名字>.cells` 加载：

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
