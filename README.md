# LuaJIT Decompiler v2

LuaJIT 字节码反编译器。本仓库基于
[marsinator358/luajit-decompiler-v2](https://github.com/marsinator358/luajit-decompiler-v2)
维护，版本记录见 [CHANGELOG.md](CHANGELOG.md)。

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
| `-j`, `--jobs N` | 并行文件数，默认 `hardware_concurrency()`；`1` 为串行 |

## 已知限制

- 部分复杂条件赋值（多分支、多路径）仍可能输出为 if 形式；
- 多返回值结果进条件时保留多赋值（第二个结果还要用，不能整体内联）；
- 写入者不支配使用点的跨块临时槽保守保留。

## 参考

- 上游项目：[marsinator358/luajit-decompiler-v2](https://github.com/marsinator358/luajit-decompiler-v2)
- LuaJIT 源码：[LuaJIT/LuaJIT](https://github.com/LuaJIT/LuaJIT)
- 布尔表达式反编译算法论文：
  [www.cse.iitd.ac.in/~sak/reports/isec2016-paper.pdf](https://www.cse.iitd.ac.in/~sak/reports/isec2016-paper.pdf)
