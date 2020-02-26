/****************************************************************************
**
** Copyright (C) 2018 The Qt Company Ltd.
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
#include "dependencyparametersscriptvalue.h"

#include <language/preparescriptobserver.h>
#include <language/qualifiedid.h>
#include <language/scriptengine.h>
#include <tools/proxyhandler.h>
#include <tools/stringconstants.h>

namespace qbs {
namespace Internal {

namespace {
    const QString internalDataProperty = QStringLiteral("_qbs_data");
}

class DependencyParameterProxyHandler: public DefaultProxyHandler
{
public:
    QJSValue get(QJSValue target, const QJSValue &key, const QJSValue &receiver) override
    {
        QJSValue value = this->DefaultProxyHandler::get(target, key, receiver);
        if (target.hasProperty(key.toString())) {
            QString name = key.toString();
            QStringList data = target.property(internalDataProperty).toVariant().toStringList();
            QBS_CHECK(!data.isEmpty());
            Property p(data[0], data[1], name, value.toVariant(), Property::PropertyInParameters);
            engine()->addPropertyRequestedInScript(p);
        }
        return value;
    }
};

// TODO: This would be a good place to investigate the possibility of
//       using the previous observer approach and whether it could be implemented more
//       efficiently than the proxy approach. At least the code would then be more similar.
static QJSValue toScriptValue(ScriptEngine *engine, const QString &productName,
                              const QVariantMap &v, const QString &depName,
                              const QualifiedId &moduleName, QJSValue handler)
{
    QJSValue obj = engine->newObject();
    bool isModuleObject = false;
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (it.value().userType() == QMetaType::QVariantMap) {
            QualifiedId id = moduleName;
            id << it.key();
            QJSValue scriptValue = toScriptValue(engine, productName, it.value().toMap(), depName,
                                                 id, handler);
            obj.setProperty(it.key(), scriptValue);
        } else {
            if (!isModuleObject) {
                isModuleObject = true;
                QJSValue data = engine->newArray(2);
                data.setProperty(0, productName);
                data.setProperty(1, depName + QLatin1Char(':') + moduleName.toString());
                obj.setProperty(internalDataProperty, data); // TODO make non-enumerable
            }
            obj.setProperty(it.key(), engine->toScriptValue(it.value()));
        }
    }
    if (isModuleObject)
        return engine->newProxyObject(obj, handler);
    else
        return obj;
}

QJSValue dependencyParametersValue(const QString &productName, const QString &dependencyName,
                                   const QVariantMap &parametersMap, ScriptEngine *engine)
{
    QJSValue handler = engine->newQObject(new DependencyParameterProxyHandler());
    return toScriptValue(engine, productName, parametersMap, dependencyName, {}, handler);
}

} // namespace Internal
} // namespace qbs
