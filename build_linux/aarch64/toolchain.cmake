set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
# NOTE: do NOT set CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY here. That
# would make FindThreads mis-detect pthread as "already in libc" and skip
# linking -lpthread, breaking the link on glibc targets (aarch64, ppc64le,
# i686, ...) where pthread is a separate library.
