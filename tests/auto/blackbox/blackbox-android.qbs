import qbs

QbsAutotest {
    testName: "blackbox-android"
    Depends { name: "qbs_app" }
    Depends { name: "qbs-setup-toolchains" }
    Group {
        name: "testdata"
        prefix: "testdata-android/"
        files: ["**/*"]
        fileTags: []
    }
    files: [
        "../shared.h",
        "tst_blackboxbase.cpp",
        "tst_blackboxbase.h",
        "tst_blackboxandroid.cpp",
        "tst_blackboxandroid.h",
    ]
    // TODO: Use Utilities.cStringQuote
    cpp.defines: base.concat(['SRCDIR="' + path + '"'])
}
