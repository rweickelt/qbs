CppApplication {
    Probe {
        id: gccTester
        property bool isGcc: qbs.toolchain.contains("gcc")
        configure: {
            if (isGcc)
                console.info("is gcc");
            else
                console.info("is not gcc");
        }
    }

    files: "main.cpp"
}
