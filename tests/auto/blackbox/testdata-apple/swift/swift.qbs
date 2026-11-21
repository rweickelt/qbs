import qbs

Project {
    SwiftApplication {
        name: "hello"
        consoleApplication: true
        codesign.enableCodeSigning: false
        files: ["hello/main.swift", "hello/helper.swift"]
    }
    SwiftLibrary {
        name: "Greeter"
        type: "staticlibrary"
        codesign.enableCodeSigning: false
        files: ["static/Greeter.swift"]
        Properties {
            condition: qbs.targetOS.includes("darwin")
            bundle.isBundle: false
        }
    }
    SwiftApplication {
        name: "staticapp"
        consoleApplication: true
        codesign.enableCodeSigning: false
        Depends { name: "Greeter" }
        files: ["static/main.swift"]
    }
    SwiftLibrary {
        name: "DynGreeter"
        type: "dynamiclibrary"
        codesign.enableCodeSigning: false
        files: ["dynamic/Greeter.swift"]
        Properties {
            condition: qbs.targetOS.includes("darwin")
            bundle.isBundle: false
        }
    }
    SwiftApplication {
        name: "dynapp"
        consoleApplication: true
        codesign.enableCodeSigning: false
        Depends { name: "DynGreeter" }
        files: ["dynamic/main.swift"]
    }
}
