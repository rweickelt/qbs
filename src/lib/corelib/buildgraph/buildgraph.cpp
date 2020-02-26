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
#include "buildgraph.h"

#include "artifact.h"
#include "artifactsscriptvalue.h"
#include "cycledetector.h"
#include "dependencyparametersscriptvalue.h"
#include "projectbuilddata.h"
#include "productbuilddata.h"
#include "rulenode.h"
#include "transformer.h"

#include <jsextensions/jsextensions.h>
#include <jsextensions/moduleproperties.h>
#include <language/artifactproperties.h>
#include <language/language.h>
#include <language/preparescriptobserver.h>
#include <language/propertymapinternal.h>
#include <language/resolvedfilecontext.h>
#include <language/scriptengine.h>
#include <logging/categories.h>
#include <logging/logger.h>
#include <language/property.h>
#include <logging/translator.h>
#include <tools/error.h>
#include <tools/fileinfo.h>
#include <tools/proxyhandler.h>
#include <tools/scripttools.h>
#include <tools/qbsassert.h>
#include <tools/qttools.h>
#include <tools/stringconstants.h>

#include <QtCore/qdir.h>
#include <QtCore/qfile.h>

#include <algorithm>
#include <iterator>
#include <vector>

namespace qbs {
namespace Internal {

enum class DependencyType { Internal, Exported };

static QJSValue setupDependenciesScriptValue(ScriptEngine *engine,
                                             const ResolvedProduct *product,
                                             DependencyType depType);
static QJSValue setupProductScriptValue(ScriptEngine *engine, const ResolvedProduct *product);
static QJSValue setupExportsScriptValue(const ExportedModule &module, ScriptEngine *engine);
static QString childItemsProperty() { return QStringLiteral("childItems"); }
static QString exportsProperty() { return QStringLiteral("exports"); }
static QString productPtrKey() { return QStringLiteral("_qbs_productPtr"); }

class ProjectProxyHandler: public DefaultProxyHandler {
    Q_OBJECT
public:
    ProjectProxyHandler(const ResolvedProject *project)
        : m_project(project)
    {
    }

    QJSValue get(QJSValue target, const QJSValue &key,
                             const QJSValue &receiver) override
    {
        if (!key.isString())
            return QJSValue();

        const auto name = key.toString();
        QJSValue value;
        if (target.hasOwnProperty(name)) {
            value = target.property(name);
            const Property p(m_project->name, QString(), name, value.toVariant(),
                             Property::PropertyInProject);
            engine()->addPropertyRequestedInScript(p);
        } else {
            value = DefaultProxyHandler::get(target, key, receiver);
        }
        return value;
    }

private:
    const ResolvedProject *m_project;
};

class ProductProxyHandler: public DefaultProxyHandler {
    Q_OBJECT
public:
    Q_INVOKABLE QJSValue moduleProperty(const QString &moduleName,
                                        const QString &propertyName)
    {
        Q_ASSERT(m_resolvedProduct);
        const ResolvedProduct *product = m_resolvedProduct;
        QBS_CHECK(product != nullptr);
        const auto cmp = [moduleName](const ResolvedModuleConstPtr &m) {
            return m->name == moduleName;
        };
        const auto module = std::find_if(product->modules.cbegin(),
                                         product->modules.cend(),
                                         cmp);
        if (module != product->modules.cend()) {
            QJSValue moduleProxy = ModuleProxyHandler::create(engine(), module->get());
            return moduleProxy.property(propertyName);
        } else {
            return QJSValue();
        }
    }

