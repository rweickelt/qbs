/****************************************************************************
**
** Copyright (C) 2020 Richard Weickelt
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

#ifndef QBS_QBSSCRIPTENGINE_H
#define QBS_QBSSCRIPTENGINE_H

#include "forward_decls.h"
#include "property.h"
#include <buildgraph/requestedartifacts.h>
#include <buildgraph/requesteddependencies.h>
#include <language/filetags.h>
#include <logging/logger.h>
#include <tools/codelocation.h>
#include <tools/filetime.h>
#include <tools/set.h>

#include <QtCore/qdir.h>
#include <QtCore/qhash.h>
#include <QtCore/qlist.h>
#include <QtCore/qprocess.h>
#include <QtCore/qstring.h>

#include <QtQml/qqmlengine.h>

#include <memory>
#include <mutex>
#include <stack>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace qbs {
namespace Internal {

class Artifact;
class AbstractProxyHandler;
class ScriptEngine;
struct VTablePatcher;

enum class EvalContext {
    PropertyEvaluation, ProbeExecution, ModuleProvider, RuleExecution, JsCommand
};
class DubiousContext
{
public:
    enum Suggestion { NoSuggestion, SuggestMoving };
    DubiousContext(EvalContext c, Suggestion s = NoSuggestion) : context(c), suggestion(s) { }
    EvalContext context;
    Suggestion suggestion;
};
using DubiousContextList = std::vector<DubiousContext>;


/*
 * ScriptObject that acquires resources, for example a file handle.
 * Such objects are usually created in JS scripts and are automatically
 * destroyed by the garbage collector. But the gc does sometimes not
 * destroy objects before EOL of the engine, thus requiring a way of
 * manually releasing resources when needed.
 */
class ResourceAcquiringScriptObject
{
public:
    inline ResourceAcquiringScriptObject(ScriptEngine *engine): m_engine(engine) {}
    virtual ~ResourceAcquiringScriptObject();
    virtual void releaseResources() = 0;
    inline void setReleased() { m_released = true; }

protected:
    inline bool isReleased() const { return m_released; }

private:
    ScriptEngine *m_engine = nullptr;
    bool m_released = false;
};

enum class ObserveMode { Enabled, Disabled };

class QBS_AUTOTEST_EXPORT ScriptEngine : public QJSEngine
{
    Q_OBJECT
    ScriptEngine(Logger &logger, EvalContext evalContext, QObject *parent = nullptr);
public:
    class CancelException {};
    static ScriptEngine *create(Logger &logger, EvalContext evalContext, QObject *parent = nullptr);
    ~ScriptEngine() override;

    Logger &logger() const { return m_logger; }
    void import(const FileContextBaseConstPtr &fileCtx, QJSValue &targetObject,
                ObserveMode observeMode);

    void setEvalContext(EvalContext c) { m_evalContext = c; }
    EvalContext evalContext() const { return m_evalContext; }
    void checkContext(const QString &operation, const DubiousContextList &dubiousContexts);

