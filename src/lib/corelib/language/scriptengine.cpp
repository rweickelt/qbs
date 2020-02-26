

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

#include "scriptengine.h"

#include "filecontextbase.h"
#include "jsimports.h"
#include "propertymapinternal.h"
#include "scriptimporter.h"
#include "preparescriptobserver.h"

#include <buildgraph/artifact.h>
#include <jsextensions/importhelper.h>
#include <jsextensions/jsextensions.h>
#include <language/language.h>
#include <logging/translator.h>
#include <tools/error.h>
#include <tools/fileinfo.h>
#include <tools/profiling.h>
#include <tools/proxyhandler.h>
#include <tools/qbsassert.h>
#include <tools/qttools.h>
#include <tools/stlutils.h>
#include <tools/stringconstants.h>

#include <QtCore/qdebug.h>
#include <QtCore/qdiriterator.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qtextstream.h>
#include <QtCore/qtimer.h>

#include <QtQml/qjsvalueiterator.h>
#include <QtQml/private/qv4engine_p.h>
#include <QtQml/private/qv4functionobject_p.h>
#include <QtQml/private/qv4jscall_p.h>
#include <QtQml/private/qv4object_p.h>
#include <QtQml/private/qv4objectiterator_p.h>
#include <QtQml/private/qv4objectproto_p.h>
#include <QtQml/private/qv4proxy_p.h>
#include <QtQml/private/qv4script_p.h>
#include <QtQml/private/qv4value_p.h>
#include <QtQml/private/qjsvalue_p.h>
#include <QtQml/private/qqmlglobal_p.h>

#include <functional>
#include <set>
#include <utility>

QT_BEGIN_NAMESPACE

namespace QV4 {

static bool removeAllOccurrences(ArrayObject *target, ReturnedValue val) {
    uint len = target->getLength();
    bool found = false;
    for (uint i = 0; i < len; ++i) {
        ReturnedValue v = target->get(i);
        if (v == val) {
            found = true;
            target->put(i, Value::undefinedValue());
        }
    }
    return  found;
}

#ifdef __SANITIZE_ADDRESS__
#define NO_SANITIZE __attribute__((no_sanitize_address))
#else
#define NO_SANITIZE
#endif

struct ProxyObjectOwnPropertyKeyIterator : OwnPropertyKeyIterator
{
    PersistentValue ownKeys;
    uint index = 0;
    uint len = 0;

