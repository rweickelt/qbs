Pipeline {

    Stage {
        name: "Build Stage"

        BuildQbsJob {
            os: "linux"
            qtVersion: "5.12.8"
        }

//        BuildQbsJob {
//            os: "macos"
//            osx_image: "xcode11.3"
//            qtVersion: "5.15.0"
//        }

//        BuildQbsJob {
//            os: "windows"
//            qtVersion: "5.15.0"
//        }

        BuildDocsJob {
            os: "linux"
            qtVersion: "5.12.8"
        }
    }

    Stage {
        name: "Test Stage"

        TestJob {
            os: "linux"
            qtVersion: "5.15.0"
        }

//        BlackboxQtTestJob {
//            os: "linux"
//            qtVersion: "5.15.0"
//        }
    }
}
