import "ci-utils.js" as CiUtils

Rule {
    property string name
    property pathList paths: []

    alwaysRun: true
    inputs: "step_output"
    multiplex: true

    outputFileTags: {
        Array.from = CiUtils.from;
        // The config array must include all custom
        // properties to become a unique string.
        var config = {
            name: name,
            paths: paths,
            productName: product.name,
        };
        var config_string = JSON.stringify(config);

        var tags = [
            "export_output",
            config_string
        ];

        return tags;
    }

    prepare: {
        var rule = CiUtils.ruleObjectFromFiletags(outputs);
        var cmds = [];
        for (var i = 0; i < rule.paths.length; i++) {
            var cmd = new JavaScriptCommand();
            cmd.description = "exporting " + rule.paths[i];
            cmd.highlight = "codegen";
            cmd.sourceCode = function() {
                var rule = CiUtils.ruleObjectFromFiletags(outputs);
                // Do something here?
            }
            cmds.push(cmd);
        }
        return cmds;
    }


}
