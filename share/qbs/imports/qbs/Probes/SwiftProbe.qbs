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
import qbs.Host
import qbs.Process
import "path-probe.js" as PathProbeConfigure

BinaryProbe {
    id: swiftcProbe
    names: ["swiftc"]

    // Inputs
    property path toolchainPath

    // Outputs
    property string compilerVersion
    property int compilerVersionMajor
    property int compilerVersionMinor
    property int compilerVersionPatch
    property string targetTriple

    platformSearchPaths: {
        var paths = [];
        if (toolchainPath)
            paths.push(FileInfo.joinPaths(toolchainPath, "usr", "bin"));
        if (Host.os().contains("unix"))
            paths = paths.concat(["/usr/bin", "/usr/local/bin"]);
        return paths;
    }

    configure: {
        if (!condition)
            return;

        var results = PathProbeConfigure.configure(
                    selectors, names, nameSuffixes, nameFilter, candidateFilter, searchPaths,
                    pathSuffixes, platformSearchPaths, environmentPaths, platformEnvironmentPaths);
        found = results.found;
        allResults = results.files;
        if (!found || !results.files || !results.files.length) {
            found = false;
            return;
        }

        var result = results.files[0];
        candidatePaths = result.candidatePaths;
        path = result.path;
        filePath = result.filePath;
        fileName = result.fileName;
        if (!filePath) {
            found = false;
            return;
        }

        var proc = new Process();
        try {
            if (proc.exec(filePath, ["--version"], false) !== 0) {
                found = false;
                return;
            }
            var text = "";
            try {
                text += proc.readStdOut();
            } catch (e) {
            }
            try {
                text += proc.readStdErr();
            } catch (e) {
            }
            var versionMatch = /Swift version (\d+)\.(\d+)(?:\.(\d+))?/.exec(text);
            if (versionMatch) {
                compilerVersionMajor = parseInt(versionMatch[1], 10);
                compilerVersionMinor = parseInt(versionMatch[2], 10);
                compilerVersionPatch = versionMatch[3] ? parseInt(versionMatch[3], 10) : 0;
                compilerVersion = [compilerVersionMajor, compilerVersionMinor,
                                   compilerVersionPatch].join(".");
            }
            var targetMatch = /Target:\s*(\S+)/.exec(text);
            if (targetMatch)
                targetTriple = targetMatch[1];
        } finally {
            proc.close();
        }
    }
}