    NO_SANITIZE ProxyObjectOwnPropertyKeyIterator(ArrayObject *keys);
    NO_SANITIZE ~ProxyObjectOwnPropertyKeyIterator() override = default;
    NO_SANITIZE PropertyKey next(const Object *o, Property *pd = nullptr, PropertyAttributes *attrs = nullptr) override;
};

NO_SANITIZE ProxyObjectOwnPropertyKeyIterator::ProxyObjectOwnPropertyKeyIterator(QV4::ArrayObject *keys)
{
    ownKeys = keys;
    len = keys->getLength();
}

NO_SANITIZE QV4::PropertyKey ProxyObjectOwnPropertyKeyIterator::next(const Object *m, Property *pd, PropertyAttributes *attrs)
{
    if (index >= len || m == nullptr)
        return PropertyKey::invalid();

    Scope scope(m);
    ScopedObject keys(scope, ownKeys.asManaged());
    PropertyKey key = PropertyKey::fromId(keys->get(PropertyKey::fromArrayIndex(index)));
    ++index;

    if (pd || attrs) {
        ScopedProperty p(scope);
        PropertyAttributes a = const_cast<Object *>(m)->getOwnProperty(key, pd ? pd : p);
        if (attrs)
            *attrs = a;
    }

    return key;
}

// Taken from https://codereview.qt-project.org/c/qt/qtdeclarative/+/323120
NO_SANITIZE static QV4::PropertyAttributes patchedVirtualGetOwnProperty(const QV4::Managed *m,
                                                       QV4::PropertyKey id,
                                                       QV4::Property *p)
{
    using namespace QV4;
    Scope scope(m);
    const auto *o = static_cast<const ProxyObject *>(m);
    if (!o->d()->handler) {
        scope.engine->throwTypeError();
        return Attr_Invalid;
    }

    ScopedObject target(scope, o->d()->target);
    Q_ASSERT(target);
    ScopedObject handler(scope, o->d()->handler);
    ScopedString deleteProp(scope, scope.engine->newString(QStringLiteral("getOwnPropertyDescriptor")));
    ScopedValue trap(scope, handler->get(deleteProp));
    if (scope.hasException())
        return Attr_Invalid;
    if (trap->isNullOrUndefined())
        return target->getOwnProperty(id, p);
    if (!trap->isFunctionObject()) {
        scope.engine->throwTypeError();
        return Attr_Invalid;
    }

    JSCallData cdata(scope, 2, nullptr, handler);
    cdata.args[0] = target;
    cdata.args[1] = id.isArrayIndex() ? QV4::Value::fromUInt32(id.asArrayIndex()).toString(scope.engine) : id.asStringOrSymbol();

    ScopedValue trapResult(scope, static_cast<const FunctionObject *>(trap.ptr)->call(cdata));
    if (scope.engine->hasException)
        return Attr_Invalid;
    if (!trapResult->isObject() && !trapResult->isUndefined()) {
        scope.engine->throwTypeError();
        return Attr_Invalid;
    }

    ScopedProperty targetDesc(scope);
    PropertyAttributes targetAttributes = target->getOwnProperty(id, targetDesc);
    if (trapResult->isUndefined()) {
        if (p)
            p->value = Encode::undefined();
        if (targetAttributes == Attr_Invalid) {
            return Attr_Invalid;
        }
        if (!targetAttributes.isConfigurable() || !target->isExtensible()) {
            scope.engine->throwTypeError();
            return Attr_Invalid;
        }
        return Attr_Invalid;
    }

    //bool extensibleTarget = target->isExtensible();
    ScopedProperty resultDesc(scope);
    PropertyAttributes resultAttributes;
    ObjectPrototype::toPropertyDescriptor(scope.engine, trapResult, resultDesc, &resultAttributes);
    resultDesc->completed(&resultAttributes);

    if (!targetDesc->isCompatible(targetAttributes, resultDesc, resultAttributes)) {
        scope.engine->throwTypeError();
        return Attr_Invalid;
    }

    if (!resultAttributes.isConfigurable()) {
        if (targetAttributes == Attr_Invalid || targetAttributes.isConfigurable()) {
            scope.engine->throwTypeError();
            return Attr_Invalid;
        }
    }

    if (p) {
       p->value = resultDesc->value;
        p->set = resultDesc->set;
    }
    return resultAttributes;
}

// Stolen from https://codereview.qt-project.org/c/qt/qtdeclarative/+/327368
NO_SANITIZE static OwnPropertyKeyIterator *patchedVirtualOwnPropertyKeys(const Object *m,
                                                           Value *iteratorTarget)
{
    using namespace QV4;
    Scope scope(m);
    const auto *o = static_cast<const ProxyObject *>(m);
    if (!o->d()->handler) {
        scope.engine->throwTypeError();
        return nullptr;
    }

    ScopedObject target(scope, o->d()->target);
    Q_ASSERT(target);
    ScopedObject handler(scope, o->d()->handler);
    ScopedString name(scope, scope.engine->newString(QStringLiteral("ownKeys")));
    ScopedValue trap(scope, handler->get(name));

    if (scope.hasException())
        return nullptr;
    if (trap->isUndefined())
        return target->ownPropertyKeys(iteratorTarget);
    if (!trap->isFunctionObject()) {
        scope.engine->throwTypeError();
        return nullptr;
    }

    JSCallData cdata(scope, 1, nullptr, handler);
    cdata.args[0] = target;
    ScopedObject trapResult(scope, static_cast<const FunctionObject *>(trap.ptr)->call(cdata));
    if (scope.engine->hasException)
        return nullptr;
    if (!trapResult) {
        scope.engine->throwTypeError();
        return nullptr;
    }

    uint len = trapResult->getLength();
    ScopedArrayObject trapKeys(scope, scope.engine->newArrayObject());
    ScopedStringOrSymbol key(scope);
    for (uint i = 0; i < len; ++i) {
        key = trapResult->get(i);
        if (scope.engine->hasException)
            return nullptr;
        if (!key) {
            scope.engine->throwTypeError();
            return nullptr;
        }
        Value keyAsValue = Value::fromReturnedValue(key->toPropertyKey().id());
        trapKeys->push_back(keyAsValue);
    }

    ScopedArrayObject targetConfigurableKeys(scope, scope.engine->newArrayObject());
    ScopedArrayObject targetNonConfigurableKeys(scope, scope.engine->newArrayObject());
    ObjectIterator it(scope, target, ObjectIterator::EnumerableOnly);
    ScopedPropertyKey k(scope);
    while (1) {
        PropertyAttributes attrs;
        k = it.next(nullptr, &attrs);
        if (!k->isValid())
            break;
        Value keyAsValue = Value::fromReturnedValue(k->id());
        if (attrs.isConfigurable())
            targetConfigurableKeys->push_back(keyAsValue);
        else
            targetNonConfigurableKeys->push_back(keyAsValue);
    }
    if (target->isExtensible() && targetNonConfigurableKeys->getLength() == 0) {
        *iteratorTarget = *m;
        return new ProxyObjectOwnPropertyKeyIterator(trapKeys);
    }

    ScopedArrayObject uncheckedResultKeys(scope, scope.engine->newArrayObject());
    uncheckedResultKeys->copyArrayData(trapKeys);

    len = targetNonConfigurableKeys->getLength();
    for (uint i = 0; i < len; ++i) {
        k = PropertyKey::fromId(targetNonConfigurableKeys->get(i));
        if (!removeAllOccurrences(uncheckedResultKeys, k->id())) {
            scope.engine->throwTypeError();
            return nullptr;
        }
    }

    if (target->isExtensible()) {
        *iteratorTarget = *m;
        return new ProxyObjectOwnPropertyKeyIterator(trapKeys);
    }

    len = targetConfigurableKeys->getLength();
    for (uint i = 0; i < len; ++i) {
        k = PropertyKey::fromId(targetConfigurableKeys->get(i));
        if (!removeAllOccurrences(uncheckedResultKeys, k->id())) {
            scope.engine->throwTypeError();
            return nullptr;
        }
    }

    len = uncheckedResultKeys->getLength();
    for (uint i = 0; i < len; ++i) {
        if (uncheckedResultKeys->get(i) != Encode::undefined()) {
            scope.engine->throwTypeError();
            return nullptr;
        }
    }

    *iteratorTarget = *m;
    return new ProxyObjectOwnPropertyKeyIterator(trapKeys);
}

}

