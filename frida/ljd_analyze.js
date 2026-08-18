/*
 * ljd_analyze.js — 灵魂潮汐 (com.glkj.lhcx.gf) LuaJIT 运行时分析脚本
 *
 * 用法:
 *   adb forward tcp:27042 tcp:45678
 *   frida -H 127.0.0.1:27042 -f com.glkj.lhcx.gf -l frida/ljd_analyze.js
 *
 * 依赖: 设备上运行 frida-server (>=16), 建议以 root 启动以便写 /data/local/tmp。
 *
 * 功能:
 *   1. Frida 16 下 libtolua.so 通常不在脚本注入时映射, 这里轮询等待,
 *      并 hook android_dlopen_ext/dlopen 兜底;
 *   2. Hook luaL_loadbufferx/luaL_loadbuffer/luaL_loadstring/lua_loadx/
 *      luaL_loadfilex, 记录每个 chunk 的名字、长度、文件头(区分字节码/源码);
 *   3. 按 lua_State* 关联最近加载的 chunk, lua_pcall 失败时打印错误文本;
 *   4. 可按需把加载到的字节码/源码原样落盘, 直接用本仓库的反编译器处理。
 *
 * 说明: 该 so 为 LuaJIT 2.1 beta3 (luaJIT_version_2_1_0_beta3),
 *       与 luajit64/ 下磁盘字节码版本一致。
 */
'use strict';

const CONFIG = {
    SAVE_DUMPS: true,          // 是否把加载到的 chunk 写入设备
    DUMP_DIR: '',              // 留空则自动取应用 files 目录; 可手动指定
    LOG_LOAD: true,            // 打印每次 chunk 加载
    LOG_INTERNAL: false,       // 是否也打印 luaL_loadbuffer/lua_loadx 内部转发调用
    LOG_ERRORS: true,          // 打印 lua_pcall 失败的错误
    LOG_BACKTRACE: false,      // 打印 native 回溯 (用于定位加载调用方)
    MAX_CHUNK: 32 * 1024 * 1024,
    WAIT_MS: 120000,
    POLL_MS: 25,
};

const MODULE_NAME = 'libtolua.so';
const PACKAGE_NAME = 'com.glkj.lhcx.gf';

let hooksInstalled = false;
let dumpCounter = 0;
const seenChunks = new Set();       // "size:head" 去重
const stateChunk = new Map();       // lua_State* -> 最近加载的 chunk 名

function stateKey(L) {
    return L ? L.toString() : '0x0';
}

function hexOf(arrayBuffer) {
    const u8 = new Uint8Array(arrayBuffer);
    const parts = [];
    for (let i = 0; i < u8.length; i++) {
        parts.push(u8[i].toString(16).padStart(2, '0'));
    }
    return parts.join(' ');
}

function sanitizeName(name) {
    let s = (name || '').replace(/[^A-Za-z0-9._-]/g, '_');
    if (s.length > 96) s = s.slice(0, 96);
    return s || 'anon';
}

function printBacktrace(ctx) {
    try {
        const bt = Thread.backtrace(ctx, Backtracer.ACCURATE);
        for (const addr of bt) {
            const sym = DebugSymbol.fromAddress(addr);
            console.log('        ' + sym.toString());
        }
    } catch (e) {
        console.warn('[ljd] backtrace failed: ' + e);
    }
}

function ensureDumpDir() {
    try {
        const mkdir = () => {
            const File = Java.use('java.io.File');
            const dir = File.$new(CONFIG.DUMP_DIR);
            if (!dir.exists()) dir.mkdirs();
        };
        if (typeof Java.performNow === 'function') {
            Java.performNow(mkdir);
        } else {
            Java.perform(mkdir);
        }
    } catch (e) {
        // 纯 native 上下文或 Java 不可用时忽略, 写文件失败会自动回退。
    }
}

function resolveDumpDir() {
    if (CONFIG.DUMP_DIR) return CONFIG.DUMP_DIR;

    // 优先取应用私有 files 目录 (进程自身可写), 失败则回退标准路径。
    try {
        if (typeof Java.performNow === 'function') {
            let dir = null;
            Java.performNow(() => {
                const AT = Java.use('android.app.ActivityThread');
                const app = AT.currentApplication();
                if (app) dir = app.getFilesDir().getAbsolutePath();
            });
            if (dir) {
                CONFIG.DUMP_DIR = dir + '/ljd_dumps';
                return CONFIG.DUMP_DIR;
            }
        }
    } catch (e) {}

    CONFIG.DUMP_DIR = '/data/user/0/' + PACKAGE_NAME + '/files/ljd_dumps';
    return CONFIG.DUMP_DIR;
}

