# llvm-objcopy shim

structck.py 需要 llvm-objcopy 把裸字节包成 ELF，再由 llvm-objdump 反汇编，
用作与改写器无关的独立分母。CommandLineTools 有 llvm-objdump 但没有 llvm-objcopy，
brew 的 llvm 公式约 1.5 GB。

这里的 llvm-objcopy 是 Python 替身，只实现 structck 用到的那一种调用：
把输入字节写成只有一个 .text 段、起始地址为 0 的 ELF64。llvm-objdump 是到
CommandLineTools 的符号链接。

用法：

    export LLVM_BIN=games/dora/tools/llvmshim
    python refs/by3-port/games/totk/third_party/d2r/structck.py <改写产物目录> <模块> <NSO>