QT_END_NAMESPACE

namespace qbs {
namespace Internal {

namespace {
const QString proxyFactoryCode = QStringLiteral(R"QBS(
     (function(target, handler){
        return new Proxy(target, handler);
     })
)QBS");

const QString definePropertyCode = QStringLiteral(R"QBS(
     (function(obj, name, handler){
        Object.defineProperty(obj, name, handler);
     })
)QBS");
}

// Work-around for QTBUG-86323 and QTBUG-88786. Only needed before Qt 6.1
struct VTablePatcher {
    VTablePatcher(ScriptEngine *engine)
       : engine(engine),
         originalVtable(getVtable())
    {
        // TODO: Is there a way to get the internal class object without creating a proxy?
        QJSValue dummyProxy = engine->newProxyObject(new QObject());
        internalClass = QJSValuePrivate::getValue(&dummyProxy)->objectValue()->internalClass();

        if (!patchedVtable) {
            memcpy(patchedVTableMemory, internalClass->vtable, sizeof(QV4::VTable));
            patchedVtable = reinterpret_cast<QV4::VTable *>(patchedVTableMemory);
            patchedVtable->getOwnProperty = &QV4::patchedVirtualGetOwnProperty;
            patchedVtable->ownPropertyKeys = &QV4::patchedVirtualOwnPropertyKeys;
        }
        setPatchEnabled(true);
    }

    ~VTablePatcher() {
        setPatchEnabled(false);
    }

    const QV4::VTable *getVtable()
    {
        return engine->handle()->classes[QV4::EngineBase::Class_ProxyObject]->vtable;
    }

    void setPatchEnabled(bool enabled)
    {
        if (enabled)
            internalClass->vtable = patchedVtable;
        else
            internalClass->vtable = originalVtable;
    }

    ScriptEngine *engine = nullptr;
    QV4::Heap::InternalClass* internalClass = nullptr;
    const QV4::VTable *originalVtable = nullptr;