function saveChunk(chunkName, buff, size, isBytecode) {
    if (!CONFIG.SAVE_DUMPS || size <= 0 || size > CONFIG.MAX_CHUNK) return;

    const head = hexOf(buff.readByteArray(Math.min(16, size)));
    const key = size + ':' + head;
    if (seenChunks.has(key)) return;
    seenChunks.add(key);

    const ext = isBytecode ? 'luac' : 'lua';
    const base = sanitizeName(chunkName);

    resolveDumpDir();
    ensureDumpDir();

    const writeTo = (dir) => {
        const fname = dir + '/' + base + '_' + dumpCounter++ + '.' + ext;
        try {
            const f = new File(fname, 'wb');
            f.write(buff.readByteArray(size));
            f.close();
            console.log('[dump] ' + fname + ' (' + size + ' bytes)');
            return true;
        } catch (e) {
            console.warn('[dump] write failed (' + dir + '): ' + e);
            return false;
        }
    };

    if (writeTo(CONFIG.DUMP_DIR)) return;

    // 子目录创建失败时, 回退到应用 files 目录根或 /data/local/tmp 平铺保存。
    const fallbacks = [
        '/data/user/0/' + PACKAGE_NAME + '/files',
        '/data/local/tmp',
    ];
    for (const dir of fallbacks) {
        if (writeTo(dir)) return;
    }
}

function handleLoad(L, buff, size, namePtr, kind) {
    let chunkName = '';
    try {
        if (namePtr && !namePtr.isNull()) chunkName = namePtr.readCString() || '';
    } catch (e) {}

    const sizeInt = size.toInt32 ? size.toInt32() : parseInt(size, 10);
    stateChunk.set(stateKey(L), chunkName);

    const isBytecode = sizeInt >= 3 && buff.readU8() === 0x1b
        && buff.add(1).readU8() === 0x4c && buff.add(2).readU8() === 0x4a;

    if (CONFIG.LOG_LOAD) {
        const head = hexOf(buff.readByteArray(Math.min(16, sizeInt)));
        console.log('[load] L=' + L + ' kind=' + kind + ' ' + (isBytecode ? 'bytecode' : 'source')
            + ' size=' + sizeInt + ' name="' + chunkName + '"');
        console.log('        head: ' + head);
        if (CONFIG.LOG_BACKTRACE) printBacktrace(this.context);
    }

    if (sizeInt >= 0 && sizeInt <= CONFIG.MAX_CHUNK) {
        saveChunk(chunkName, buff, sizeInt, isBytecode);
    }
}

