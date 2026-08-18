# 版本变更

本仓库基于
[marsinator358/luajit-decompiler-v2](https://github.com/marsinator358/luajit-decompiler-v2)
维护。条目按版本从新到旧排列。

### v2.0.10  表/闭包身份与方法 self

- 表构造器和函数表达式不再按“无副作用”复制到多个使用点：
  `local t = {}; t[k] = v; foo(t)` 以及 `setmetatable(t, t)` 会保留同一个局部表,
  不再写成 `;({})[k] = v` / `setmetatable({}, {})`；
- 闭包 upvalue 计入引用, 局部递归函数不再被收成 IIFE 后留下 `var_0(...)`；
- FR2 方法调用里从未写入的 self 空洞不再被邻近同槽变量填入,
  `obj.Find(<hole>, "path")` 还原为 `obj:Find("path")`；
- 清理未使用声明时会统计函数位置的引用, 避免把多次调用的
  `local function choose(...)` 当成死代码删掉。

### v2.0.9  支配树空转修复

- Lengauer-Tarjan 的 DFS 编号不再用 0 表示“未访问”（入口指令 id 也是 0）。
  回边指向入口时会把入口重新入栈, 写出环状支配树,
  `cfgDominates` 沿 idom 链走到死循环, 中等大小函数也会超过 30s 超时
  （碧蓝航线 `WSAtlasWorld.lua` / `Dorm3dRoomScene.lua`）；
- 支配查询改为欧拉序 O(1), 避免 7 万条指令的直线函数上 O(n²) 走链。

### v2.0.8  方法链折叠

**方法链整体折叠**

- 递归还原嵌套方法调用：原先只处理语句级 `obj.method(obj, ...)`，
  `self.BtnHide.GetComponent(self.BtnHide, "Button").onClick:AddListener(...)`
  这类长链的内层调用还原不到。现对表达式树后序遍历，输出
  `self.BtnHide:GetComponent("Button").onClick:AddListener(...)`；
- 允许任意接收者类型（全局表、调用结果、表下标），
  `MainUIApi.GetString(MainUIApi, ...)` 还原为 `MainUIApi:GetString(...)`；
- 识别编译器为方法调用插入的 MOV 副本（槽拷贝或 `self.View` 这类表下标拷贝）：
  `local a = Find(...); local b = a; a.GetComponent(b, ...)` 以及
  `local t = self.View; self.View.GetComponent(t, "Toggle")` 均还原为冒号调用；
- 循环体入口标签不再挡住紧邻单用途接收者内联：把标签转给使用点后删除写入语句，
  `local x = self[k]:GetComponent("Button").onClick; x:RemoveListener(v)`
  折叠为 `self[k]:GetComponent("Button").onClick:RemoveListener(v)`；
- 方法还原与跨块传播交替至固定点，折叠“还原后才变成单用途”的接收者临时槽。
- 按 `SlotScope*` 从整棵函数树收集 MOV 副本，调试局部把调用包进
  `DECLARATION` 子块时仍能还原冒号调用，并把
  `local a = Find("Image"); local b = a; local bg = a.GetComponent(b, "Image")`
  折叠为 `local bg = Find("Image"):GetComponent("Image")`。

**条件默认值与调试名拷贝**

- `local x = soulPOD.favor; if not soulPOD.favor then x = 0 end; self.favor = x`
  在条件已被内联成字段本身后仍识别，输出 `self.favor = soulPOD.favor or 0`；
- 合并单用途拷贝 `local var = expr; local name = var` 为 `local name = expr`，
  以及 `local var = expr; return var` 为 `return expr`。
  同一槽位号上连续两个 `SlotScope`（编译器 MOV 切开的调试名）按作用域身份
  区分，不再误判成同一个变量。
- `x = A; if A then x = B end` 折叠为 `x = A and B`
  （`ToggleTown:SetActive(self.showUI and Unlock(...))`）。

**仍保守拒绝（行为正确）**

- 多返回值结果进条件：`local ok, err = sock:listen(...); if not ok then` 保留多赋值；
- 写入者在分支里、使用点可从其它路径到达的跨块接收者；
- 条件拷贝：`transform.Find(var, ...)` 且 `var` 只在 if 分支赋值时不折叠。

### v2.0.7  多线程与临时变量削减

**多线程并行反编译**

- 新增 `-j, --jobs N` 选项，默认使用 `std::thread::hardware_concurrency()`；
- 采用现代 C++ 线程池：`std::jthread` + `std::atomic<size_t>` 原子任务游标
  分发独立文件任务，无队列锁竞争；
- 并行模式下进度条自动关闭，日志按输入顺序回放，输出确定可读；
- 串行模式（`-j 1`）保持实时日志输出，行为与旧版一致；
- CMake 通过 `find_package(Threads)` 链接线程库。

**临时变量（var_x）数量削减**

- 广义临时槽消除：使用点前连续的单用途槽赋值链整体内联，
  例如 `local f = table.insert; local t = x; f(t, ...)` 直接还原为
  `table.insert(x, ...)`，支持跨多条赋值与多返回值调用；
- 通用方法调用还原：`self.list.pushnode(self.list, node)` 还原为
  `self.list:pushnode(node)`，并内联接收者临时槽；
- 修复合并作用域误删：引用计数表在消除时同步递减，写入语句仅在无剩余
  引用时删除，杜绝输出未声明的 `var_<槽位>` 兜底名（全语料库从
  34503 处降到 0）；
- 修复 `build_multi_assignment` 合并后旧作用域未重定向导致的幽灵引用，
  例如 `file, err = io.open(...); if file == nil` 正确还原；
- 保留表构造器合并级联：config 大表仍输出为单个表构造器，不产生
  逐项临时变量。

**跨块拷贝传播**

- 新增 `propagate_cross_block_copies`：编译器为方法调用/表达式生成的临时槽
  常横跨 goto/label 重构出的多个块（如 `local var = self.list` 在 if 分支、
  使用在合并后的块），单趟链消除的连续窗口够不到。新 pass 用原始字节码
  CFG 的 Lengauer-Tarjan 支配树（O(n α(n)) / O(n) 内存，数万指令大函数
  安全）+ 无中间改写检查做保守传播，将单写入者临时槽的值替换到支配使用点，
  并在无剩余引用时删除写入语句；
- 传播移到 `build_multi_assignment` 之后并二次运行方法还原：避免破坏多返回
  值合并（`field_dict[key], pos = decode_value(...)` 可正常还原），同时把
  新内联的接收者转成 `obj:method()`；
- CFG 按 LuaJIT 实际语义建模：IS* 条件指令后紧跟的 JMP 由条件指令代表
  （分支到 JMP 目标 + 落到 i+2），修正条件跳转支配关系；
- 纯 RHS 槽位传播：常量/全局等无副作用写入者即使作用域被多写入（槽位复用
  或与真局部合并，如 `local var_6_0 = 2` 与 value 共享槽），只要写入者
  支配使用点且中间无同槽重写，即可安全内联 (`value + 2^n`)；
- 以函数级固定点迭代处理内联引入的新引用，避免顺序依赖留下幽灵槽；
- 子函数（闭包）upvalue 捕获的作用域不删除写入者；多返回值 CALL 的结果槽
  不参与传播（交给 `build_multi_assignment` 合并）；
- 兜底防护：任何残留的无名槽位作用域自动补名并在函数头部补声明，
  全语料库 `var_<槽位>` 兜底名为 0。
- 死声明清理：从未被引用、初始值无副作用的声明（含被闭包捕获保护的判断）
  直接删除；空声明仅在剥离调试信息的字节码中删除。
- 修复 `Expression` 构造函数在未初始化枚举上执行 `delete_type()` 的
  UBSan 告警。

### v2.0.6  条件赋值输出优化

- 识别 `local x; if not y then x = d end` 这类丢失初始读值的模式，
  还原为 `local x = y or d`；若变量随后直接写回同一目标，则进一步内联为
  `target = target or d`。

### v2.0.5  大端字节码支持

- 支持 `BCDUMP_F_BE`（0x01）标志，按 dump 端序读取指令、upvalue 引用与
  多字节行号，可反编译大端 LuaJIT 字节码。

### v2.0.4  位运算 AST 反编译

- 实现 LuaJIT 2.1 新增的 7 个位运算指令（`BNOT`、`BAND`、`BOR`、`BXOR`、
  `BSHL`、`BSHR`、`BSAR`）的 AST 反编译输出：
  - 一元按位取反：`~x`
  - 按位与/或/异或：`a & b`、`a | b`、`a ~ b`
  - 左移/右移/算术右移：`a << b`、`a >> b`、`a ~>> b`
- 接受字节码头中的 `BCDUMP_F_BITOP`（0x10）与原型标志
  `PROTO_BITOP`（0x80）。

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

### v2.0.2  输出文件名修正

- 去掉输入文件名的 `.bytes` 包装后缀后再补 `.lua`，避免出现
  `xxx.lua.lua` 双后缀：
  - `xxx.lua.bytes` 输出为 `xxx.lua`；
  - 其他扩展名或无扩展名文件统一补 `.lua`。

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
