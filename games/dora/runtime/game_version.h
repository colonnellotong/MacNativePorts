/* 哆啦A梦 牧场物语 自然王国与和乐家人 — base v0
   main    build ID 53CC82C1C5CAE8D42EA7EEAE165E5752
   sdk     build ID 3644A6760469451CABC99F00C540ACC2A64AEE41
   subsdk0 build ID D0F13015E5BADF2243D60A2BC9B978DDDBA80608
   均由 games/dora/scripts/inspect_npdm.py 从 main.npdm 与 NSO 头读出，完整清单见 assets/expected-v0.json。
   本作没有任何游戏代码地址被定义：TOTK 的 GAME_ADDR 映射不适用，所以任何遗留引用都应编译报错。
   Doraemon 的地址等真需要打补丁时再在此处命名。

   这是继王国之泪、Bayonetta 3 之后第三个接入的游戏，也是第一个非自研引擎。
   与 by3 的差别写在这里，避免后来者重新推导：
     - 引擎：Unity IL2CPP + Addressables + CRIWARE ADX2/Sofdec2，不是自研 NVN 引擎
     - 模块：main / sdk / subsdk0 / rtld 四件，nnSdk 独立成 sdk 模块，内核边界只在 sdk（76 svc + 42 cntpct）
     - 版本：本体 v0 单独可跑；1.3.0 更新（0100F5E0177AC800）尚未合并 ExeFS/RomFS
     - rtld 与 by3 逐字节相同，说明同一 NintendoSDK 版本，宿主 ABI 不需要分叉
*/
#pragma once

#define GAME_DORA 1
#define TOTK_GAME_VERSION "1.0.0"
#define TOTK_GAME_DIR "v0"
#define GAME_TITLE_ID 0x0100f5e0177ac000ULL

/* 服务权限 47 项，文件系统只授 RomFS 只读整分区，无 SaveData 权限。
   Unity 的存档走 fs_host 的挂载点，不经过主机授权。 */
#define GAME_SERVICE_COUNT 47