    void addPropertyRequestedInScript(const Property &property) {
        m_propertiesRequestedInScript += property;
    }
    void addDependenciesArrayRequested(const ResolvedProduct *p)
    {
        m_productsWithRequestedDependencies.insert(p);
    }
    void setArtifactsMapRequested(const ResolvedProduct *product, bool forceUpdate)
    {
        m_requestedArtifacts.setAllArtifactTags(product, forceUpdate);
    }
    void setArtifactSetRequestedForTag(const ResolvedProduct *product, const QString &tag)
    {
        m_requestedArtifacts.setArtifactsForTag(product, FileTag::fromSetting(tag));
    }
    void setNonExistingArtifactSetRequested(const ResolvedProduct *product, const QString &tag)
    {
        m_requestedArtifacts.setNonExistingTagRequested(product, tag);
    }
    void setArtifactsEnumerated(const ResolvedProduct *product)
    {
        m_requestedArtifacts.setArtifactsEnumerated(product);
    }
    void addPropertyRequestedFromArtifact(const Artifact *artifact, const Property &property);
    void addRequestedExport(const ResolvedProduct *product) { m_requestedExports.insert(product); }
    void clearRequestedProperties() {
        m_propertiesRequestedInScript.clear();
        m_propertiesRequestedFromArtifact.clear();
        m_importedFilesUsedInScript.clear();
        m_productsWithRequestedDependencies.clear();
        m_requestedArtifacts.clear();
        m_requestedExports.clear();
    }
    PropertySet propertiesRequestedInScript() const { return m_propertiesRequestedInScript; }
    QHash<QString, PropertySet> propertiesRequestedFromArtifact() const {
        return m_propertiesRequestedFromArtifact;
    }
    Set<const ResolvedProduct *> productsWithRequestedDependencies() const
    {
        return m_productsWithRequestedDependencies;
    }
    RequestedDependencies requestedDependencies() const
    {
        return RequestedDependencies(m_productsWithRequestedDependencies);
    }
    RequestedArtifacts requestedArtifacts() const { return m_requestedArtifacts; }
    Set<const ResolvedProduct *> requestedExports() const { return m_requestedExports; }

    void addImportedFileUsedInScript(const QString &filePath);
    std::vector<QString> importedFilesUsedInScript() const;

    void setUsesIo() { m_usesIo = true; }
    void clearUsesIo() { m_usesIo = false; }
    bool usesIo() const { return m_usesIo; }

    void enableProfiling(bool enabled);

    QProcessEnvironment environment() const { return m_environment; }
    void setEnvironment(const QProcessEnvironment &env) { m_environment = env; }
    void addCanonicalFilePathResult(const QString &filePath, const QString &resultFilePath);
    void addFileExistsResult(const QString &filePath, bool exists);
    void addDirectoryEntriesResult(const QString &path, QDir::Filters filters,
                                   const QStringList &entries);
    void addFileLastModifiedResult(const QString &filePath, const FileTime &fileTime);
    QHash<QString, QString> canonicalFilePathResults() const { return m_canonicalFilePathResult; }
    QHash<QString, bool> fileExistsResults() const { return m_fileExistsResult; }
    QHash<std::pair<QString, quint32>, QStringList> directoryEntriesResults() const
    {
        return m_directoryEntriesResult;
    }

    QHash<QString, FileTime> fileLastModifiedResults() const { return m_fileLastModifiedResult; }
    Set<QString> imports() const;
    static QJSValueList argumentList(const QStringList &argumentNames, const QJSValue &context);

    void addResourceAcquiringScriptObject(ResourceAcquiringScriptObject *obj);
    void removeResourceAcquiringScriptObject(ResourceAcquiringScriptObject *obj);
    void releaseResourcesOfScriptObjects();

    QJSValue &artifactScriptValue(const Artifact *artifact)
    {
        return m_artifactScriptValues[artifact];
    }

    QJSValue &artifactsScriptValue(const void *productOrModule)
    {
        return m_artifactsScriptValues[productOrModule];
    }

    QJSValue &productScriptValue(const ResolvedProduct *product)
    {
        return m_productScriptValues[product];
    }

    QJSValue &projectScriptValue(const ResolvedProject *project)
    {
        return m_projectScriptValues[project];
    }

    QJSValue &moduleScriptValuePrototype(const ResolvedModule *module)
    {
        return m_moduleScriptValues[module];
    }

    void unobserveProperties();

    QJSValue globalObject() const { return m_globalObject; }
    void setGlobalObject(const QJSValue &object) { m_globalObject = object; }

    QJSValue newProxyObject(QObject *handler);
    QJSValue newProxyObject(const QJSValue &target, QObject *handler);
    QJSValue newProxyObject(const QJSValue &target, const QJSValue &handler);
    void defineProperty(QJSValue &object, const QString &name, const QJSValue &handler);

    QJSValue evaluate2(const QString &program,
                      const QString &fileName = QString(),
                      int lineNumber = 0, int columnNumber = 0);

