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
#include "artifactsscriptvalue.h"

#include "artifact.h"
#include "productbuilddata.h"
#include "transformer.h"
#include <language/language.h>
#include <language/scriptengine.h>
#include <tools/fileinfo.h>
#include <tools/proxyhandler.h>
#include <tools/stringconstants.h>

namespace qbs {
namespace Internal {

enum BuildGraphScriptValueCommonPropertyKeys : quint32 {
    CachedValueKey,
    FileTagKey,
    ProductPtrKey,
};

namespace {
    const QString internalArtifactPropertyName = QStringLiteral("_qbs_artifact");
}

class ArtifactsMapProxyHandler: public DefaultProxyHandler
{
public :
    ArtifactsMapProxyHandler(const ResolvedProduct *product):
        m_product(product) {}
    // Only needed to compile artifactsMapScriptValue
    ArtifactsMapProxyHandler(const ResolvedModule *) {
        QBS_CHECK(false);
    }

    QJSValue get(QJSValue target, const QJSValue &key,
             const QJSValue &receiver) override
    {
        Q_UNUSED(receiver);
        if (!key.isString())
            return QJSValue();
        const QString name = key.toString();
        QJSValue value;
        if (target.hasOwnProperty(name)) {
            engine()->setArtifactSetRequestedForTag(m_product, name);
            value = target.property(name);
        } else {
            engine()->setNonExistingArtifactSetRequested(m_product, name);
        }
        return value;
    }

    QStringList ownKeys(const QJSValue &target) override
    {
        engine()->setArtifactsEnumerated(m_product);
        return DefaultProxyHandler::ownKeys(target);
    }

private:
    const ResolvedProduct *m_product = nullptr;
};

class ModuleInArtifactProxyHandler: public DefaultProxyHandler
{
public :
    ModuleInArtifactProxyHandler(const Artifact *artifact, const QString &moduleName):
        m_artifact(artifact), m_moduleName(moduleName) {}

    QJSValue get(QJSValue target, const QJSValue &key,
             const QJSValue &receiver) override
    {
        Q_UNUSED(receiver);
        if (!key.isString())
            return QJSValue();

        QString name = key.toString();
        QJSValue value = target.property(name);
        const Property p(m_artifact->product->uniqueName(),
                         m_moduleName, name, value.toVariant(),
                         Property::PropertyInModule);
        engine()->addPropertyRequestedFromArtifact(m_artifact, p);
        return value;
    }

    QStringList ownKeys(const QJSValue &target) override
    {
        return DefaultProxyHandler::ownKeys(target) + QStringList {
            QStringLiteral("artifacts"),
            QStringLiteral("dependencies")
        };
    }

private:
    const Artifact *m_artifact = nullptr;
    QString m_moduleName;
};

class ArtifactProxyHandler: public AbstractProxyHandler {
    Q_OBJECT
public:
    ArtifactProxyHandler(const Artifact *artifact)
        : m_artifact(artifact),
          m_properties(artifact->properties->value())
    {
    }

    Q_INVOKABLE QJSValue moduleProperty(const QString &moduleName,
                                        const QString &propertyName = QString())
    {
        const auto cmp = [moduleName](const ResolvedModuleConstPtr &m) {
            return m->name == moduleName;
        };
        const std::vector<ResolvedModulePtr> &modules = m_artifact->product->modules;
        const auto module = std::find_if(modules.cbegin(), modules.cend(), cmp);
        if (module != modules.cend()) {
            QJSValue m = ModuleProxyHandler::create(engine(), module->get(), m_artifact);
            if (propertyName.isEmpty())
                return m;
            else
                return m.property(propertyName);

        } else {
            return QJSValue();
        }
    }

