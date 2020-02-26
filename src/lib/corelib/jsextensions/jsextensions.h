/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qbs.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#ifndef QBS_JSEXTENSIONS_H
#define QBS_JSEXTENSIONS_H

#include <type_traits>

#include <QtCore/qhash.h>
#include <QtCore/qobject.h>
#include <QtCore/qstring.h>
#include <QtCore/qstringlist.h>
#include <QtQml/QJSValue>

namespace qbs {
namespace Internal {

class ScriptEngine;

class JsExtensions
{
public:
    static void setupExtensions(const QStringList &names, ScriptEngine *engine);
    static QJSValue loadExtension(QJSEngine *engine, const QString &name);
    static bool hasExtension(const QString &name);
    static QStringList extensionNames();
};

class JsExtension: public QObject {
    Q_OBJECT

public:
    JsExtension(QObject *parent = nullptr): QObject(parent) {}

    using InstallerFunctionPtr = std::add_pointer<QJSValue(QJSEngine *)>::type;

protected:
    ScriptEngine* engine();
    ScriptEngine* engine() const;
};

class JsExtensionInstaller {
public:
    JsExtensionInstaller(const QString &name,
                         const JsExtension::InstallerFunctionPtr installerFunction);
private:
};

#define QBS_REGISTER_JS_EXTENSION(name, function) \
    namespace { \
        JsExtensionInstaller jsExtensionInstaller(QLatin1String(name), function); \
    }

} // namespace Internal
} // namespace qbs

#endif // Include guard.
