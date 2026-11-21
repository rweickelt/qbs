/****************************************************************************
**
** Copyright (C) 2026 Ivan Komissarov (abbapoh@gmail.com)
** Contact: http://www.qt.io/licensing
**
** This file is part of Qbs.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms and
** conditions see http://www.qt.io/terms-conditions. For further information
** use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file.  Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, The Qt Company gives you certain additional
** rights.  These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

var FileInfo = require("qbs.FileInfo");
var TextFile = require("qbs.TextFile");
var Utilities = require("qbs.Utilities");

function shouldLink(product) {
    return !(product.multiplexed || product.aggregate) || product.multiplexConfigurationId;
}

function isWholeModule(product) {
    return product.qbs.buildVariant !== "debug";
}

function isApplication(product) {
    return product.type && product.type.includes("application");
}

function isDynamicLibrary(product) {
    return product.type && product.type.includes("dynamiclibrary");
}

function isStaticLibrary(product) {
    return product.type && product.type.includes("staticlibrary");
}

function moduleBase(product) {
    return FileInfo.joinPaths(product.buildDirectory, product.swift.moduleName);
}

function sourceObjects(product, inputs) {
    var sources = (inputs.swift || []).slice().sort(function(a, b) {
        return a.filePath.localeCompare(b.filePath);
    });
    if (isWholeModule(product)) {
        return sources.length
                ? [{ source: undefined, objectPath: moduleBase(product) + product.swift.objectSuffix }]
                : [];
    }
    return sources.map(function(source) {
        var stem = FileInfo.joinPaths(product.buildDirectory, "swift-objects",
                                      Utilities.getHash(source.filePath));
        return { source: source, objectPath: stem + product.swift.objectSuffix, stem: stem };
    });
}

function artifact(filePath, tags) {
    return { filePath: filePath, fileTags: tags };
}

function compileOutputArtifacts(product, inputs) {
    var base = moduleBase(product);
    var artifacts = [
        artifact(base + ".swiftmodule", ["swiftmodule"]),
        artifact(base + ".swiftdoc", ["swiftdoc"]),
        artifact(base + ".swiftsourceinfo", ["swiftsourceinfo"]),
        artifact(base + ".abi.json", ["swift.abi"])
    ];
    if (product.swift.emitObjcHeader) {
        artifacts.push(artifact(base + "-Swift.h", ["swift.objc-header"]));
    }
    var objects = sourceObjects(product, inputs);
    if (isWholeModule(product)) {
        if (objects.length)
            artifacts.push(artifact(objects[0].objectPath, ["swift.obj"]));
        artifacts.push(artifact(base + ".d", ["swift.deps"]));
    } else {
        artifacts.push(artifact(base + "-output-file-map.json", ["swift.output-file-map"]));
        artifacts.push(artifact(FileInfo.joinPaths(product.buildDirectory,
                                                   product.swift.moduleName + ".priors"),
                                ["swift.deps"]));
        artifacts.push(artifact(FileInfo.joinPaths(product.buildDirectory,
                                                   product.swift.moduleName + ".emit-module.d"),
                                ["swift.deps"]));
        objects.forEach(function(obj) {
            artifacts.push(artifact(obj.objectPath, ["swift.obj"]));
            artifacts.push(artifact(obj.stem + ".d", ["swift.deps"]));
            artifacts.push(artifact(obj.stem + ".swiftdeps", ["swift.deps"]));
        });
    }
    return artifacts;
}

function outputFileMap(product, inputs) {
    var base = moduleBase(product);
    var map = {};
    map[""] = {
        "swiftmodule": base + ".swiftmodule",
        "swiftdoc": base + ".swiftdoc",
        "swiftsourceinfo": base + ".swiftsourceinfo",
        "swift-dependencies": base + ".swiftdeps"
    };
    sourceObjects(product, inputs).forEach(function(obj) {
        map[obj.source.filePath] = {
            "object": obj.objectPath,
            "dependencies": obj.stem + ".d",
            "swift-dependencies": obj.stem + ".swiftdeps"
        };
    });
    return JSON.stringify(map, undefined, 2);
}

function appleTarget(product) {
    var os = product.qbs.targetOS || [];
    var simulator = os.includes("ios-simulator") || os.includes("tvos-simulator")
            || os.includes("watchos-simulator");
    var name;
    var version;
    if (os.includes("watchos")) {
        name = "watchos";
        version = product.swift.minimumWatchosVersion;
    } else if (os.includes("tvos")) {
        name = "tvos";
        version = product.swift.minimumTvosVersion;
    } else if (os.includes("ios")) {
        name = "ios";
        version = product.swift.minimumIosVersion;
    } else if (os.includes("macos") || os.includes("osx")) {
        name = "macosx";
        version = product.swift.minimumMacosVersion;
        simulator = false;
    }
    if (!name || !version)
        return undefined;
    var triple = product.qbs.architecture + "-apple-" + name + version;
    if (simulator)
        triple += "-simulator";
    return triple;
}

function appendWrapped(args, flag, values) {
    (values || []).forEach(function(value) {
        args.push(flag, value);
    });
}

function commonDriverArgs(product) {
    var args = [];
    var swift = product.swift;
    if (swift.sdkPath)
        args.push("-sdk", swift.sdkPath);
    var triple = appleTarget(product);
    if (triple)
        args.push("-target", triple);
    (swift.flags || []).forEach(function(flag) { args.push(flag); });
    return args;
}

function compileArgs(product, inputs, outputs) {
    var swift = product.swift;
    var args = ["-c", "-module-name", swift.moduleName, "-emit-module",
                "-emit-module-path", moduleBase(product) + ".swiftmodule",
                "-emit-dependencies"];
    if (isWholeModule(product)) {
        args.push("-wmo");
        var objects = sourceObjects(product, inputs);
        if (objects.length)
            args.push("-o", objects[0].objectPath);
    } else {
        args.push("-incremental");
        args.push("-output-file-map", outputs["swift.output-file-map"][0].filePath);
    }
    if (!isApplication(product))
        args.push("-parse-as-library");
    if (isStaticLibrary(product))
        args.push("-static");
    if (swift.enableLibraryEvolution)
        args.push("-enable-library-evolution");
    if (swift.swiftLanguageVersion)
        args.push("-swift-version", swift.swiftLanguageVersion);
    if (swift.bridgingHeader)
        args.push("-import-objc-header", swift.bridgingHeader);
    if (swift.emitObjcHeader)
        args.push("-emit-objc-header-path", moduleBase(product) + "-Swift.h");
    var opt = swift.optimization;
    if (opt === "none")
        args.push("-Onone");
    else if (opt === "small")
        args.push("-Osize");
    else if (opt === "fast")
        args.push("-O");
    if (swift.debugInformation)
        args.push("-g");
    if (swift.warningLevel === "none")
        args.push("-suppress-warnings");
    if (swift.treatWarningsAsErrors)
        args.push("-warnings-as-errors");
    (swift.defines || []).forEach(function(define) { args.push("-D" + define); });
    var seen = {};
    function addImportPath(dir) {
        if (dir && !seen[dir]) {
            seen[dir] = true;
            args.push("-I", dir);
        }
    }
    ((inputs && inputs.swiftmodule) || []).forEach(function(mod) {
        addImportPath(FileInfo.path(mod.filePath));
    });
    (swift.importPaths || []).forEach(addImportPath);
    args = args.concat(commonDriverArgs(product));
    appendWrapped(args, "-Xcc", swift.clangFlags);
    (inputs.swift || []).slice().sort(function(a, b) {
        return a.filePath.localeCompare(b.filePath);
    }).forEach(function(source) { args.push(source.filePath); });
    return args;
}

function prepareCompile(project, product, inputs, outputs) {
    var commands = [];
    if (!isWholeModule(product)) {
        var writeMap = new JavaScriptCommand();
        writeMap.description = "generating output file map for " + product.swift.moduleName;
        writeMap.highlight = "codegen";
        writeMap.mapFilePath = outputs["swift.output-file-map"][0].filePath;
        writeMap.mapContents = outputFileMap(product, inputs);
        writeMap.sourceCode = function() {
            var file = new TextFile(mapFilePath, TextFile.WriteOnly);
            try {
                file.write(mapContents);
            } finally {
                file.close();
            }
        };
        commands.push(writeMap);
    }
    var cmd = new Command(product.swift.compilerPath, compileArgs(product, inputs, outputs));
    cmd.description = "compiling Swift module " + product.swift.moduleName;
    cmd.highlight = "compiler";
    cmd.workingDirectory = product.buildDirectory;
    commands.push(cmd);
    return commands;
}

function linkedFileName(product) {
    var swift = product.swift;
    if (isDynamicLibrary(product))
        return swift.dynamicLibraryPrefix + product.targetName + swift.dynamicLibrarySuffix;
    if (isStaticLibrary(product))
        return swift.staticLibraryPrefix + product.targetName + swift.staticLibrarySuffix;
    return swift.executablePrefix + product.targetName + swift.executableSuffix;
}

function linkOutputArtifacts(product) {
    var tags = [];
    if (isApplication(product))
        tags.push("application");
    else if (isDynamicLibrary(product))
        tags.push("dynamiclibrary");
    else if (isStaticLibrary(product))
        tags.push("staticlibrary");
    var fileName = linkedFileName(product);
    var outputPath = FileInfo.joinPaths(product.destinationDirectory, fileName);
    var bundleFilePath = outputPath;
    var bundleLoaded = product.moduleProperty("bundle", "isBundle") !== undefined;
    if (bundleLoaded && product.qbs.targetOS.includes("darwin")) {
        tags.push("bundle.input");
        if (product.moduleProperty("bundle", "isBundle")) {
            var exePath = product.moduleProperty("bundle", "executablePath");
            if (exePath) {
                outputPath = FileInfo.joinPaths(product.destinationDirectory, ".tmp",
                                                FileInfo.fileName(exePath));
                bundleFilePath = FileInfo.joinPaths(product.destinationDirectory, exePath);
            }
        }
    }
    var result = { filePath: outputPath, fileTags: tags };
    if (tags.includes("bundle.input"))
        result.bundle = { _bundleFilePath: bundleFilePath };
    return [result];
}

function linkArgs(product, inputs, outputs) {
    var args = [];
    if (isApplication(product))
        args.push("-emit-executable");
    else if (isStaticLibrary(product))
        args.push("-emit-library", "-static");
    else
        args.push("-emit-library");
    args.push("-module-name", product.swift.moduleName);
    var output = (outputs.application || outputs.dynamiclibrary || outputs.staticlibrary)[0];
    args.push("-o", output.filePath);
    args = args.concat(commonDriverArgs(product));
    if (isDynamicLibrary(product) && product.qbs.targetOS.includes("darwin")) {
        args.push("-Xlinker", "-install_name", "-Xlinker",
                  "@rpath/" + linkedFileName(product));
    }
    (inputs["swift.obj"] || []).forEach(function(obj) { args.push(obj.filePath); });
    if (!isStaticLibrary(product)) {
        var rpaths = {};
        function addRpath(dir) {
            if (dir && !rpaths[dir]) {
                rpaths[dir] = true;
                args.push("-Xlinker", "-rpath", "-Xlinker", dir);
            }
        }
        var dynamicLibs = inputs.dynamiclibrary || [];
        if (isApplication(product) && dynamicLibs.length) {
            dynamicLibs.forEach(function(lib) { addRpath(FileInfo.path(lib.filePath)); });
            if (product.qbs.targetOS.includes("darwin"))
                addRpath("@loader_path");
            else if (product.qbs.targetOS.includes("linux"))
                addRpath("$ORIGIN");
        }
        (inputs.staticlibrary || []).forEach(function(lib) { args.push(lib.filePath); });
        dynamicLibs.forEach(function(lib) { args.push(lib.filePath); });
    }
    appendWrapped(args, "-Xlinker", product.swift.linkerFlags);
    return args;
}

function prepareLink(project, product, inputs, outputs) {
    var cmd = new Command(product.swift.compilerPath, linkArgs(product, inputs, outputs));
    cmd.description = "linking " + product.name;
    cmd.highlight = "linker";
    cmd.workingDirectory = product.buildDirectory;
    return [cmd];
}
