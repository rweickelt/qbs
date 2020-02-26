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

#include "jsextensions.h"
#include "language/scriptengine.h"
#include "tools/stringconstants.h"

#include <QtCore/qmap.h>
#include <QtCore/qglobalstatic.h>
#include <QtQml/qqmlengine.h>

#include <utility>

namespace qbs {
namespace Internal {

using InstallerTable = QMap<QString, JsExtension::InstallerFunctionPtr>;
Q_GLOBAL_STATIC(InstallerTable, jsExtensions)

void JsExtensions::setupExtensions(const QStringList &names, ScriptEngine *engine)
{
    for (const auto &name: names) {
        const auto &installer = jsExtensions->value(name, nullptr);
        QBS_ASSERT(installer, qWarning() << "Extension " << name << " requested"; continue);
        installer(engine);
    }
}

QJSValue JsExtensions::loadExtension(QJSEngine *engine, const QString &name)
{
    const auto &installer = jsExtensions->value(name, nullptr);
    QJSValue globalObject = engine->globalObject();
    QJSValue extensionNS = globalObject.property(StringConstants::extensionDefaults());
    if (extensionNS.isUndefined()) {
        extensionNS = engine->newObject();
        globalObject.setProperty(StringConstants::extensionDefaults(), extensionNS);
    }
    QJSValue extension = extensionNS.property(name);
    if (!extension.isUndefined())
        return extension;

    if (installer) {
        extension = installer(engine);
        extensionNS.setProperty(name, extension);
    }

    return extension;
}

bool JsExtensions::hasExtension(const QString &name)
{
    return jsExtensions->contains(name);
}

QStringList JsExtensions::extensionNames()
{
    return jsExtensions->keys();
}

ScriptEngine *JsExtension::engine()
{
    return (qobject_cast<ScriptEngine *>(qjsEngine(this)));
}

ScriptEngine *JsExtension::engine() const
{
    return (qobject_cast<ScriptEngine *>(qjsEngine(this)));
}

JsExtensionInstaller::JsExtensionInstaller(const QString &name,
                                           const JsExtension::InstallerFunctionPtr installer)
{
    jsExtensions->insert(name, installer);
}



} // namespace Internal
} // namespace qbs
