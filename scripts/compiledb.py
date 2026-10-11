# PlatformIO pre-build script: put the Xtensa toolchain's own include paths into
# compile_commands.json (pio run -t compiledb) so clangd-based editors
# (Zed, VS Code + clangd, ...) can resolve <Arduino.h>, libstdc++, etc.
Import("env")  # noqa: F821  (provided by PlatformIO/SCons)

env.Replace(COMPILATIONDB_INCLUDE_TOOLCHAIN=True)  # noqa: F821
