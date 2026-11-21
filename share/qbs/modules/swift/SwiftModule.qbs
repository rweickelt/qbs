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

import qbs.FileInfo
import qbs.ModUtils
import qbs.Probes
import "swift.js" as Swift

Module {
    Depends {
        name: "xcode"
        required: false
        condition: qbs.targetOS.includes("darwin")
                   && qbs.toolchain && qbs.toolchain.includes("xcode")
    }

    Probes.SwiftProbe {
        id: swiftProbe
        toolchainPath: _toolchainPath
    }

    // Probes run only after the module condition has been accepted, so this cannot
    // depend on swiftProbe.found. Absence of swiftc is reported by validate.
    condition: qbs.targetOS.includes("darwin")

    property path _toolchainPath: {
        if (qbs.targetOS.includes("darwin") && qbs.toolchain && qbs.toolchain.includes("xcode")
                && xcode.present)
            return xcode.toolchainPath;
        return undefined;
    }

    version: compilerVersion
    property string compilerVersion: swiftProbe.compilerVersion
    property int compilerVersionMajor: swiftProbe.compilerVersionMajor
    property int compilerVersionMinor: swiftProbe.compilerVersionMinor
    property int compilerVersionPatch: swiftProbe.compilerVersionPatch
    property string targetTriple: swiftProbe.targetTriple

    property path toolchainInstallPath: swiftProbe.path
    property string compilerName: "swiftc"
    property string compilerPath: swiftProbe.filePath
    property string sdkPath: {
        if (qbs.targetOS.includes("darwin") && qbs.toolchain && qbs.toolchain.includes("xcode")
                && xcode.present)
            return xcode.sdkPath;
        return undefined;
    }
    // Used when a minimum*Version property is left unset, so -target matches -sdk.
    property string _sdkVersion: {
        if (qbs.targetOS.includes("darwin") && qbs.toolchain && qbs.toolchain.includes("xcode")
                && xcode.present)
            return xcode.sdkVersion;
        return undefined;
    }

    property string moduleName: product.targetName
    PropertyOptions {
        name: "moduleName"
        description: "Swift module name. Must be a Swift identifier. Defaults to the product target name."
    }

    property string swiftLanguageVersion
    PropertyOptions {
        name: "swiftLanguageVersion"
        description: "Value passed to -swift-version. Leave unset to use the compiler default."
    }

    property string optimization: qbs.optimization
    PropertyOptions {
        name: "optimization"
        allowedValues: ["none", "fast", "small"]
        description: "Maps onto -Onone, -O, and -Osize."
    }

    property bool debugInformation: qbs.debugInformation
    property string warningLevel: "all"
    PropertyOptions {
        name: "warningLevel"
        allowedValues: ["none", "all"]
    }
    property bool treatWarningsAsErrors: false

    property stringList defines
    property stringList flags
    PropertyOptions {
        name: "flags"
        description: "Extra arguments passed to swiftc as-is."
    }
    property stringList clangFlags
    PropertyOptions {
        name: "clangFlags"
        description: "Arguments forwarded to clang, each prefixed with -Xcc."
    }
    property stringList linkerFlags
    PropertyOptions {
        name: "linkerFlags"
        description: "Arguments forwarded to the linker, each prefixed with -Xlinker."
    }

    property pathList importPaths
    property path bridgingHeader
    property bool emitObjcHeader: false
    property bool enableLibraryEvolution: false

    property string minimumMacosVersion: (qbs.targetOS.includes("macos") || qbs.targetOS.includes("osx"))
            ? _sdkVersion : undefined
    property string minimumIosVersion: qbs.targetOS.includes("ios") ? _sdkVersion : undefined
    property string minimumTvosVersion: qbs.targetOS.includes("tvos") ? _sdkVersion : undefined
    property string minimumWatchosVersion: qbs.targetOS.includes("watchos") ? _sdkVersion : undefined

    property string executablePrefix: ""
    property string executableSuffix: qbs.targetOS.includes("windows") ? ".exe" : ""
    property string dynamicLibraryPrefix: qbs.targetOS.includes("windows") ? "" : "lib"
    property string dynamicLibrarySuffix: {
        if (qbs.targetOS.includes("windows"))
            return ".dll";
        if (qbs.targetOS.includes("darwin"))
            return ".dylib";
        return ".so";
    }
    property string staticLibraryPrefix: qbs.targetOS.includes("windows") ? "" : "lib"
    property string staticLibrarySuffix: qbs.targetOS.includes("windows") ? ".lib" : ".a"
    property string objectSuffix: qbs.targetOS.includes("windows") ? ".obj" : ".o"

    FileTagger {
        patterns: ["*.swift"]
        fileTags: ["swift"]
    }

    validate: {
        var validator = new ModUtils.PropertyValidator("swift");
        validator.setRequiredProperty("compilerPath", compilerPath);
        validator.setRequiredProperty("moduleName", moduleName);
        validator.addCustomValidator("moduleName", moduleName, function(value) {
            return /^[A-Za-z_][A-Za-z0-9_]*$/.test(value);
        }, "'" + moduleName + "' must be a Swift identifier");
        validator.addCustomValidator("optimization", optimization, function(value) {
            return !value || ["none", "fast", "small"].includes(value);
        }, "must be 'none', 'fast', or 'small'");
        validator.validate();
    }

    Rule {
        name: "swift.compiler"
        condition: Swift.shouldLink(product)
        multiplex: true
        inputs: ["swift"]
        inputsFromDependencies: ["swiftmodule"]
        outputFileTags: ["swift.obj", "swiftmodule", "swiftdoc", "swiftsourceinfo", "swift.abi",
                         "swift.deps", "swift.output-file-map", "swift.objc-header"]
        outputArtifacts: Swift.compileOutputArtifacts(product, inputs)
        prepare: Swift.prepareCompile.apply(Swift, arguments)
    }

    Rule {
        name: "swift.linker.application"
        condition: Swift.shouldLink(product) && product.type && product.type.includes("application")
        multiplex: true
        inputs: ["swift.obj"]
        inputsFromDependencies: ["staticlibrary", "dynamiclibrary"]
        outputFileTags: ["application", "bundle.input"]
        outputArtifacts: Swift.linkOutputArtifacts(product)
        prepare: Swift.prepareLink.apply(Swift, arguments)
    }

    Rule {
        name: "swift.linker.dynamic"
        condition: Swift.shouldLink(product)
                   && product.type && product.type.includes("dynamiclibrary")
        multiplex: true
        inputs: ["swift.obj"]
        inputsFromDependencies: ["staticlibrary", "dynamiclibrary"]
        outputFileTags: ["dynamiclibrary", "bundle.input"]
        outputArtifacts: Swift.linkOutputArtifacts(product)
        prepare: Swift.prepareLink.apply(Swift, arguments)
    }

    Rule {
        name: "swift.linker.static"
        condition: Swift.shouldLink(product)
                   && product.type && product.type.includes("staticlibrary")
        multiplex: true
        inputs: ["swift.obj"]
        outputFileTags: ["staticlibrary", "bundle.input"]
        outputArtifacts: Swift.linkOutputArtifacts(product)
        prepare: Swift.prepareLink.apply(Swift, arguments)
    }
}
