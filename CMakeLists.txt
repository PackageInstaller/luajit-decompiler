cmake_minimum_required(VERSION 3.20)

project(luajit-decompiler-v2
	VERSION 2.0.0
	DESCRIPTION "LuaJIT bytecode decompiler (Linux port, C++23)"
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# 原项目按 MSVC /J 编译（char 无符号）。Linux 需等价选项，否则字节码
# 字节处理（write_string 等）与 Windows 版行为不一致。
add_compile_options(-funsigned-char)

add_executable(luajit-decompiler-v2
	main.cpp
	bytecode/bytecode.cpp
	bytecode/prototype.cpp
	ast/ast.cpp
	lua/lua.cpp
)

target_compile_definitions(luajit-decompiler-v2 PRIVATE _CHAR_UNSIGNED)

# 产物名为 luajit-decompiler（不带 -v2 后缀），并提供安装规则
set_target_properties(luajit-decompiler-v2 PROPERTIES OUTPUT_NAME luajit-decompiler)
install(TARGETS luajit-decompiler-v2 RUNTIME DESTINATION bin)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(luajit-decompiler-v2 PRIVATE -Wall -Wextra)
endif()
