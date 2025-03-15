// Checks simple case with a single module.
CppApplication {
    name: "single-mod"
    condition: {
        if (qbs.toolchainType === "msvc"
            || ((qbs.toolchainType === "gcc" || qbs.toolchainType === "mingw")
                && cpp.compilerVersionMajor >= 11)
            || (qbs.toolchainType === "clang" && cpp.compilerVersionMajor >= 16)) {
            return true;
        }
        console.info("Unsupported toolchainType " + qbs.toolchainType);
        return false;
    }
    consoleApplication: true
    files: [
        "/usr/local/Cellar/llvm/19.1.7_1/share/libc++/v1/std.cppm",
        "main.cpp"
    ]
    cpp.cxxLanguageVersion: "c++23"
    cpp.forceUseCxxModules: true
    // cpp.treatWarningsAsErrors: true
}
