Product {
    property string os
    property string osx_image
    property stringList tags

    property stringList environment: []
    property pathList stashDirectories
    property bool sudo: false
    property string stage: project.name

    type: ["step_output", "export_output" ]

    Properties {
        condition: tags
        type: outer.concat(tags)
    }

    Depends { name: "ci" }
    ci.stashPaths: stashDirectories
}
