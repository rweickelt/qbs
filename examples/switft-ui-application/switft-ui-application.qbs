/****************************************************************************
**
** Copyright (C) 2026 Ivan Komissarov (abbapoh@gmail.com)
** Contact: https://www.qt.io/licensing/
**
** This file is part of the examples of Qbs.
**
** $QT_BEGIN_LICENSE:BSD$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** BSD License Usage
** Alternatively, you may use this file under the terms of the BSD license
** as follows:
**
** "Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in
**     the documentation and/or other materials provided with the
**     distribution.
**   * Neither the name of The Qt Company Ltd nor the names of its
**     contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
**
** $QT_END_LICENSE$
**
****************************************************************************/

SwiftApplication {
    Depends { name: "xcode"; required: false }
    Depends { name: "ib" }
    condition: qbs.hostOS.includes("macos") && xcode.present
               && (qbs.targetOS.includes("macos") || qbs.targetOS.includes("ios"))
    name: "SwitftUiApplication"
    install: true

    // Device builds look for an "iPhone Developer" identity. Turn signing on once one exists:
    // codesign.enableCodeSigning: true
    // codesign.signingIdentity: "Apple Development: My Team Name"
    codesign.enableCodeSigning: false

    bundle.identifierPrefix: "io.qbs"

    // Without an explicit deployment version, swiftc keeps the host triple even when
    // -sdk points at another platform.
    Group {
        condition: qbs.targetOS.includes("macos")
        product.swift.minimumMacosVersion: xcode.sdkVersion
    }
    Group {
        condition: qbs.targetOS.includes("ios")
        product.swift.minimumIosVersion: xcode.sdkVersion
        product.bundle.infoPlist: ({
            "UIApplicationSupportsIndirectInputEvents": true,
            "UILaunchScreen": ({}),
            "UISupportedInterfaceOrientations": [
                "UIInterfaceOrientationPortrait",
                "UIInterfaceOrientationLandscapeLeft",
                "UIInterfaceOrientationLandscapeRight"
            ]
        })
    }

    Group {
        prefix: "SwitftUiApplication/"
        files: [
            "MyApp.swift",
            "ContentView.swift",
            "Assets.xcassets"
        ]
    }

    Group {
        name: "Xcode Project"
        files: ["SwitftUiApplication.xcodeproj/project.xcproj"]
    }
}