function installHooks() {
    if (hooksInstalled) return;

    const mod = Process.findModuleByName(MODULE_NAME);
    if (!mod) {
        console.warn('[ljd] ' + MODULE_NAME + ' not mapped yet, keep polling...');
        return;
    }
    hooksInstalled = true;
    console.log('[ljd] ' + MODULE_NAME + ' base=' + mod.base + ' size=' + mod.size);
    console.log('[ljd] dump dir: ' + resolveDumpDir());
    ensureDumpDir();

    const ver = Module.findExportByName(MODULE_NAME, 'luaJIT_version_2_1_0_beta3');
    console.log('[ljd] LuaJIT 2.1 beta3 marker @ ' + ver);

    const exports = [
        // name, 参数: L=0 buff=1 size=2 name=3 mode=4
        ['luaL_loadbufferx', 5],
        ['luaL_loadbuffer', 4],
        ['luaL_loadstring', 2],
        ['luaL_loadfilex', 3],
        ['lua_loadx', 5],
        ['lua_load', 5],
    ];

    for (const [sym, argc] of exports) {
        const addr = Module.findExportByName(MODULE_NAME, sym);
        if (!addr) {
            console.warn('[ljd] export not found: ' + sym);
            continue;
        }

        Interceptor.attach(addr, {
            onEnter(args) {
                this.L = args[0];

                if (sym === 'luaL_loadbufferx' || sym === 'luaL_loadbuffer') {
                    if (sym === 'luaL_loadbuffer' && !CONFIG.LOG_INTERNAL) return;
                    handleLoad(this.L, args[1], args[2], args[3], sym);
                } else if (sym === 'luaL_loadstring') {
                    const s = args[1];
                    let len = 0;
                    try { len = s.readCString().length; } catch (e) {}
                    handleLoad(this.L, s, ptr(len), null, sym);
                } else if (sym === 'luaL_loadfilex') {
                    let fname = '';
                    try { fname = args[1].readCString() || ''; } catch (e) {}
                    console.log('[load] L=' + this.L + ' kind=' + sym + ' file="' + fname + '"');
                    if (CONFIG.LOG_BACKTRACE) printBacktrace(this.context);
                } else if (sym === 'lua_loadx' || sym === 'lua_load') {
                    if (!CONFIG.LOG_INTERNAL) return;
                    let chunkName = '';
                    try { chunkName = args[3].readCString() || ''; } catch (e) {}
                    console.log('[load] L=' + this.L + ' kind=' + sym + ' name="' + chunkName
                        + '" (reader=' + args[1] + ' data=' + args[2] + ')');
                    if (CONFIG.LOG_BACKTRACE) printBacktrace(this.context);
                    // 注意: 不要在这里调用 reader, 会提前消费输入导致加载失败。
                }
            }
        });
    }

    // lua_pcall 失败时打印错误, 并按 lua_State 关联最近加载的 chunk。
    const pcallAddr = Module.findExportByName(MODULE_NAME, 'lua_pcall');
    if (pcallAddr && CONFIG.LOG_ERRORS) {
        Interceptor.attach(pcallAddr, {
            onEnter(args) {
                this.L = args[0];
            },
            onLeave(retval) {
                if (retval.toInt32() !== 0) {
                    const L = this.L;
                    const chunk = stateChunk.get(stateKey(L)) || '?';
                    try {
                        const luaTolstring = new NativeFunction(
                            Module.findExportByName(MODULE_NAME, 'lua_tolstring'),
                            'pointer', ['pointer', 'int', 'pointer']);
                        const lenPtr = Memory.alloc(Process.pointerSize);
                        const errPtr = luaTolstring(L, -1, lenPtr);
                        const errMsg = errPtr.isNull() ? '(no message)' : errPtr.readCString();
                        console.log('[error] L=' + L + ' chunk="' + chunk + '" pcall failed: ' + errMsg);
                    } catch (e) {
                        console.log('[error] L=' + L + ' chunk="' + chunk + '" pcall failed (code '
                            + retval.toInt32() + ')');
                    }
                }
            }
        });
    }

    const closeAddr = Module.findExportByName(MODULE_NAME, 'lua_close');
    if (closeAddr) {
        Interceptor.attach(closeAddr, {
            onEnter(args) {
                stateChunk.delete(stateKey(args[0]));
            }
        });
    }

    console.log('[ljd] hooks installed');
}

// Frida 16 下 so 可能是 spawn 之后才映射: 轮询直到出现, 超时后仍可通过
// dlopen hook 在加载瞬间安装。
function waitForModule() {
    const start = Date.now();
    const poll = () => {
        if (Process.findModuleByName(MODULE_NAME)) {
            installHooks();
            return;
        }
        if (Date.now() - start > CONFIG.WAIT_MS) {
            console.error('[ljd] timeout waiting for ' + MODULE_NAME
                + ' (ls /proc/' + Process.id + '/maps 或稍后手动 installHooks())');
            return;
        }
        setTimeout(poll, CONFIG.POLL_MS);
    };
    poll();
}

function hookDlopen() {
    for (const sym of ['android_dlopen_ext', 'dlopen']) {
        const addr = Module.findExportByName(null, sym);
        if (!addr) continue;
        Interceptor.attach(addr, {
            onEnter(args) {
                try {
                    const path = args[0].readCString() || '';
                    this.want = path.includes('tolua');
                } catch (e) {
                    this.want = false;
                }
            },
            onLeave() {
                if (this.want) setTimeout(installHooks, 0);
            }
        });
    }
}

waitForModule();
hookDlopen();

// 手动触发入口: 若轮询超时, 可在 REPL 里执行 installLjdHooks()
globalThis.installLjdHooks = installHooks;
// 写盘自检: REPL 里执行 testLjdDump() 验证落盘目录可用
globalThis.testLjdDump = function () {
    resolveDumpDir();
    ensureDumpDir();
    try {
        const probe = Memory.alloc(3);
        probe.writeByteArray([0x1b, 0x4c, 0x4a]);
        const f = new File(CONFIG.DUMP_DIR + '/.probe', 'wb');
        f.write(probe.readByteArray(3));
        f.close();
        console.log('[ljd] probe OK: ' + CONFIG.DUMP_DIR + '/.probe');
    } catch (e) {
        console.warn('[ljd] probe failed: ' + e);
    }
};