    static QV4::VTable *patchedVtable;
    static quint8 patchedVTableMemory[sizeof(QV4::VTable)];
};

quint8 VTablePatcher::patchedVTableMemory[sizeof(QV4::VTable)];
QV4::VTable *VTablePatcher::patchedVtable;

std::mutex ScriptEngine::m_creationDestructionMutex;

ScriptEngine::ScriptEngine(Logger &logger, EvalContext evalContext, QObject *parent)
    : QJSEngine(parent), m_logger(logger), m_evalContext(evalContext)
{
    this->QJSEngine::globalObject().setProperty(QStringLiteral("console"),
                               JsExtensions::loadExtension(this, QStringLiteral("console")));
    extendJavaScriptBuiltins();

    m_proxyFactory = evaluate(proxyFactoryCode, QStringLiteral("proxy.js"));
    if (m_proxyFactory.isError())
      qWarning() << m_proxyFactory.toString();
    Q_ASSERT(!m_proxyFactory.isError());

    m_definePropertyFunction = evaluate(definePropertyCode, QStringLiteral("defineProperty.js"));
    if (m_definePropertyFunction.isError())
      qWarning() << m_definePropertyFunction.toString();
    Q_ASSERT(!m_definePropertyFunction.isError());

    m_vtablePatcher = std::make_unique<VTablePatcher>(this);
    m_globalObject = this->QJSEngine::globalObject();
    // m_scopeProxyHandler = new DefaultProxyHandler();
}

ScriptEngine::~ScriptEngine()
{
    m_creationDestructionMutex.lock();
    connect(this, &QObject::destroyed, std::bind(&std::mutex::unlock, &m_creationDestructionMutex));

    releaseResourcesOfScriptObjects();
    if (m_profilingEnabled) {
        const auto elapsedTime = ImportHelper::instance(this)->elapsedTime();
        logger().qbsLog(LoggerInfo, true)
                << Tr::tr("Setting up imports took %1.").arg(elapsedTimeString(elapsedTime));
    }

    // delete m_scopeProxyHandler;
}

void ScriptEngine::import(const FileContextBaseConstPtr &fileCtx, QJSValue &targetObject,
                             ObserveMode observeMode)
{
    m_observeMode = observeMode;
    const auto *context = ImportHelper::instance(this)->fileContextScope(fileCtx);
    for (auto it = context->imports.constBegin(); it != context->imports.constEnd(); ++it)
        targetObject.setProperty(it.key(), it.value());
}

bool ScriptEngine::gatherFileResults() const
{
    return evalContext() == EvalContext::PropertyEvaluation
            || evalContext() == EvalContext::ProbeExecution;
}

void ScriptEngine::cancel()
{
    setInterrupted(true);
}

void ScriptEngine::checkContext(const QString &operation,
                                const DubiousContextList &dubiousContexts)
{
    for (const DubiousContext &info : dubiousContexts) {
        if (info.context != evalContext())
            continue;
        QString warning;
        switch (info.context) {
        case EvalContext::PropertyEvaluation:
            warning = Tr::tr("Suspicious use of %1 during property evaluation.").arg(operation);
            if (info.suggestion == DubiousContext::SuggestMoving)
                warning += QLatin1Char(' ') + Tr::tr("Should this call be in a Probe instead?");
            break;
        case EvalContext::RuleExecution:
            warning = Tr::tr("Suspicious use of %1 during rule execution.").arg(operation);
            if (info.suggestion == DubiousContext::SuggestMoving) {
                warning += QLatin1Char(' ')
                        + Tr::tr("Should this call be in a JavaScriptCommand instead?");
            }
            break;
        case EvalContext::ModuleProvider:
        case EvalContext::ProbeExecution:
        case EvalContext::JsCommand:
            QBS_ASSERT(false, continue);
            break;
        }
        m_logger.printWarning(ErrorInfo(warning, QStringList()));
        return;
    }
}

void ScriptEngine::addFileLastModifiedResult(const QString &filePath, const FileTime &fileTime)
{
    if (gatherFileResults())
        m_fileLastModifiedResult.insert(filePath, fileTime);
}

ResourceAcquiringScriptObject::~ResourceAcquiringScriptObject()
{
    // Unregister from the engine in case we are destroyed before
    // releseResources() has been called. This happens sometimes
    // and cannot be controlled.
    if (!m_released) {
        m_engine->removeResourceAcquiringScriptObject(this);
    }
}

void ScriptEngine::addResourceAcquiringScriptObject(ResourceAcquiringScriptObject *obj)
{
    m_resourceAcquiringScriptObjects.push_back(obj);
}

void ScriptEngine::removeResourceAcquiringScriptObject(ResourceAcquiringScriptObject *obj)
{
    removeOne(m_resourceAcquiringScriptObjects, obj);
}

void ScriptEngine::releaseResourcesOfScriptObjects()
{
    if (m_resourceAcquiringScriptObjects.empty())
        return;
    for (auto obj: m_resourceAcquiringScriptObjects) {
        obj->releaseResources();
        obj->setReleased();
    }
    m_resourceAcquiringScriptObjects.clear();
}

void ScriptEngine::addCanonicalFilePathResult(const QString &filePath,
                                              const QString &resultFilePath)
{
    if (gatherFileResults())
        m_canonicalFilePathResult.insert(filePath, resultFilePath);
}

void ScriptEngine::addFileExistsResult(const QString &filePath, bool exists)
{
    if (gatherFileResults())
        m_fileExistsResult.insert(filePath, exists);
}

void ScriptEngine::addDirectoryEntriesResult(const QString &path, QDir::Filters filters,
                                             const QStringList &entries)
{
    if (gatherFileResults()) {
        m_directoryEntriesResult.insert(
                    std::pair<QString, quint32>(path, static_cast<quint32>(filters)),
                    entries);
    }
}

Set<QString> ScriptEngine::imports() const
{
    return ImportHelper::instance(this)->importedFiles();
}

void ScriptEngine::addImportedFileUsedInScript(const QString &filePath)
{
    // Import list is assumed to be small, so let's not use a set.
    if (!contains(m_importedFilesUsedInScript, filePath))
        m_importedFilesUsedInScript.push_back(filePath);
}

void ScriptEngine::addPropertyRequestedFromArtifact(const Artifact *artifact,
                                                       const Property &property)
{
    m_propertiesRequestedFromArtifact[artifact->filePath()] << property;
}

std::vector<QString> ScriptEngine::importedFilesUsedInScript() const
{
    return m_importedFilesUsedInScript;
}

void ScriptEngine::unobserveProperties()
{

}

void ScriptEngine::enableProfiling(bool enabled)
{
    m_profilingEnabled = enabled;
    ImportHelper::instance(this)->setProfilingEnabled(enabled);
}

void ScriptEngine::extendJavaScriptBuiltins()
{
    class JSTypeExtender
    {
    public:
        JSTypeExtender(ScriptEngine *engine, const QString &typeName)
            : m_engine(engine), m_typeName(typeName)
        {
            m_jsProto = engine->QJSEngine::globalObject().property(typeName).property(
                        QStringLiteral("prototype"));
            QBS_ASSERT(m_jsProto.isObject(), return);

            m_installer = engine->evaluate(QStringLiteral(
                            "(function(p, n, f){ "
                            "    Object.defineProperty(p, n, {value: f, enumerable: false});"
                            "})"));
            QBS_ASSERT(m_installer.isCallable(), return);
        }

        void addFunction(const QString &name, const QString &code)
        {
            QString file = QStringLiteral("%1.%2.js").arg(m_typeName, name);
            QJSValue func = m_engine->QJSEngine::evaluate(code, file);
            QBS_ASSERT(!func.isError(), return);
            m_installer.call({m_jsProto, QJSValue(name), func});
        }

    private:
        ScriptEngine *const m_engine;
        QString m_typeName;
        QJSValue m_jsProto;
        QJSValue m_installer;
    };

    JSTypeExtender arrayExtender(this, QStringLiteral("Array"));
    arrayExtender.addFunction(QStringLiteral("contains"),
        QStringLiteral("(function(e){return this.indexOf(e) !== -1;})"));
    arrayExtender.addFunction(QStringLiteral("containsAll"),
        QStringLiteral("(function(e){var $this = this;"
                        "return e.every(function (v) { return $this.contains(v) });})"));
    arrayExtender.addFunction(QStringLiteral("containsAny"),
        QStringLiteral("(function(e){var $this = this;"
                        "return e.some(function (v) { return $this.contains(v) });})"));
    arrayExtender.addFunction(QStringLiteral("uniqueConcat"),
        QStringLiteral("(function(other){"
                        "var r = this.concat();"
                        "var s = {};"
                        "r.forEach(function(x){ s[x] = true; });"
                        "other.forEach(function(x){"
                            "if (!s[x]) {"
                              "s[x] = true;"
                              "r.push(x);"
                            "}"
                        "});"
                        "return r;})"));

    JSTypeExtender stringExtender(this, QStringLiteral("String"));
    stringExtender.addFunction(QStringLiteral("contains"),
        QStringLiteral("(function(e){return this.indexOf(e) !== -1;})"));
    stringExtender.addFunction(QStringLiteral("startsWith"),
        QStringLiteral("(function(e){return this.slice(0, e.length) === e;})"));
    stringExtender.addFunction(QStringLiteral("endsWith"),
        QStringLiteral("(function(e){ return this.slice(-e.length) === e;})"));

    // Workaround for QTBUG-90404, but this one should even be more safe
    JSTypeExtender functionExtender(this, QStringLiteral("Function"));
    functionExtender.addFunction(QStringLiteral("call"),
    QStringLiteral(R"QBS((function(context, ...args) {
                           const fn = Symbol();
                           if (!context)
                             context = {};
                           try {
                             context[fn] = this;
                             return context[fn](...args);
                            } catch(e) {
                               // Turn primitive types into complex ones 1 -> Number
                              context = new context.constructor(context);
                              context[fn] = this;
                              return context[fn](...args);
                           }
                         }))QBS"));
}

QJSValue ScriptEngine::newProxyObject(QObject *handler)
{
    return newProxyObject(newObject(), handler);
}

QJSValue ScriptEngine::newProxyObject(const QJSValue &target, QObject *handler)
{
    return newProxyObject(target, newQObject(handler));
}

QJSValue ScriptEngine::newProxyObject(const QJSValue &target, const QJSValue &handler)
{
    if (m_vtablePatcher)
        m_vtablePatcher->setPatchEnabled(false);
    QJSValue proxy = m_proxyFactory.call(QJSValueList{ target, handler });
    QBS_ASSERT(!proxy.isError(),  qFatal("%s", qPrintable(proxy.toString())));
    if (m_vtablePatcher)
        m_vtablePatcher->setPatchEnabled(true);
    return proxy;
}

void ScriptEngine::defineProperty(QJSValue &object, const QString &name, const QJSValue &handler)
{
    m_definePropertyFunction.call(QJSValueList{object, name, handler});
}

ScriptEngine *ScriptEngine::create(Logger &logger, EvalContext evalContext, QObject *parent)
{
    std::lock_guard<std::mutex> lock(m_creationDestructionMutex);
    return new ScriptEngine(logger, evalContext, parent);
}

QJSValueList ScriptEngine::argumentList(const QStringList &argumentNames,
                                           const QJSValue &context)
{
    QJSValueList result;
    for (const auto &name : argumentNames)
        result += context.property(name);
    return result;
}

ErrorInfo ScriptEngine::toErrorInfo(const QJSValue &errorValue)
{
    const QString fileName =
            QUrl(errorValue.property(QStringLiteral("fileName")).toString()).path();
    int lineNumber = errorValue.property(QStringLiteral("lineNumber")).toInt();
    const QString message = errorValue.property(QStringLiteral("message")).toString();
    CodeLocation errorLocation(fileName, lineNumber, 0, false);
    return ErrorInfo(message, errorLocation);
}

bool ScriptEngine::hasError() const
{
    return handle()->hasException;
}

QJSValue ScriptEngine::catchError()
{
    if (handle()->hasException) {
        QJSValue result;
        QJSValuePrivate::setValue(&result, handle(), handle()->catchException());
        return result;
    } else {
        return QJSValue();
    }
}

// TODO: Remove, not needed anymore
CodeLocation ScriptEngine::callerLocation(int index) const
{
    QV4::ExecutionEngine* executionEngine = this->handle();
    Q_ASSERT(executionEngine != nullptr);

    QV4::StackTrace trace = executionEngine->stackTrace(index+1);
    QV4::StackFrame& frame = trace.last();

    CodeLocation result(QUrl(frame.source).toLocalFile(), frame.line, frame.column);
    return result;
}

// Rewrite of QJSValue::call(). We need to catch normal throw statements and treat them
// as errors.
QJSValue ScriptEngine::call(QJSValue *value, const QJSValueList &args)
{
    using namespace QV4;
    QV4::Value *val = QJSValuePrivate::getValue(value);
    if (!val)
        return QJSValue();

    auto *f = val->as<FunctionObject>();
    if (!f)
        return QJSValue();

    ExecutionEngine *engine = QJSValuePrivate::engine(value);
    Q_ASSERT(engine);

    Scope scope(engine);
    JSCallData jsCallData(scope, args.length());
    *jsCallData->thisObject = engine->globalObject;
    for (int i = 0; i < args.size(); ++i) {
        if (!QJSValuePrivate::checkEngine(engine, args.at(i))) {
            qWarning("QJSValue::call() failed: cannot call function with argument created in a different engine");
            return QJSValue();
        }
        jsCallData->args[i] = QJSValuePrivate::convertedToValue(engine, args.at(i));
    }

    ScopedValue result(scope, f->call(jsCallData));
    if (engine->hasException) {
        QQmlError error = engine->catchExceptionAsQmlError();
        if (error.description().isEmpty())
            error.setDescription(QLatin1String("Exception occurred during function evaluation"));
        QJSValue ev = newErrorObject(QJSValue::GenericError, error.description());
        ev.setProperty(QStringLiteral("fileName"), error.url().toLocalFile());
        ev.setProperty(QStringLiteral("lineNumber"), error.line());
        return ev;
    }
    if (engine->isInterrupted.loadAcquire())
        result = engine->newErrorObject(QStringLiteral("Interrupted"));

    return QJSValue(engine, result->asReturnedValue());
}

// Converts thrown non-error values to errors.
// Fixes the location in case the exception was thrown inside an eval() expression.
static void convertExceptionToError(ScriptEngine *engine, QJSValue &value,
                                    QStringList *stackTrace)
{
    if (!stackTrace || stackTrace->isEmpty())
        return;

    QString frame = stackTrace->takeFirst();
    // Discard eval() stack frames in favor of the first real file location
    // instead (where eval() is called).
    if (frame.contains(QStringLiteral("eval code")))
        frame = stackTrace->takeFirst();

    QRegularExpression reg(
                QStringLiteral(":(?<line>\\d+):(?<column>-?\\d+):(?<url>file://.*)"));
    const auto match = reg.match(frame);
    QBS_CHECK(match.hasMatch());
    const QString fileName = QUrl(match.captured(QStringLiteral("url"))).toLocalFile();
    const int lineNumber = match.captured(QStringLiteral("line")).toInt();
    const QString message = value.property(QStringLiteral("message")).toString();

    if (!value.isError())
        value = engine->newErrorObject(QJSValue::GenericError, message);
    value.setProperty(QStringLiteral("fileName"), fileName);
    value.setProperty(QStringLiteral("lineNumber"), lineNumber);
}

QJSValue ScriptEngine::evaluate2(const QString &program, const QString &fileName, int lineNumber,
                                    int columnNumber)
{
    static const QString proxyName = QStringLiteral("__scopeProxi");
    static const QString prefix = QStringLiteral("with(") + proxyName + QStringLiteral("){");
    static const QString suffix = QStringLiteral("}");
    const QString script = prefix + program + suffix;
    QStringList exceptionStackTrace;

    QJSValue proxy = newProxyObject(m_globalObject, new DefaultProxyHandler());
    Q_ASSERT(!proxy.isError());
    Q_ASSERT(!proxy.isUndefined());

    this->QJSEngine::globalObject().setProperty(proxyName, proxy);
    QJSValue result = evaluate(script, fileName, lineNumber, columnNumber, &exceptionStackTrace);
    this->QJSEngine::globalObject().setProperty(proxyName, QJSValue());

    convertExceptionToError(this, result, &exceptionStackTrace);
    return result;
}

QJSValue ScriptEngine::evaluate(const QJSValue &scope, const QString &program,
                                   const QString &fileName, int lineNumber, int columnNumber,
                                   QStringList *exceptionStackTrace)
{
    static const QString proxyName = QStringLiteral("__scopeProxy");
    static const QString prefix = QStringLiteral("with(") + proxyName + QStringLiteral("){");
    static const QString suffix = QStringLiteral("}");
    const QString script = prefix + program + suffix;

    this->QJSEngine::globalObject().setProperty(proxyName, scope);
    QJSValue result = evaluate(script, fileName, lineNumber, columnNumber, exceptionStackTrace);
    this->QJSEngine::globalObject().setProperty(proxyName, QJSValue());

    convertExceptionToError(this, result, exceptionStackTrace);
    return result;
}

static QUrl urlForFileName(const QString &fileName)
{
    if (!fileName.startsWith(QLatin1Char(':')))
        return QUrl::fromLocalFile(fileName);

    QUrl url;
    url.setPath(fileName.mid(1));
    url.setScheme(QLatin1String("qrc"));
    return url;
}

CodeLocation ScriptEngine::extractFunctionLocation(const QJSValue &value)
{
    QBS_CHECK(value.isCallable());
    QQmlSourceLocation location =
            QJSValuePrivate::getValue(&value)->as<QV4::FunctionObject>()->sourceLocation();

    // Evaluation prefixes the script with 'with(__scopeProxy){'
    static const int prefixLength = 19;
    const QString filePath = QUrl(location.sourceFile).toLocalFile();
    const int line = location.line;
    const int column = (location.line != 0) ? location.column - 1
                                      : location.column - 1 - prefixLength;

    return CodeLocation(filePath, line, column);
}

// Work-around for QTBUG-54925
// Stolen from https://codereview.qt-project.org/c/qt/qtdeclarative/+/312028
QJSValue ScriptEngine::evaluate(const QString &program, const QString &fileName, int lineNumber,
                                   int columnNumber, QStringList *exceptionStackTrace)
{
    QV4::ExecutionEngine *v4 = handle();
    QV4::Scope scope(v4);
    QV4::ScopedValue result(scope);

    QV4::Script script(v4->rootContext(), QV4::Compiler::ContextType::Global, program, urlForFileName(fileName).toString(), lineNumber, columnNumber);
    script.strictMode = false;
    if (v4->currentStackFrame)
        script.strictMode = v4->currentStackFrame->v4Function->isStrict();
    else if (v4->globalCode)
        script.strictMode = v4->globalCode->isStrict();
    script.inheritContext = true;
    script.parse();
    if (!scope.engine->hasException)
        result = script.run();
    if (exceptionStackTrace)
        exceptionStackTrace->clear();
    if (scope.engine->hasException) {
        QV4::StackTrace trace;
        result = v4->catchException(&trace);
        if (exceptionStackTrace) {
            for (auto &&frame: trace) {
                exceptionStackTrace->push_back(QString::fromLatin1("%1:%2:%3:%4").arg(
                                          frame.function,
                                          QString::number(frame.line),
                                          QString::number(frame.column),
                                          frame.source)
                                      );
            }
        }
    }
    if (v4->isInterrupted.loadAcquire())
        result = v4->newErrorObject(QStringLiteral("Interrupted"));

    return QJSValue(v4, result->asReturnedValue());
}

// Work-around for QTBUG-46122
QString ScriptEngine::extractFunctionSourceCode(const CodeLocation &location)
{
    const int line = location.line();
    const int column = location.column();
    QFile file(location.filePath());
    QBS_ASSERT(file.exists(), qWarning() << "File does not exist: " << file.fileName());
    QBS_CHECK(file.open(QIODevice::ReadOnly));
    QTextStream input(&file);

    // Seek the begin of the function
    for (int i = 1; i < line; ++i)
        input.readLine();
    input.read(column);
    // Seek the end of the function
    // Technically we would need to parse the function to find the end.
    // But for simplicity we just count { and } and hope that curly braces
    // are always used in pairs.
    QString extractedSourceCode;
    QTextStream output(&extractedSourceCode);
    std::stack<QChar> stack;
    QChar c;
    while (!input.atEnd()) {
        input >> c;
        output << c;
        if (c == QLatin1Char('{')) {
            stack.push(QLatin1Char('}'));
        } else if (c == QLatin1Char('}')) {
            if (c == stack.top())
                stack.pop();
            else
                throw ErrorInfo(QStringLiteral("blabla"));
        } else {
            continue;
        }
        if (stack.empty())
            break;
    }

    return extractedSourceCode;
}

QString ScriptEngine::extractFunctionSourceCode(const QJSValue &value,
                                                   QString originalSourceCode,
                                                   const CodeLocation &location,
                                                   int prefixLength)
{
    QBS_CHECK(value.isCallable());

    QQmlSourceLocation observedLocation =
            QJSValuePrivate::getValue(&value)->as<QV4::FunctionObject>()->sourceLocation();
    int line = observedLocation.line - location.line();
    // Column matters only if the errors happen on the first line
    int column = (line != 0) ? observedLocation.column - 1
                             : observedLocation.column - 1 - prefixLength;

    QTextStream input(&originalSourceCode);
    // Seek the begin of the function
    for (int i = 0; i < line; ++i)
        input.readLine();
    input.read(column);
    // Seek the end of the function
    // Technically we would need to parse the function to find the end.
    // But for simplicity we just count { and } and hope that curly braces
    // are not used elsewhere.
    QString extractedSourceCode;
    QTextStream output(&extractedSourceCode);
    std::stack<QChar> stack;
    QChar c;
    while (!input.atEnd()) {
        input >> c;
        output << c;
        if (c == QLatin1Char('{')) {
            stack.push(QLatin1Char('}'));
        } else if (c == QLatin1Char('}')) {
            if (c == stack.top())
                stack.pop();
            else
                throw ErrorInfo(QStringLiteral("blabla"));
        } else {
            continue;
        }
        if (stack.empty())
            break;
    }
    return extractedSourceCode;
}

} // namespace Internal
} // namespace qbs
