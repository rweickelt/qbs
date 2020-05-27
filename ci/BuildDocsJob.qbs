// Linux, Windows, Macos
Job {

    property string qtVersion

    name: "build-docs-" + os + "-" + osx_image + "-" + qtVersion
    tags: "build-job"

    InstallQtStep {
        name: "install-qt"
        qtVersion: product.qtVersion
    }
    Step {
        name: "build-docs-with-qbs"
        inputs: "install-qt"
        command: "mkdir"
        arguments: [ "-p", "release/install-root" ]
    }
    Export {
        name: "qbs-binaries"
        paths: "release/install-root"
    }
}
