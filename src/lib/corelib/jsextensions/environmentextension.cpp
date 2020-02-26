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
#include <language/scriptengine.h>

#include <logging/translator.h>
#include <tools/hostosinfo.h>
#include <tools/stringconstants.h>

#include <QtCore/qdir.h>
#include <QtCore/qobject.h>
#include <QtCore/qprocess.h>

#include <QtQml/qjsvalue.h>

namespace qbs {
namespace Internal {

class EnvironmentExtension : public JsExtension
{
    Q_OBJECT
public:
    Q_INVOKABLE QJSValue getEnv(const QString &name);
    Q_INVOKABLE void putEnv(const QString &name, const QString &value);
    Q_INVOKABLE void unsetEnv(const QString &name);
    Q_INVOKABLE QJSValue currentEnv() const;

private:
};

static QProcessEnvironment *getProcessEnvironment(QJSEngine *engine, const QString &func,
                                                  bool doThrow = true)
{
    QVariant v = engine->property(StringConstants::qbsProcEnvVarInternal());
    auto procenv = reinterpret_cast<QProcessEnvironment *>(v.value<void *>());
    if (!procenv && doThrow) {
        engine->throwError(QString(QStringLiteral("%1 can only be called from "
                                                  "Module.setupBuildEnvironment and "
                                                  "Module.setupRunEnvironment")).arg(func));
    }

    return procenv;
}

QJSValue EnvironmentExtension::getEnv(const QString &name)
{
    Q_ASSERT(engine());
    const QProcessEnvironment env = engine()->environment();
    const QProcessEnvironment *procenv = getProcessEnvironment(engine(), QStringLiteral("getEnv"),
                                                               false);
    if (!procenv)
        procenv = &env;

    const QString value = procenv->value(name);
    return !value.isNull() ? value : QJSValue();
}

void EnvironmentExtension::putEnv(const QString &name, const QString &value)
{
    getProcessEnvironment(engine(), QStringLiteral("putEnv"))->insert(name, value);
}

void EnvironmentExtension::unsetEnv(const QString &name)
{
    getProcessEnvironment(engine(), QStringLiteral("unsetEnv"))->remove(name);
}

QJSValue EnvironmentExtension::currentEnv() const
{
    Q_ASSERT(engine());
    const QProcessEnvironment env = engine()->environment();
    const QProcessEnvironment *procenv = getProcessEnvironment(engine(), QStringLiteral("getEnv"),
                                                               false);
    if (!procenv)
        procenv = &env;

    QJSValue envObject = engine()->newObject();
    const auto keys = procenv->keys();
    for (const QString &key : keys) {
        const QString keyName = HostOsInfo::isWindowsHost() ? key.toUpper() : key;
        envObject.setProperty(keyName, QJSValue(procenv->value(key)));
    }
    return envObject;
}


QJSValue createEnvironmentExtension(QJSEngine *engine)
{
    return engine->newQObject(new EnvironmentExtension());
}

QBS_REGISTER_JS_EXTENSION("Environment", createEnvironmentExtension)

}
}

#include "environmentextension.moc"
