// Linux, Windows, Macos
Job {

    property string qtVersion

    name: "build-qbs-" + os + "-" + osx_image + "-" + qtVersion
    tags: "build-job"

    InstallQtStep {
        name: "install-qt"
        qtVersion: product.qtVersion
    }

    Step {
        name: "build-qbs-with-qbs"
        inputs: "install-qt"
        command: "mkdir"
        arguments: [ "-p", "release/install-root" ]
    }

    Export {
        name: "qbs-binaries"
        paths: "release/install-root"
    }
}
