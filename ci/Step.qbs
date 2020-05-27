import "ci-utils.js" as CiUtils

Rule {
    property stringList arguments: [ "bla" ]
    property string command
    property stringList dependsOn: []
    property stringList environment: product.environment
    property string name: command
    property bool sudo: product.sudo

    property stringList _arguments: arguments

    alwaysRun: true
    inputs: dependsOn.concat(["import_output"])
    inputsFromDependencies: ["step_output", "export_output"]
    multiplex: true
    requiresInputs: false

    // We abuse an output file tag to forward
    // Rule properties to the prepare script. There is
    // no other way to do so.
    // The rule's name is also used as an output file tag
    // if set. This allows subsequent rules to use the rule
    // name as an input.
    outputFileTags: {
        Array.from = CiUtils.from;
        // The config array must include all custom
        // properties to become a unique string.
        var config = {
            command: command,
            arguments: Array.from(_arguments),
            environment: Array.from(environment),
            name: name,
            sudo: sudo,
            productName: product.name,
        };
        var config_string = JSON.stringify(config);

        var tags = [
            "step_output",
            config_string
        ];
        if (command !== name)
            tags.push(name);

        return tags;
    }

    prepare: {
        // Extract the rule properties from the
        // output file tag.
        var rule = CiUtils.ruleObjectFromFiletags(outputs);
        var cmd = new Command();
        cmd.program = rule.command;
        cmd.arguments = rule.arguments;
        cmd.description = rule.name;
        cmd.environment = rule.environment;
        cmd.highlight = "compiler"
        cmd.stdoutFilePath = "/dev/null"

        return [cmd];
    }


}
