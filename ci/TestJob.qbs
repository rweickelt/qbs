Job {

    property string qtVersion

    name: "test-qbs-" + os + "-" + osx_image + "-" + qtVersion
    tags: "test-job"

    Depends { productTypes: "build-job"; required: false }

    InstallQtStep {
        name: "install-qt"
        qtVersion: product.qtVersion
    }

    Step {
        name: "api"
        inputs: ["install-qt", "setup-toolchains"]
        command: "echo"
    }

    Step {
        name: "language"
        inputs: ["install-qt", "setup-toolchains"]
        command: "echo"
    }

    Step {
        name: "blackbox"
        inputs: ["install-qt", "setup-toolchains"]
        command: "echo"
    }
}