    QJSValue get(QJSValue target, const QJSValue &key,
                             const QJSValue &receiver) override
    {
        Q_UNUSED(receiver);
        const auto name = key.toString();

        if (target.hasProperty(name))
            return target.property(name);

        if (name == QStringLiteral("moduleProperty"))
            return engine()->toScriptValue(this).property(name);

        if (name == StringConstants::fileNameProperty()) {
            return m_artifact->fileName();
        } else if (name == StringConstants::filePathProperty()) {
            return m_artifact->filePath();
        } else if (name == StringConstants::baseNameProperty()) {
            return {FileInfo::baseName(m_artifact->filePath())};
        } else if (name == StringConstants::completeBaseNameProperty()) {
            return {FileInfo::completeBaseName(m_artifact->filePath())};
        } else if (name == QStringLiteral("baseDir")) {
            QString basedir;
            if (m_artifact->artifactType == Artifact::SourceFile) {
                QDir sourceDir(m_artifact->product->sourceDirectory);
                basedir = FileInfo::path(sourceDir.relativeFilePath(m_artifact->filePath()));
            } else {
                QDir buildDir(m_artifact->product->buildDirectory());
                basedir = FileInfo::path(buildDir.relativeFilePath(m_artifact->filePath()));
            }
            return basedir;
        } else if (name == StringConstants::fileTagsProperty()) {
            const QStringList fileTags = sorted(m_artifact->fileTags().toStringList());
            const Property p(m_artifact->product->uniqueName(), QString(), name, QVariant(fileTags),
                           Property::PropertyInArtifact);
            engine()->addPropertyRequestedFromArtifact(m_artifact, p);
            return engine()->toScriptValue(fileTags);

        } else if (name == QStringLiteral("children")) {
            uint size = 0;
            QJSValueList values;
            for (const Artifact *child : m_artifact->childArtifacts()) {
                values << Transformer::translateFileConfig(engine(), child, QString());
                ++size;
            }
            QJSValue sv = engine()->newArray(size);
            for (uint idx = 0; idx < size; ++idx)
                sv.setProperty(idx, values[idx]);
            return sv;
        } else if (name == StringConstants::productValue()) {
            const auto product = m_artifact->product;
            const QVariantMap productProperties {
                {StringConstants::buildDirectoryProperty(), product->buildDirectory()},
                {StringConstants::destinationDirProperty(), product->destinationDirectory},
                {StringConstants::nameProperty(), product->name},
                {StringConstants::sourceDirectoryProperty(), product->sourceDirectory},
                {StringConstants::targetNameProperty(), product->targetName},
                {StringConstants::typeProperty(), sorted(product->fileTags.toStringList())}
            };
            return engine()->toScriptValue(productProperties);
        } else if (name == QStringLiteral("__internalPtr")) { // TODO Remove?
            return engine()->toScriptValue(QVariant::fromValue<uintptr_t>(reinterpret_cast<uintptr_t>(m_artifact)));
        } else {
            QJSValue result = moduleScriptValue(name);
            return result;
        }

        return QJSValue();
    }

    Q_INVOKABLE QStringList ownKeys(const QJSValue target)
    {
        Q_UNUSED(target);
        QStringList keys = m_artifact->properties->value().keys();
        keys << QStringList {
                    StringConstants::productVar(),
                    StringConstants::fileNameProperty(),
                    StringConstants::filePathProperty(),
                    StringConstants::baseNameProperty(),
                    StringConstants::completeBaseNameProperty(),
                    QStringLiteral("baseDir"),
                    QStringLiteral("children"),
                    StringConstants::fileTagsProperty(),
                    StringConstants::moduleNameProperty(),
                };
        return keys;
    }

    Q_INVOKABLE virtual QJSValue getOwnPropertyDescriptor(QJSValue target,
                                                          const QJSValue &key)
    {
        return defaultGetOwnPropertyDescriptor(target, key);
    }

private:
    QJSValue moduleScriptValue(const QString &name)
    {
        const ResolvedProduct *product = m_artifact->product.get();
        if (QualifiedId::fromString(name).length() > 1) {
            // 'name' must be a submodule, find it in the module list
            for (const auto& module: product->modules) {
                if (module->name == name) {
                    QJSValue target = engine()->newObject();
                    return ModuleProxyHandler::create(engine(), target, module.get(), m_artifact);
                }
            }
        } else {
            // 'name' is either a stand-alone or a umbrella module
            std::vector<ResolvedModulePtr> candidates;
            bool isStandalone = false;
            for (const auto &module: product->modules) {
                const QualifiedId id = QualifiedId::fromString(module->name);
                if (id.first() == name) {
                    candidates << module;
                    if (id.length() == 1) {
                        isStandalone = true;
                        break;
                    }
                }
            }
            if (candidates.empty())
                return QJSValue();

            if (isStandalone) {
                QJSValue target = engine()->newObject();
                return ModuleProxyHandler::create(engine(), target, candidates[0].get(),
                                                  m_artifact);
            }

            QJSValue topModule = engine()->newObject();
            for (const auto &module: candidates) {
                QJSValue target = engine()->newObject();
                QJSValue value = ModuleProxyHandler::create(engine(), target,
                                                            module.get(), m_artifact);
                QualifiedId id = QualifiedId::fromString(module->name);
                topModule.setProperty(id.last(), value);
            }
            return topModule;
        }
        return QJSValue();
    }

