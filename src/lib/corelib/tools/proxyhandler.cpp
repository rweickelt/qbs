/****************************************************************************
**
** Copyright (C) 2020 Richard Weickelt
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

#include "proxyhandler.h"
#include <buildgraph/artifact.h>
#include <buildgraph/artifactsscriptvalue.h>
#include <language/language.h>
#include <language/propertymapinternal.h>
#include <language/scriptengine.h>
#include <tools/stlutils.h>
#include <tools/stringconstants.h>

namespace qbs {
namespace Internal {

namespace {
    const QString artifactPtrKey = QStringLiteral("_qbs_artifactPtr");
    const QString dependencyParametersKey = QStringLiteral("_qbs_dependencyParameters");
    const QString modulePtrKey = QStringLiteral("_qbs_modulePtr");
    const QString proxyKey = QStringLiteral("_qbs_proxyObject");
}

AbstractProxyHandler::~AbstractProxyHandler()
{

}

bool AbstractProxyHandler::has(const QJSValue &target, const QJSValue &key)
{
    if (!key.isString())
        return false;

    const QString name = key.toString();
    if (target.hasProperty(name))
        return true;
    if (name == proxyKey)
        return true;

    return false;
}

bool AbstractProxyHandler::set(QJSValue target, const QJSValue &key, const QJSValue &value,
                               const QJSValue &receiver)
{
    Q_UNUSED(target);
    Q_UNUSED(key);
    Q_UNUSED(value);
    Q_UNUSED(receiver);
    qjsEngine(this)->throwError(QStringLiteral("Write access is not allowed."));
    return false;
}

QJSValue AbstractProxyHandler::defaultGetOwnPropertyDescriptor(QJSValue target, const QJSValue &key)
{
    QString name = key.toString();
    QJSValue value = this->get(target, key, QJSValue());

    QJSValue result;
    if (!value.isUndefined()) {
        result = qjsEngine(this)->newObject();
        result.setProperty(QStringLiteral("configurable"), true);
        result.setProperty(QStringLiteral("enumerable"), true);
        result.setProperty(QStringLiteral("writable"), false);
        result.setProperty(QStringLiteral("value"), value);

        // The JS engine checks whether the real target has compatible attributes.
        // This looks surprising, but see https://stackoverflow.com/questions/59274975/javascript-proxy-target-object-conflicts-with-property-descriptors
        // The workaround is to create a new (empty) property on-the-fly and add it to the target object.
        target.setProperty(name, value);
    }
    return result;
}

ScriptEngine *AbstractProxyHandler::engine() const
{
    return scriptEngine(this);
}

QStringList AbstractProxyHandler::getPropertyNames(const QJSValue &object)
{
    QStringList names;
    QJSValueIterator it(object);
    while (it.hasNext()) {
        it.next();
        names << it.name();
    }
    return names;
}

QJSValue AbstractProxyHandler::reflectGet(const QJSValue &target, const QJSValue &key,
                                          const QJSValue receiver)
{
    if (m_reflect.isUndefined())
        m_reflect = engine()->globalObject().property(QStringLiteral("Reflect"));

    if (m_reflectGet.isUndefined())
        m_reflectGet = m_reflect.property(QStringLiteral("get"));

    return m_reflectGet.call(QJSValueList{target, key, receiver});
}

bool AbstractProxyHandler::isProxiedObject(const QJSValue &value)
{
    return value.hasProperty(proxyKey);
}

QJSValue DefaultProxyHandler::get(QJSValue target, const QJSValue &key,
                                  const QJSValue &receiver)
{
    return reflectGet(target, key, receiver);
}

QStringList DefaultProxyHandler::ownKeys(const QJSValue &target)
{
    QStringList keys;
    QJSValueIterator it(target);
    while (it.hasNext()) {
        it.next();
        keys << it.name();
    }
    return keys;
}

ModuleProxyHandler::ModuleProxyHandler()
{
}

QJSValue ModuleProxyHandler::get(QJSValue target, const QJSValue &key, const QJSValue &receiver)
{
    if (!key.isString())
        return QJSValue();

    const Artifact *artifact = this->artifact(target);
    const ResolvedModule *module = this->module(target);
    const auto name = key.toString();
    QJSValue result;

    if (name == StringConstants::artifactsProperty()) {
        // TODO: Would that lead to an recursion if we operate on an artifact?
        result = artifactsScriptValueForModule(engine(), module);
    } else if (name == StringConstants::dependenciesProperty()) {
        result = dependenciesValue(module);
    } else if (name == StringConstants::importsProperty()) {
        result = engine()->newArray();
    } else if (name == StringConstants::parametersProperty()) {
        result = target.property(StringConstants::parametersProperty());
    } else {
        bool propertyIsPresent = false;
        QVariant value;
        if (artifact)
            value = artifact->properties->moduleProperty(module->name, name, &propertyIsPresent);
        if (!propertyIsPresent)
            value = module->product->moduleProperties->moduleProperty(module->name, name,
                                                                      &propertyIsPresent);
        if (propertyIsPresent) {
            const Property p(module->product->uniqueName(),
                             module->name, name, value,
                             Property::PropertyInModule);
            if (artifact)
                engine()->addPropertyRequestedFromArtifact(artifact, p);
            else
                engine()->addPropertyRequestedInScript(p);
            result = engine()->toScriptValue(value);
        } else {
            return DefaultProxyHandler::get(target, key, receiver);
        }
    }

    return result;
}

QStringList ModuleProxyHandler::ownKeys(const QJSValue &target)
{
    Q_UNUSED(target);
    // TODO: Be more efficient (?)
    const ResolvedModule *module = this->module(target);
    QVariantMap moduleProperties = module->product->moduleProperties->value();
    moduleProperties = moduleProperties.value(module->name).toMap();
    QStringList keys = moduleProperties.keys();
    keys << StringConstants::artifactsProperty();
    keys << StringConstants::dependenciesProperty();
    if (target.hasOwnProperty(StringConstants::parametersProperty()))
        keys << StringConstants::parametersProperty();
    return keys;
}

const Artifact *ModuleProxyHandler::artifact(const QJSValue &target) const
{
    QVariant value = target.property(artifactPtrKey).toVariant();
    const auto *artifact = qvariant_cast<const Artifact *>(value);
    return artifact;
}

const ResolvedModule *ModuleProxyHandler::module(const QJSValue &target) const
{
    QVariant value = target.property(modulePtrKey).toVariant();
    const auto *module = qvariant_cast<const ResolvedModule *>(value);
    return module;
}

QJSValue ModuleProxyHandler::dependenciesValue(const ResolvedModule *module) const
{
    QJSValue result = engine()->newArray();
    quint32 idx = 0;
    for (const QString &depName : qAsConst(module->moduleDependencies)) {
        for (const auto &dep : module->product->modules) {
            if (dep->name != depName)
                continue;
            QJSValue target = engine()->newObject();
            target.setProperty(StringConstants::parametersProperty(), engine()->newObject());
            QJSValue obj = ModuleProxyHandler::create(engine(), target, dep.get());
            result.setProperty(idx++, obj);
            break;
        }
    }
    QBS_ASSERT(idx == quint32(module->moduleDependencies.size()),;);
    return result;
}

void ModuleProxyHandler::init(ScriptEngine *engine, QJSValue &target, const ResolvedModule *module,
                 const Artifact *artifact)
{
    target.setProperty(modulePtrKey, engine->toScriptValue(QVariant::fromValue(module)));
    if (artifact)
        target.setProperty(artifactPtrKey, engine->toScriptValue(QVariant::fromValue(artifact)));
}

QJSValue ModuleProxyHandler::create(ScriptEngine *engine, const ResolvedModule *module,
                                    const Artifact *artifact)
{
    QJSValue target = engine->newObject();
    return create(engine, target, module, artifact);
}

QJSValue ModuleProxyHandler::create(ScriptEngine *engine, QJSValue &target,
                                    const ResolvedModule *module,
                                    const Artifact *artifact)
{
    init(engine, target, module, artifact);
    return engine->newProxyObject(target, new ModuleProxyHandler());
}

} // namespace Internal
} // namespace qbs