    QJSValue get(QJSValue target, const QJSValue &key,
                             const QJSValue &receiver) override
    {
        if (!key.isString())
            return QJSValue();

        const ResolvedProduct *product = this->product(target);
        const auto name = key.toString();
        QJSValue result;
        if (name == StringConstants::artifactsProperty()) {
            result = artifactsScriptValueForProduct(engine(), product);
        } else if (name == StringConstants::dependenciesProperty()) {
            engine()->addDependenciesArrayRequested(product);
            result = setupDependenciesScriptValue(engine(), product, DependencyType::Internal);
        } else if (name == exportsProperty()) {
            engine()->addRequestedExport(product);
            result = setupExportsScriptValue(product->exportedModule, engine());
            QJSValue dependencies = setupDependenciesScriptValue(engine(), product,
                                                                 DependencyType::Exported);
            result.setProperty(StringConstants::dependenciesProperty(), dependencies);
        } else if (name == StringConstants::importsProperty()) {
            result = engine()->newArray();
        } else if (name == QStringLiteral("moduleProperty")) {
            m_resolvedProduct = product;
            result = engine()->toScriptValue(this).property(name);
        } else if (product->productProperties.contains(name)) {
            const QVariant value = product->productProperties.value(name);
            const Property p(product->uniqueName(), {}, name, value, Property::PropertyInProduct);
            engine()->addPropertyRequestedInScript(p);
            result = engine()->toScriptValue(value);
        } else {
            // If a super-module is requested, we need to walk through the whole module list and
            // assemble an object containing all submodules.
            for (const auto &m: product->modules) {
                QualifiedId id = QualifiedId::fromString(m->name);
                if (id.first() == name) {
                    QJSValue v = ModuleProxyHandler::create(engine(), m.get());
                    if (id.size() > 1) {
                        if (result.isUndefined())
                            result = engine()->newObject();
                        result.setProperty(id.last(), v);
                    } else {
                        result = v;
                        break;
                    }
                }
            }
            if (result.isUndefined())
                result = DefaultProxyHandler::get(target, key, receiver);
        }
        return result;
    }

    QStringList ownKeys(const QJSValue &target) override
    {
        Q_UNUSED(target);
        const ResolvedProduct *product = this->product(target);
        QStringList keys = product->productProperties.keys();
        keys << QStringList {
                    StringConstants::artifactsProperty(),
                    StringConstants::dependenciesProperty(),
                    exportsProperty()
                };
        if (target.hasOwnProperty(StringConstants::parametersProperty()))
            keys << StringConstants::parametersProperty();
        return keys;
    }

    static QJSValue create(ScriptEngine *engine, QJSValue &target,
                           const ResolvedProduct *product)
    {
        target.setProperty(productPtrKey(),
                           engine->toScriptValue(QVariant::fromValue(product)));
        return engine->newProxyObject(target, new ProductProxyHandler());
    }

private:
    ProductProxyHandler() = default;

    const ResolvedProduct *product(const QJSValue &target) const
    {
        QVariant value = target.property(productPtrKey()).toVariant();
        const auto *product = qvariant_cast<const ResolvedProduct *>(value);
        Q_ASSERT(product);
        return product;
    }

