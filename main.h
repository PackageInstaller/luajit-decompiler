/*
 * LuaJIT Decompiler v2 — Linux 移植版
 *
 * 本移植版：
 *   - 移除全部平台相关 API（文件系统/控制台/对话框），可跨平台编译
 *   - C++23 + CMake 构建
 *   - 编译时强制 -funsigned-char，保证字节处理与上游一致
 *
 * 字节码格式对照：LuaJIT 源码 src/lj_bcdump.h / src/lj_bc.h
 *   BCDUMP_VERSION=2，BCDUMP_F_BE=0x01, STRIP=0x02, FFI=0x04,
 *   FR2=0x08, BITOP=0x10
 */

#ifndef _CHAR_UNSIGNED
#error 需要 -funsigned-char（见 CMakeLists.txt），保证字节处理一致
#endif

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define DEBUG_INFO __FUNCTION__, __FILE__, __LINE__

constexpr char PROGRAM_NAME[] = "LuaJIT Decompiler v2 (Linux)";
constexpr uint64_t DOUBLE_SIGN = 0x8000000000000000;
constexpr uint64_t DOUBLE_EXPONENT = 0x7FF0000000000000;
constexpr uint64_t DOUBLE_FRACTION = 0x000FFFFFFFFFFFFF;
constexpr uint64_t DOUBLE_SPECIAL = DOUBLE_EXPONENT;
constexpr uint64_t DOUBLE_NEGATIVE_ZERO = DOUBLE_SIGN;

void print(const std::string& message);
void print_progress_bar(const double& progress = 0, const double& total = 100);
void erase_progress_bar();
void assert(const bool& assertion, const std::string& message, const std::string& filePath, const std::string& function, const std::string& source, const uint32_t& line);
std::string byte_to_string(const uint8_t& byte);

class Bytecode;
class Ast;
class Lua;

#include "bytecode/bytecode.h"
#include "ast/ast.h"
#include "lua/lua.h"
