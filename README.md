# LuaJIT Decompiler v2

LuaJIT 字节码反编译器。本仓库基于
[marsinator358/luajit-decompiler-v2](https://github.com/marsinator358/luajit-decompiler-v2)
维护，主要变更见下方版本记录。

## 版本变更

### v2.0  Linux 原生移植

上游为 Win32 / MSVC / C++20 项目，依赖 Windows API。本版改为可跨平台编译的
C++23 项目：

- 移除全部 Windows API：文件系统遍历改用 `<filesystem>`，文件读写改用
  `<fstream>`，控制台输出与错误提示改用 stdout/stderr，删除文件选择对话框
  与 MessageBox 交互逻辑；
- 新增 CMake 构建（C++23），无第三方依赖；
- 编译强制 `-funsigned-char` 并定义 `_CHAR_UNSIGNED`，保证字节处理与上游
  Windows 版本一致（见 CMakeLists.txt）；
- 调试/发布分支改用标准 `NDEBUG`（原代码使用 MSVC 专有的 `_DEBUG`）；
- 输出目录不存在时自动创建；
- README 改用中文编写。

### v2.0.1  字节码兼容性修正

对照 LuaJIT 源码（`src/lj_bc.h` 的 BCDEF 指令表、`src/lj_bcdump.h` 格式定义、
`src/lj_bcread.c` 读取逻辑）修正以下问题：

- **指令表对齐 LuaJIT 2.1**：上游指令表是 LuaJIT 2.0 全集，缺少 2.1 新增的
  7 个位运算指令（`BNOT`、`BAND`、`BOR`、`BXOR`、`BSHL`、`BSHR`、`BSAR`）。
  已按 BCDEF 顺序补入，并依据 `lj_bc.h` 修正 ABC/AD 格式归类
  （`BAND` 等 6 个为 ABC，`BNOT` 为 AD）；
- **支持 FR2 字节码**：`FR2`（0x08）是 LuaJIT 2.1 在 64 位/GC64 下的双槽帧
  标志，v2.0.1 明确支持此类字节码；
- **修复次正规数常量导致的反编译失败**：常量 `2^-1074`（位型
  `0x0000000000000001`）经 `%g` 格式化后，C 运行库 `strtod` 会下溢为 0，
  往返校验失败。修复为对次正规数输出 LuaJIT 支持的精确十六进制浮点字面量
  （如 `0x0.0000000000001p-1022`），`0.0` 仍按原逻辑输出 `0`；
- **剥离调试信息的字节码**：`STRIP`（0x02）标志下本地变量名、upvalue 名、
  行号均不存在，输出自动使用 `var_<函数>_<槽位>` 占位名，字符串常量
  （函数名、字段名、字面量）不受影响。

### v2.0.2  输出文件名修正

- 去掉输入文件名的 `.bytes` 包装后缀后再补 `.lua`，避免出现
  `xxx.lua.lua` 双后缀：
  - `xxx.lua.bytes` 输出为 `xxx.lua`；
  - 其他扩展名或无扩展名文件统一补 `.lua`。

### v2.0.3  字节码变体兼容性增强

- 放宽主原型必须 vararg/无参的限制（部分变体的顶层 chunk 带固定参数）；
- 处理"先读后写"的临时槽位（如 `x or {}` 短路模式），残留作用域统一
  闭合并自动命名；
- 多返回值调用（`CALL B=0`）放宽使用次数限制，TSETM 无法合并进表构造器
  时输出 `t[k] = f()` 形式，避免反编译中断；
- 条件/if 构建增加目标地址标签回退；无法消除的拷贝条件退化为纯测试；
- 泛型 for 支持 JMP 入口 + 循环体在前的变体布局；
- 槽位表按 128 分配，防御 framesize 小于实际槽位的字节码；
- 修复 RET/RETM 表达式填充循环在超过 255 个返回值时因 8 位下标回绕
  导致的死循环（改为 32 位下标）；
- 增加空变量、空 tableIndex、空 slotScope 的防御性检查。

### v2.0.4  位运算 AST 反编译

- 实现 LuaJIT 2.1 新增的 7 个位运算指令（`BNOT`、`BAND`、`BOR`、`BXOR`、
  `BSHL`、`BSHR`、`BSAR`）的 AST 反编译输出：
  - 一元按位取反：`~x`
  - 按位与/或/异或：`a & b`、`a | b`、`a ~ b`
  - 左移/右移/算术右移：`a << b`、`a >> b`、`a ~>> b`
- 接受字节码头中的 `BCDUMP_F_BITOP`（0x10）与原型标志
  `PROTO_BITOP`（0x80）。

## 构建

要求：支持 C++23 的编译器（GCC 13 或 Clang 16 以上）、CMake 3.20 以上。

```bash
cmake -B build -S .
cmake --build build -j
```

构建产物：`build/luajit-decompiler`。

## 使用

```bash
build/luajit-decompiler INPUT_PATH [选项]
```

`INPUT_PATH` 可以是单个 LuaJIT 字节码文件，也可以是目录（目录会递归处理，
输出保持相对目录结构）。默认输出到当前目录下的 `output/`。

选项：

| 选项 | 说明 |
|---|---|
| `-h`, `-?`, `--help` | 显示帮助 |
| `-o`, `--output PATH` | 指定输出目录 |
| `-e`, `--extension EXT` | 只处理指定扩展名的文件 |
| `-s`, `--silent_assertions` | 失败文件静默跳过（不中断） |
| `-f`, `--force_overwrite` | 覆盖已存在的输出文件 |
| `-i`, `--ignore_debug_info` | 忽略字节码调试信息 |
| `-m`, `--minimize_diffs` | 优化输出格式，减少差异 |
| `-u`, `--unrestricted_ascii` | 关闭默认 UTF-8 与字符串限制 |

## 已知限制

- 大端字节码（big endian）暂不支持；
- 条件赋值（conditional assignment）的反编译输出仍可能不够理想；

## 参考

- 上游项目：[marsinator358/luajit-decompiler-v2](https://github.com/marsinator358/luajit-decompiler-v2)
- LuaJIT 源码：[LuaJIT/LuaJIT](https://github.com/LuaJIT/LuaJIT)
- 布尔表达式反编译算法论文：
  [www.cse.iitd.ac.in/~sak/reports/isec2016-paper.pdf](https://www.cse.iitd.ac.in/~sak/reports/isec2016-paper.pdf)