    const ResolvedProduct *m_resolvedProduct = nullptr;
};

static QJSValue setupProjectScriptValue(ScriptEngine *engine,
        const ResolvedProjectConstPtr &project)
{
    QJSValue &proxy = engine->projectScriptValue(project.get());
    if (!proxy.isUndefined())
        return proxy;

    QJSValue obj = engine->newObject();
    const QVariantMap &properties = project->projectProperties();
    for (auto it = properties.cbegin(); it != properties.cend(); ++it)
        obj.setProperty(it.key(), engine->toScriptValue(it.value()));

    obj.setProperty(StringConstants::filePathProperty(), project->location.filePath());
    obj.setProperty(StringConstants::pathProperty(), FileInfo::path(project->location.filePath()));
    proxy = engine->newProxyObject(obj, new ProjectProxyHandler(project.get()));
    return proxy;
}

static QJSValue setupDependenciesScriptValue(ScriptEngine *engine,
                                             const ResolvedProduct *product, DependencyType depType)
{
    QJSValue result = engine->newArray();
    quint32 idx = 0;
    const bool exportCase = depType == DependencyType::Exported;
    std::vector<ResolvedProductPtr> productDeps;
    if (exportCase) {
        if (!product->exportedModule.productDependencies.empty()) {
            const auto allProducts = product->topLevelProject()->allProducts();
            const auto getProductForName = [&allProducts](const QString &name) {
                const auto cmp = [name](const ResolvedProductConstPtr &p) {
                    return p->uniqueName() == name;
                };
                const auto it = std::find_if(allProducts.cbegin(), allProducts.cend(), cmp);
                QBS_ASSERT(it != allProducts.cend(), return ResolvedProductPtr());
                return *it;
            };
            std::transform(product->exportedModule.productDependencies.cbegin(),
                           product->exportedModule.productDependencies.cend(),
                           std::back_inserter(productDeps), getProductForName);
        }
    } else {
        productDeps = product->dependencies;
    }
    for (const ResolvedProductPtr &dependency : qAsConst(productDeps)) {
        const QVariantMap &params
                = (exportCase ? product->exportedModule.dependencyParameters.value(dependency)
                              : product->dependencyParameters.value(dependency));
        QJSValue obj = engine->newObject();
        QJSValue parametersValue = dependencyParametersValue(product->name,
                                                             dependency->name,
                                                             params,
                                                             engine);
        obj.setProperty(StringConstants::parametersProperty(), parametersValue);
        QJSValue productScriptValue = ProductProxyHandler::create(engine, obj, dependency.get());
        result.setProperty(idx++, productScriptValue);
    }
    if (exportCase) {
        for (const ExportedModuleDependency &m : product->exportedModule.moduleDependencies) {
            QJSValue obj = engine->newObject();
            obj.setProperty(StringConstants::nameProperty(), m.name);
            QJSValue exportsValue = engine->newObject();
            obj.setProperty(exportsProperty(), exportsValue);
            exportsValue.setProperty(StringConstants::dependenciesProperty(),
                                     engine->newArray());
            for (auto modIt = m.moduleProperties.begin(); modIt != m.moduleProperties.end();
                 ++modIt) {
                const QVariantMap entries = modIt.value().toMap();
                if (entries.empty())
                    continue;
                QJSValue moduleObj = engine->newObject();
                ModuleProperties::setModuleScriptValue(exportsValue, moduleObj, modIt.key(), engine);
                for (auto valIt = entries.begin(); valIt != entries.end(); ++valIt)
                    moduleObj.setProperty(valIt.key(), engine->toScriptValue(valIt.value()));
            }
            result.setProperty(idx++, obj);
        }
        return result;
    }
    for (const auto &dependency : product->modules) {
        if (dependency->isProduct)
            continue;

        const QVariantMap &params = product->moduleParameters.value(dependency);
        QJSValue obj = engine->newObject();
        QJSValue parametersValue = dependencyParametersValue(product->name,
                                                             dependency->name,
                                                             params,
                                                             engine);
        obj.setProperty(StringConstants::parametersProperty(), parametersValue);
        QJSValue moduleScriptValue = ModuleProxyHandler::create(engine, obj, dependency.get());
        result.setProperty(idx++, moduleScriptValue);
    }
    return result;
}

static QJSValue setupExportedPropertyScriptValue(const ExportedProperty &property,
                                                 ScriptEngine *engine)
{
    QJSValue propertyScriptValue = engine->newObject();
    propertyScriptValue.setProperty(StringConstants::nameProperty(), property.fullName);
    propertyScriptValue.setProperty(StringConstants::typeProperty(),
                                    PropertyDeclaration::typeString(property.type));
    propertyScriptValue.setProperty(StringConstants::sourceCodeProperty(), property.sourceCode);
    propertyScriptValue.setProperty(QStringLiteral("isBuiltin"), property.isBuiltin);
    return propertyScriptValue;
}

static void setupExportedPropertiesScriptValue(QJSValue &parentObject,
                                               const std::vector<ExportedProperty> &properties,
                                               ScriptEngine *engine)
{
    QJSValue propertiesScriptValue = engine->newArray(properties.size());
    parentObject.setProperty(QStringLiteral("properties"), propertiesScriptValue);
    quint32 arrayIndex = 0;
    for (const ExportedProperty &p : properties) {
        propertiesScriptValue.setProperty(arrayIndex++,
                                          setupExportedPropertyScriptValue(p, engine));
    }
}

static QJSValue setupExportedItemScriptValue(const ExportedItem *item, ScriptEngine *engine)
{
    QJSValue itemScriptValue = engine->newObject();
    itemScriptValue.setProperty(StringConstants::nameProperty(), item->name);
    setupExportedPropertiesScriptValue(itemScriptValue, item->properties, engine);
    QJSValue childrenScriptValue = engine->newArray(item->children.size());
    itemScriptValue.setProperty(childItemsProperty(), childrenScriptValue);
    quint32 arrayIndex = 0;
    for (const auto &childItem : item->children) {
        childrenScriptValue.setProperty(arrayIndex++,
                                        setupExportedItemScriptValue(childItem.get(), engine));
    }
    return itemScriptValue;
}

static QJSValue setupExportsScriptValue(const ExportedModule &module, ScriptEngine *engine)
{
    QJSValue exportsScriptValue = engine->newObject();
    for (auto it = module.propertyValues.cbegin(); it != module.propertyValues.cend(); ++it)
        exportsScriptValue.setProperty(it.key(), engine->toScriptValue(it.value()));
    setupExportedPropertiesScriptValue(exportsScriptValue, module.m_properties, engine);
    QJSValue childrenScriptValue = engine->newArray(module.children.size());
    exportsScriptValue.setProperty(childItemsProperty(), childrenScriptValue);
    quint32 arrayIndex = 0;
    for (const auto &exportedItem : module.children) {
        childrenScriptValue.setProperty(arrayIndex++,
                                        setupExportedItemScriptValue(exportedItem.get(), engine));
    }
    QJSValue importsScriptValue = engine->newArray(module.importStatements.size());
    exportsScriptValue.setProperty(StringConstants::importsProperty(), importsScriptValue);
    arrayIndex = 0;
    for (const QString &importStatement : module.importStatements)
        importsScriptValue.setProperty(arrayIndex++, importStatement);
    for (auto it = module.modulePropertyValues.cbegin(); it != module.modulePropertyValues.cend();
         ++it) {
        const QVariantMap entries = it.value().toMap();
        if (entries.empty())
            continue;
        QJSValue moduleObject = engine->newObject();
        ModuleProperties::setModuleScriptValue(exportsScriptValue, moduleObject, it.key(), engine);
        for (auto valIt = entries.begin(); valIt != entries.end(); ++valIt)
            moduleObject.setProperty(valIt.key(), engine->toScriptValue(valIt.value()));
    }
    return exportsScriptValue;
}

static QJSValue setupProductScriptValue(ScriptEngine *engine, const ResolvedProduct *product)
{
    QJSValue &productScriptValue = engine->productScriptValue(product);
    if (!productScriptValue.isUndefined())
        return productScriptValue;

    QJSValue obj = engine->newObject();
    productScriptValue = ProductProxyHandler::create(engine, obj, product);

    return productScriptValue;
}

void setupScriptEngineForFile(ScriptEngine *engine, const FileContextBaseConstPtr &fileContext,
        QJSValue &targetObject, const ObserveMode &observeMode)
{
    engine->import(fileContext, targetObject, observeMode);
}

void setupScriptEngineForProduct(ScriptEngine *engine, ResolvedProduct *product,
                                 const ResolvedModule *module, QJSValue &targetObject,
                                 bool setBuildEnvironment)
{
    QJSValue projectScriptValue = setupProjectScriptValue(engine, product->project.lock());
    targetObject.setProperty(StringConstants::projectVar(), projectScriptValue);

    if (setBuildEnvironment) {
        QVariant v;
        v.setValue<void*>(&product->buildEnvironment);
        engine->setProperty(StringConstants::qbsProcEnvVarInternal(), v);
    }
    QJSValue productScriptValue = engine->newObject();

    // If the Rule is in a Module, set up the 'moduleName' property. Required by
    // ModUtils.
    if (!module->name.isEmpty())
        productScriptValue.setProperty(StringConstants::moduleNameProperty(), module->name);

    productScriptValue.setPrototype(setupProductScriptValue(engine, product));

    targetObject.setProperty(StringConstants::productVar(), productScriptValue);
}

bool findPath(BuildGraphNode *u, BuildGraphNode *v, QList<BuildGraphNode *> &path)
{
    if (u == v) {
        path.push_back(v);
        return true;
    }

    for (BuildGraphNode * const childNode : qAsConst(u->children)) {
        if (findPath(childNode, v, path)) {
            path.prepend(u);
            return true;
        }
    }

    return false;
}

/*
 * Creates the build graph edge p -> c, which represents the dependency "c must be built before p".
 */
void connect(BuildGraphNode *p, BuildGraphNode *c)
{
    QBS_CHECK(p != c);
    qCDebug(lcBuildGraph).noquote() << "connect" << p->toString() << "->" << c->toString();
    if (c->type() == BuildGraphNode::ArtifactNodeType) {
        auto const ac = static_cast<Artifact *>(c);
        for (const Artifact *child : filterByType<Artifact>(p->children)) {
            if (child == ac)
                return;
            const bool filePathsMustBeDifferent = child->artifactType == Artifact::Generated
                    || child->product == ac->product || child->artifactType != ac->artifactType;
            if (filePathsMustBeDifferent && child->filePath() == ac->filePath()) {
                throw ErrorInfo(QStringLiteral("%1 already has a child artifact %2 as "
                                                    "different object.").arg(p->toString(),
                                                                             ac->filePath()),
                                CodeLocation(), true);
            }
        }
    }
    p->children.insert(c);
    c->parents.insert(p);
    p->product->topLevelProject()->buildData->setDirty();
}

static bool existsPath_impl(BuildGraphNode *u, BuildGraphNode *v, NodeSet *seen)
{
    if (u == v)
        return true;

    if (!seen->insert(u).second)
        return false;

    for (BuildGraphNode * const childNode : qAsConst(u->children)) {
        if (existsPath_impl(childNode, v, seen))
            return true;
    }

    return false;
}

static bool existsPath(BuildGraphNode *u, BuildGraphNode *v)
{
    NodeSet seen;
    return existsPath_impl(u, v, &seen);
}

static QStringList toStringList(const QList<BuildGraphNode *> &path)
{
    QStringList lst;
    for (BuildGraphNode *node : path)
        lst << node->toString();
    return lst;
}

bool safeConnect(Artifact *u, Artifact *v)
{
    QBS_CHECK(u != v);
    qCDebug(lcBuildGraph) << "safeConnect:" << relativeArtifactFileName(u)
                          << "->" << relativeArtifactFileName(v);

    if (existsPath(v, u)) {
        QList<BuildGraphNode *> circle;
        findPath(v, u, circle);
        qCDebug(lcBuildGraph) << "safeConnect: circle detected " << toStringList(circle);
        return false;
    }

    connect(u, v);
    return true;
}

void disconnect(BuildGraphNode *u, BuildGraphNode *v)
{
    qCDebug(lcBuildGraph).noquote() << "disconnect:" << u->toString() << v->toString();
    u->children.remove(v);
    v->parents.remove(u);
    u->onChildDisconnected(v);
}

void removeGeneratedArtifactFromDisk(Artifact *artifact, const Logger &logger)
{
    if (artifact->artifactType != Artifact::Generated)
        return;
    removeGeneratedArtifactFromDisk(artifact->filePath(), logger);
}

void removeGeneratedArtifactFromDisk(const QString &filePath, const Logger &logger)
{
    QFile file(filePath);
    if (!file.exists())
        return;
    logger.qbsDebug() << "removing " << filePath;
    if (!file.remove())
        logger.qbsWarning() << QStringLiteral("Cannot remove '%1'.").arg(filePath);
}

QString relativeArtifactFileName(const Artifact *artifact)
{
    const QString &buildDir = artifact->product->topLevelProject()->buildDirectory;
    QString str = artifact->filePath();
    if (str.startsWith(buildDir))
        str.remove(0, buildDir.size());
    if (str.startsWith(QLatin1Char('/')))
        str.remove(0, 1);
    return str;
}

Artifact *lookupArtifact(const ResolvedProductConstPtr &product,
        const ProjectBuildData *projectBuildData, const QString &dirPath, const QString &fileName,
        bool compareByName)
{
    for (const auto &fileResource : projectBuildData->lookupFiles(dirPath, fileName)) {
        if (fileResource->fileType() != FileResourceBase::FileTypeArtifact)
            continue;
        const auto artifact = static_cast<Artifact *>(fileResource);
        if (compareByName
                ? artifact->product->uniqueName() == product->uniqueName()
                : artifact->product == product) {
            return artifact;
        }
    }
    return nullptr;
}

Artifact *lookupArtifact(const ResolvedProductConstPtr &product, const QString &dirPath,
                         const QString &fileName, bool compareByName)
{
    return lookupArtifact(product, product->topLevelProject()->buildData.get(), dirPath, fileName,
                          compareByName);
}

Artifact *lookupArtifact(const ResolvedProductConstPtr &product, const QString &filePath,
                         bool compareByName)
{
    QString dirPath, fileName;
    FileInfo::splitIntoDirectoryAndFileName(filePath, &dirPath, &fileName);
    return lookupArtifact(product, dirPath, fileName, compareByName);
}

Artifact *lookupArtifact(const ResolvedProductConstPtr &product, const ProjectBuildData *buildData,
                         const QString &filePath, bool compareByName)
{
    QString dirPath, fileName;
    FileInfo::splitIntoDirectoryAndFileName(filePath, &dirPath, &fileName);
    return lookupArtifact(product, buildData, dirPath, fileName, compareByName);
}

Artifact *lookupArtifact(const ResolvedProductConstPtr &product, const Artifact *artifact,
                         bool compareByName)
{
    return lookupArtifact(product, artifact->dirPath(), artifact->fileName(), compareByName);
}

Artifact *createArtifact(const ResolvedProductPtr &product,
                         const SourceArtifactConstPtr &sourceArtifact)
{
    const auto artifact = new Artifact;
    artifact->artifactType = Artifact::SourceFile;
    setArtifactData(artifact, sourceArtifact);
    insertArtifact(product, artifact);
    return artifact;
}

void setArtifactData(Artifact *artifact, const SourceArtifactConstPtr &sourceArtifact)
{
    artifact->targetOfModule = sourceArtifact->targetOfModule;
    artifact->setFilePath(sourceArtifact->absoluteFilePath);
    artifact->setFileTags(sourceArtifact->fileTags);
    artifact->properties = sourceArtifact->properties;
}

void updateArtifactFromSourceArtifact(const ResolvedProductPtr &product,
                                      const SourceArtifactConstPtr &sourceArtifact)
{
    Artifact * const artifact = lookupArtifact(product, sourceArtifact->absoluteFilePath, false);
    QBS_CHECK(artifact);
    const FileTags oldFileTags = artifact->fileTags();
    const QVariantMap oldModuleProperties = artifact->properties->value();
    setArtifactData(artifact, sourceArtifact);
    if (oldFileTags != artifact->fileTags()
            || oldModuleProperties != artifact->properties->value()) {
        invalidateArtifactAsRuleInputIfNecessary(artifact);
    }
}

void insertArtifact(const ResolvedProductPtr &product, Artifact *artifact)
{
    qCDebug(lcBuildGraph) << "insert artifact" << artifact->filePath();
    QBS_CHECK(!artifact->product);
    QBS_CHECK(!artifact->filePath().isEmpty());
    artifact->product = product;
    product->topLevelProject()->buildData->insertIntoLookupTable(artifact);
    product->buildData->addArtifact(artifact);
}

void provideFullFileTagsAndProperties(Artifact *artifact)
{
    artifact->properties = artifact->product->moduleProperties;
    FileTags allTags = artifact->pureFileTags.empty()
            ? artifact->product->fileTagsForFileName(artifact->fileName()) : artifact->pureFileTags;
    for (const auto &props : artifact->product->artifactProperties) {
        if (allTags.intersects(props->fileTagsFilter())) {
            artifact->properties = props->propertyMap();
            allTags += props->extraFileTags();
            break;
        }
    }
    artifact->setFileTags(allTags);

    // Let a positive value of qbs.install imply the file tag "installable".
    if (artifact->properties->qbsPropertyValue(StringConstants::installProperty()).toBool())
        artifact->addFileTag("installable");
}

void applyPerArtifactProperties(Artifact *artifact)
{
    if (artifact->pureProperties.empty())
        return;
    QVariantMap props = artifact->properties->value();
    for (const auto &property : artifact->pureProperties)
        setConfigProperty(props, property.first, property.second);
    artifact->properties = artifact->properties->clone();
    artifact->properties->setValue(props);
}

void updateGeneratedArtifacts(ResolvedProduct *product)
{
    if (!product->buildData)
        return;
    for (Artifact * const artifact : filterByType<Artifact>(product->buildData->allNodes())) {
        if (artifact->artifactType == Artifact::Generated) {
            const FileTags oldFileTags = artifact->fileTags();
            const QVariantMap oldModuleProperties = artifact->properties->value();
            provideFullFileTagsAndProperties(artifact);
            applyPerArtifactProperties(artifact);
            if (oldFileTags != artifact->fileTags()
                    || oldModuleProperties != artifact->properties->value()) {
                invalidateArtifactAsRuleInputIfNecessary(artifact);
            }
        }
    }
}

// This is needed for artifacts which are inputs to rules whose outputArtifacts script
// returned an empty array for this input. Since there is no transformer, our usual change
// tracking procedure will not notice if the artifact's file tags or module properties have
// changed, so we need to force a re-run of the outputArtifacts script.
void invalidateArtifactAsRuleInputIfNecessary(Artifact *artifact)
{
    for (RuleNode * const parentRuleNode : filterByType<RuleNode>(artifact->parents)) {
        if (!parentRuleNode->rule()->isDynamic())
            continue;
        bool artifactNeedsExplicitInvalidation = true;
        for (Artifact * const output : filterByType<Artifact>(parentRuleNode->parents)) {
            if (output->children.contains(artifact)
                    && !output->childrenAddedByScanner.contains(artifact)) {
                artifactNeedsExplicitInvalidation = false;
                break;
            }
        }
        if (artifactNeedsExplicitInvalidation)
            parentRuleNode->removeOldInputArtifact(artifact);
    }
}

static void doSanityChecksForProduct(const ResolvedProductConstPtr &product,
        const Set<ResolvedProductPtr> &allProducts, const Logger &logger)
{
    qCDebug(lcBuildGraph) << "Sanity checking product" << product->uniqueName();
    CycleDetector cycleDetector(logger);
    cycleDetector.visitProduct(product);
    const ProductBuildData * const buildData = product->buildData.get();
    for (const auto &m : product->modules)
        QBS_CHECK(m->product == product.get());
    qCDebug(lcBuildGraph) << "enabled:" << product->enabled << "build data:" << buildData;
    if (product->enabled)
        QBS_CHECK(buildData);
    if (!product->buildData)
        return;
    for (BuildGraphNode * const node : qAsConst(buildData->rootNodes())) {
        qCDebug(lcBuildGraph).noquote() << "Checking root node" << node->toString();
        QBS_CHECK(buildData->allNodes().contains(node));
    }
    Set<QString> filePaths;
    for (BuildGraphNode * const node : qAsConst(buildData->allNodes())) {
        qCDebug(lcBuildGraph).noquote() << "Sanity checking node" << node->toString();
        QBS_CHECK(node->product == product);
        for (const BuildGraphNode * const parent : qAsConst(node->parents))
            QBS_CHECK(parent->children.contains(node));
        for (BuildGraphNode * const child : qAsConst(node->children)) {
            QBS_CHECK(child->parents.contains(node));
            QBS_CHECK(!child->product.expired());
            QBS_CHECK(child->product->buildData);
            QBS_CHECK(child->product->buildData->allNodes().contains(child));
            QBS_CHECK(allProducts.contains(child->product.lock()));
        }

        Artifact * const artifact = node->type() == BuildGraphNode::ArtifactNodeType
                ? static_cast<Artifact *>(node) : nullptr;
        if (!artifact) {
            QBS_CHECK(node->type() == BuildGraphNode::RuleNodeType);
            auto const ruleNode = static_cast<RuleNode *>(node);
            QBS_CHECK(ruleNode->rule());
            QBS_CHECK(ruleNode->rule()->product);
            QBS_CHECK(ruleNode->rule()->product == ruleNode->product.get());
            QBS_CHECK(ruleNode->rule()->product == product.get());
            QBS_CHECK(contains(product->rules, std::const_pointer_cast<Rule>(ruleNode->rule())));
            continue;
        }

        QBS_CHECK(product->topLevelProject()->buildData->fileDependencies.contains(
                      artifact->fileDependencies));
        QBS_CHECK(artifact->artifactType == Artifact::SourceFile ||
                  !filePaths.contains(artifact->filePath()));
        filePaths << artifact->filePath();

        for (Artifact * const child : qAsConst(artifact->childrenAddedByScanner))
            QBS_CHECK(artifact->children.contains(child));
        const TransformerConstPtr transformer = artifact->transformer;
        if (artifact->artifactType == Artifact::SourceFile)
            continue;

        const auto parentRuleNodes = filterByType<RuleNode>(artifact->children);
        QBS_CHECK(std::distance(parentRuleNodes.begin(), parentRuleNodes.end()) == 1);

        QBS_CHECK(transformer);
        QBS_CHECK(transformer->rule);
        QBS_CHECK(transformer->rule->product);
        QBS_CHECK(transformer->rule->product == artifact->product.get());
        QBS_CHECK(transformer->rule->product == product.get());
        QBS_CHECK(transformer->outputs.contains(artifact));
        QBS_CHECK(contains(product->rules, std::const_pointer_cast<Rule>(transformer->rule)));
        qCDebug(lcBuildGraph)
                << "The transformer has" << transformer->outputs.size() << "outputs.";
        ArtifactSet transformerOutputChildren;
        for (const Artifact * const output : qAsConst(transformer->outputs)) {
            QBS_CHECK(output->transformer == transformer);
            transformerOutputChildren.unite(ArtifactSet::filtered(output->children));
            for (const Artifact *a : filterByType<Artifact>(output->children)) {
                for (const Artifact *other : filterByType<Artifact>(output->children)) {
                    if (other != a && other->filePath() == a->filePath()
                            && (other->artifactType != Artifact::SourceFile
                                || a->artifactType != Artifact::SourceFile
                                || other->product == a->product)) {
                        throw ErrorInfo(QStringLiteral("There is more than one artifact for "
                                "file '%1' in the child list for output '%2'.")
                                .arg(a->filePath(), output->filePath()), CodeLocation(), true);
                    }
                }
            }
        }
        if (lcBuildGraph().isDebugEnabled()) {
            qCDebug(lcBuildGraph) << "The transformer output children are:";
            for (const Artifact * const a : qAsConst(transformerOutputChildren))
                qCDebug(lcBuildGraph) << "\t" << a->fileName();
            qCDebug(lcBuildGraph) << "The transformer inputs are:";
            for (const Artifact * const a : qAsConst(transformer->inputs))
                qCDebug(lcBuildGraph) << "\t" << a->fileName();
        }
        QBS_CHECK(transformer->inputs.size() <= transformerOutputChildren.size());
        for (Artifact * const transformerInput : qAsConst(transformer->inputs))
            QBS_CHECK(transformerOutputChildren.contains(transformerInput));
        transformer->artifactsMapRequestedInPrepareScript.doSanityChecks();
        transformer->artifactsMapRequestedInCommands.doSanityChecks();
    }
}

static void doSanityChecks(const ResolvedProjectPtr &project,
                           const Set<ResolvedProductPtr> &allProducts, Set<QString> &productNames,
                           const Logger &logger)
{
    logger.qbsDebug() << "Sanity checking project '" << project->name << "'";
    for (const ResolvedProjectPtr &subProject : qAsConst(project->subProjects))
        doSanityChecks(subProject, allProducts, productNames, logger);

    for (const auto &product : project->products) {
        QBS_CHECK(product->project == project);
        QBS_CHECK(product->topLevelProject() == project->topLevelProject());
        doSanityChecksForProduct(product, allProducts, logger);
        QBS_CHECK(!productNames.contains(product->uniqueName()));
        productNames << product->uniqueName();
    }
}

void doSanityChecks(const ResolvedProjectPtr &project, const Logger &logger)
{
    if (qEnvironmentVariableIsEmpty("QBS_SANITY_CHECKS"))
        return;
    Set<QString> productNames;
    const Set<ResolvedProductPtr> allProducts
            = Set<ResolvedProductPtr>::fromStdVector(project->allProducts());
    doSanityChecks(project, allProducts, productNames, logger);
}

} // namespace Internal
} // namespace qbs

#include "buildgraph.moc"