    const Artifact *m_artifact;
    const QVariantMap m_properties;
};

static bool isRelevantArtifact(const ResolvedProduct *, const Artifact *artifact)
{
    return !artifact->isTargetOfModule();
}
static bool isRelevantArtifact(const ResolvedModule *module, const Artifact *artifact)
{
    return artifact->targetOfModule == module->name;
}

static ArtifactSetByFileTag artifactsMap(const ResolvedProduct *product)
{
    return product->buildData->artifactsByFileTag();
}

static ArtifactSetByFileTag artifactsMap(const ResolvedModule *module)
{
    return artifactsMap(module->product);
}

static bool checkAndSetArtifactsMapUpToDateFlag(const ResolvedProduct *p)
{
    return p->buildData->checkAndSetJsArtifactsMapUpToDateFlag();
}
static bool checkAndSetArtifactsMapUpToDateFlag(const ResolvedModule *) { return true; }

static void registerArtifactsMapAccess(const ResolvedProduct *p, ScriptEngine *e, bool forceUpdate)
{
    e->setArtifactsMapRequested(p, forceUpdate);
}
static void registerArtifactsMapAccess(const ResolvedModule *, ScriptEngine *, bool) {}

template<class ProductOrModule> static QJSValue artifactsMapScriptValue(
        ScriptEngine *engine, const ProductOrModule *productOrModule)
{
    QJSValue &artifactsMapObj = engine->artifactsScriptValue(productOrModule);
    if (artifactsMapObj.isObject() && checkAndSetArtifactsMapUpToDateFlag(productOrModule)) {
        registerArtifactsMapAccess(productOrModule, engine, false);
        return artifactsMapObj;
    }
    registerArtifactsMapAccess(productOrModule, engine, true);

    const auto &map = artifactsMap(productOrModule);
    artifactsMapObj = engine->newObject();
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        const FileTag &fileTag = it.key();
        auto artifacts = it.value();
        const auto filter = [productOrModule](const Artifact *a) {
            return !isRelevantArtifact(productOrModule, a);
        };
        artifacts.erase(std::remove_if(artifacts.begin(), artifacts.end(), filter), artifacts.end());
        if (artifacts.empty())
            continue;

        QJSValue array = engine->newArray(artifacts.size());
        int i = 0;
        for (const Artifact *artifact: artifacts) {
            array.setProperty(i, scriptValueForArtifact(engine, artifact));
            ++i;
        }
        artifactsMapObj.setProperty(fileTag.toString(), array);
    }

    // Only artifacts in products are observed
    if (std::is_same<ProductOrModule, ResolvedProduct>::value) {
        auto handler = new ArtifactsMapProxyHandler(productOrModule);
        return engine->newProxyObject(artifactsMapObj, handler);
    } else {
        return artifactsMapObj;
    }
}

QJSValue artifactsScriptValueForProduct(ScriptEngine *engine, const ResolvedProduct *product)
{
    return artifactsMapScriptValue(engine, product);
}

QJSValue artifactsScriptValueForModule(ScriptEngine *engine, const ResolvedModule *module)
{
    return artifactsMapScriptValue(engine, module);
}

QJSValue scriptValueForArtifact(ScriptEngine *engine, const Artifact *artifact,
                                const QString &defaultModuleName)
{
    QJSValue &scriptValue = engine->artifactScriptValue(artifact);
//    if (scriptValue.isUndefined()) {
        QJSValue object = engine->newObject();
        if (!defaultModuleName.isEmpty())
            object.setProperty(StringConstants::moduleNameProperty(), defaultModuleName);
        object.setProperty(internalArtifactPropertyName, engine->toScriptValue(artifact));
        auto handler = new ArtifactProxyHandler(artifact);
        scriptValue = engine->newProxyObject(object, handler);
//    }
    return scriptValue;
}

const Artifact *artifactForScriptValue(ScriptEngine *engine, const QJSValue &value)
{
    QJSValue artifactValue = value.property(internalArtifactPropertyName);
    return engine->fromScriptValue<const Artifact *>(artifactValue);
}

} // namespace Internal
} // namespace qbs

#include "artifactsscriptvalue.moc"