    QJSValue evaluate(const QJSValue &scope, const QString &program,
                      const QString &fileName = QString(),
                      int lineNumber = 0, int columnNumber = 0,
                      QStringList *exceptionStackTrace = nullptr);

    QJSValue evaluate(const QString &program, const QString &fileName = QString(),
                      int lineNumber = 0, int columnNumber = 0,
                      QStringList *exceptionStackTrace = nullptr);

    void cancel();

    // The active flag is different from QScriptEngine::isEvaluating.
    // It is set and cleared externally for example by the rule execution code.
    bool isActive() const { return m_active; }
    void setActive(bool on) { m_active = on; }

    QJSValue call(QJSValue *value, const QJSValueList &args);
    bool hasError() const;
    QJSValue catchError();
    CodeLocation callerLocation(int index) const;
    CodeLocation extractFunctionLocation(const QJSValue &value);
    QString extractFunctionSourceCode(const CodeLocation &location);
    QString extractFunctionSourceCode(const QJSValue &value, QString originalSourceCode,
                                      const CodeLocation &location, int prefixLength = 0);

    void setProxyVtablePatchEnabled(bool enabled, QJSValue *object);

    static ErrorInfo toErrorInfo(const QJSValue &errorValue);

private:
    bool gatherFileResults() const;
    void extendJavaScriptBuiltins();

    static std::mutex m_creationDestructionMutex;
    PropertySet m_propertiesRequestedInScript;
    QHash<QString, PropertySet> m_propertiesRequestedFromArtifact;
    Logger &m_logger;
    QProcessEnvironment m_environment;
    QHash<QString, QString> m_canonicalFilePathResult;
    QHash<QString, bool> m_fileExistsResult;
    QHash<std::pair<QString, quint32>, QStringList> m_directoryEntriesResult;
    QHash<QString, FileTime> m_fileLastModifiedResult;
    bool m_usesIo = false;
    EvalContext m_evalContext;
    std::vector<ResourceAcquiringScriptObject *> m_resourceAcquiringScriptObjects;
    std::vector<std::tuple<QJSValue, QString, QJSValue>> m_observedProperties;
    std::vector<QString> m_importedFilesUsedInScript;
    Set<const ResolvedProduct *> m_productsWithRequestedDependencies;
    RequestedArtifacts m_requestedArtifacts;
    Set<const ResolvedProduct *> m_requestedExports;
    ObserveMode m_observeMode = ObserveMode::Disabled;
    std::unordered_map<const Artifact *, QJSValue> m_artifactScriptValues;
    std::unordered_map<const void *, QJSValue> m_artifactsScriptValues;
    std::unordered_map<const ResolvedProduct *, QJSValue> m_productScriptValues;
    std::unordered_map<const ResolvedProject *, QJSValue> m_projectScriptValues;
    std::unordered_map<const ResolvedModule *, QJSValue> m_moduleScriptValues;

    bool m_profilingEnabled = false;
    QJSValue m_proxyFactory;
    bool m_active = false;
    QJSValue m_definePropertyFunction;
    std::unique_ptr<VTablePatcher> m_vtablePatcher;
    QJSValue m_globalObject;
    QPointer<AbstractProxyHandler> m_scopeProxyHandler;
};

inline ScriptEngine *scriptEngine(const QObject *object)
{
    return static_cast<ScriptEngine *>(qjsEngine(object));
}

class EvalContextSwitcher
{
public:
    EvalContextSwitcher(ScriptEngine *engine, EvalContext newContext)
        : m_engine(engine), m_oldContext(engine->evalContext())
    {
        engine->setEvalContext(newContext);
    }

    ~EvalContextSwitcher() { m_engine->setEvalContext(m_oldContext); }

private:
    ScriptEngine * const m_engine;
    const EvalContext m_oldContext;
};

} // namespace Internal
} // namespace qbs

#endif // QBS_QBSSCRIPTENGINE_H
