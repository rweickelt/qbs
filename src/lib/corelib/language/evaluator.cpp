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

#include "evaluator.h"

#include "filetags.h"
#include "item.h"
#include "scriptengine.h"
#include "value.h"

#include <buildgraph/buildgraph.h>
#include <jsextensions/importhelper.h>
#include <jsextensions/jsextensions.h>
#include <logging/translator.h>
#include <tools/error.h>
#include <tools/fileinfo.h>
#include <tools/scripttools.h>
#include <tools/qbsassert.h>
#include <tools/qttools.h>
#include <tools/stringconstants.h>

#include <QtCore/qdebug.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qscopeguard.h>
#include <QtCore/qtemporaryfile.h>
#include <QtCore/qstringbuilder.h>
#include <QtQml/qjsvalueiterator.h>

namespace qbs {
namespace Internal {

class ErrorGuard {
public:
    ErrorGuard(Evaluator *evaluator): m_evaluator(evaluator) {
        Q_ASSERT(!m_evaluator->m_errors.hasError());
    }

    ~ErrorGuard() noexcept(false)
    {
        if (m_evaluator->m_errors.hasError() && m_evaluator->m_nestLevel == 0) {
            const ErrorInfo errors(m_evaluator->m_errors);
            m_evaluator->m_errors.clear();
            throw errors;
        }
    }

    Evaluator *m_evaluator;
};

class PropertyStackManager
{
public:
    PropertyStackManager(const Item *itemOfProperty, const QString &name, const Value *value,
                         std::stack<QualifiedId> &requestedProperties,
                         PropertyDependencies &propertyDependencies)
        : m_requestedProperties(requestedProperties)
    {
        if (value->type() == Value::JSSourceValueType
                && (itemOfProperty->type() == ItemType::ModuleInstance
                    || itemOfProperty->type() == ItemType::Module
                    || itemOfProperty->type() == ItemType::Export)) {
            const VariantValueConstPtr varValue
                    = itemOfProperty->variantProperty(StringConstants::nameProperty());
            if (!varValue)
                return;
            m_stackUpdate = true;
            const QualifiedId fullPropName
                    = QualifiedId::fromString(varValue->value().toString()) << name;
            if (!requestedProperties.empty())
                propertyDependencies[fullPropName].insert(requestedProperties.top());
            m_requestedProperties.push(fullPropName);
        }
    }

    ~PropertyStackManager()
    {
        if (m_stackUpdate)
            m_requestedProperties.pop();
    }

private:
    std::stack<QualifiedId> &m_requestedProperties;
    bool m_stackUpdate = false;
};

class ItemProxyHandler: public QObject {
    Q_OBJECT
public:
    ItemProxyHandler(const Item *item, Evaluator *evaluator): m_evaluator(evaluator), m_item(item)
    {
    }

    Q_INVOKABLE bool has(const QJSValue &target, const QJSValue &key)
    {
        Q_UNUSED(target);
        Q_ASSERT(key.isString());
        return m_item->hasProperty(key.toString());
    }

    Q_INVOKABLE QJSValue get(const QJSValue &target, const QJSValue &key, const QJSValue &receiver)
    {
        Q_UNUSED(target);
        Q_UNUSED(receiver);
        return m_evaluator->evaluateProperty(m_item, key.toString());
    }

    Q_INVOKABLE bool set(const QJSValue &target, const QJSValue &key, const QJSValue &value,
                         const QJSValue &receiver)
    {
        Q_UNUSED(target);
        Q_UNUSED(key);
        Q_UNUSED(value);
        Q_UNUSED(receiver);

        m_evaluator->throwError(
                    QStringLiteral("Error: Write access is not allowed in this context"));

        return false;
    }

    Q_INVOKABLE QStringList ownKeys(const QJSValue &target)
    {
        Q_UNUSED(target);
        return m_item->properties().keys();
    }

    Q_INVOKABLE QJSValue getOwnPropertyDescriptor(QJSValue target, const QJSValue &key)
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

protected:
    Evaluator *m_evaluator;
    const Item *m_item;
};

class ParentProxyHandler: public ItemProxyHandler {
    Q_OBJECT
public:
    ParentProxyHandler(const Item *item, Evaluator *evaluator)
        : ItemProxyHandler(item, evaluator)
    {
    }

    Q_INVOKABLE QJSValue get(const QJSValue &target, const QJSValue &key, const QJSValue &receiver)
    {
        Q_UNUSED(target);
        Q_UNUSED(receiver);
        if (!key.isString())
            return QJSValue();

        const QString name = key.toString();
        for (const Item *item = m_item; item != nullptr; item = item->parent()) {
            if (item->hasProperty(name))
                return m_evaluator->evaluateProperty(item, key.toString());
        }
        return QJSValue();
    }
};

class UnqualifiedLookupProxyHandler: public QObject {
    Q_OBJECT
public:
    struct Config {
        const ImportHelper::FileContextScope *fileContext = nullptr;
        const QList<const Item *> *scopeList = nullptr;
        const QMap<QString, QJSValue> *convenienceProperties = nullptr;
    };

    UnqualifiedLookupProxyHandler(Evaluator *evaluator): m_evaluator(evaluator)
    {
    }

    ~UnqualifiedLookupProxyHandler()
    {
        // This might look a bit cumbersome, but we do not want to deal
        // with std::shared_ptr here.
        if (m_config != nullptr) {
            // This is only the case for the non-global var property instances
            // of JSValueProxyHandler. In that case, convenienceProperties and
            // scopeList are clones of the containers created in
            // Evaluator::evaluateJSSourceValue() and m_config is not automatically destroyed.
            delete m_config->convenienceProperties;
            delete m_config->scopeList;
            delete m_config;
        }
    }

    Q_INVOKABLE bool has(const QJSValue &target, const QJSValue &key)
    {
        Q_UNUSED(target);

        const auto scopeList = m_config->scopeList;
        const auto convenienceProperties = m_config->convenienceProperties;
        const auto fileContext = m_config->fileContext;
        const auto name = key.toString();

        m_resolvedItem = nullptr;
        m_isResolvedConvenienceProperty = false;
        m_isResolvedImport = false;
        m_isResolvedFileContextProperty = false;

        m_resolvedConvenienceProperty = convenienceProperties->constFind(name);
        if (m_resolvedConvenienceProperty != convenienceProperties->cend()) {
            m_isResolvedConvenienceProperty = true;
            return true;
        }

        m_resolvedImport = fileContext->imports.constFind(name);
        if (m_resolvedImport != fileContext->imports.cend()) {
            m_isResolvedImport = true;
            return true;
        }

        for (const Item *i: *scopeList) {
            if (i->hasProperty(name)) {
                m_resolvedItem = i;
                return true;
            }
        }

        m_resolvedFileContextProperty = fileContext->convenienceProperties.constFind(name);
        if (m_resolvedFileContextProperty != fileContext->convenienceProperties.cend()) {
            m_isResolvedFileContextProperty = true;
            return true;
        }

        return false;
    }

    Q_INVOKABLE QJSValue get(const QJSValue &target, const QJSValue &key,
                             const QJSValue &receiver)
    {
        Q_UNUSED(target);
        Q_UNUSED(receiver);

        const auto name = key.toString();

        // TODO: This breaks iterability. What to return instead?
        if (!key.isString() && name == QStringLiteral("Symbol(Symbol.unscopables)"))
            return QJSValue();

        if (m_isResolvedConvenienceProperty)
            return m_resolvedConvenienceProperty.value();

        if (m_isResolvedImport)
            return m_resolvedImport.value();

        if (m_resolvedItem)
            return m_evaluator->evaluateProperty(m_resolvedItem, name);

        if (m_isResolvedFileContextProperty)
            return m_resolvedFileContextProperty.value();

        // TODO: Why not appendError()?
        qjsEngine(this)->throwError(QJSValue::ReferenceError,
                                    QStringLiteral("%1 is not defined").arg(name));
        return QJSValue();

    }

    Q_INVOKABLE bool set(const QJSValue &target, const QJSValue &key, const QJSValue &value,
                         const QJSValue &receiver)
    {
        Q_UNUSED(target);
        Q_UNUSED(key);
        Q_UNUSED(value);
        Q_UNUSED(receiver);

        m_evaluator->throwError(
                    QStringLiteral("Error: Write access is not allowed in this context"));

        return false;
    }

    const Config *configuration() const { return m_config; }
    void setConfiguration(const Config *configuration) {
        m_config = configuration;
        m_resolvedItem = nullptr;
        m_isResolvedFileContextProperty = false;
        m_isResolvedImport = false;
        m_isResolvedConvenienceProperty = false;
    }

private:
    Evaluator *m_evaluator;
    const Config *m_config = nullptr;

    QMap<QString, QJSValue>::const_iterator m_resolvedConvenienceProperty;
    QMap<QString, QJSValue>::const_iterator m_resolvedImport;
    QMap<QString, QJSValue>::const_iterator m_resolvedFileContextProperty;
    const Item *m_resolvedItem = nullptr;
    bool m_isResolvedConvenienceProperty = false;
    bool m_isResolvedImport = false;
    bool m_isResolvedFileContextProperty = false;
};

class ProbeScriptProxyHandler: public QObject {
    Q_OBJECT
public:
    ProbeScriptProxyHandler(Evaluator *evaluator): m_evaluator(evaluator)
    {
    }

    Q_INVOKABLE bool has(const QJSValue &target, const QJSValue &key)
    {
        Q_UNUSED(target);
        Q_ASSERT(key.isString());

        QBS_CHECK(m_properties);
        QBS_CHECK(m_fileContext);

        const QString name = key.toString();

        m_resolvedProperty = m_properties->find(name);
        if (m_resolvedProperty != m_properties->end())
            return true;

        m_resolvedImport = m_fileContext->imports.find(name);
        if (m_resolvedImport != m_fileContext->imports.cend())
            return true;

        m_resolvedConvenienceProperty = m_fileContext->convenienceProperties.find(name);
        if (m_resolvedConvenienceProperty != m_fileContext->convenienceProperties.cend())
            return true;

        return false;
    }

    Q_INVOKABLE QJSValue get(const QJSValue &target, const QJSValue &key, const QJSValue &receiver)
    {
        Q_UNUSED(target);
        Q_UNUSED(receiver);
        const QString name = key.toString();

        if (m_resolvedProperty != m_properties->end())
            return m_resolvedProperty.value();

        if (m_resolvedImport != m_fileContext->imports.cend())
            return m_resolvedImport.value();

        if (m_resolvedConvenienceProperty != m_fileContext->convenienceProperties.cend())
            return m_resolvedConvenienceProperty.value();

        // Should not happen since QJSEngine always call has() before
        QBS_CHECK(false);
    }

    Q_INVOKABLE bool set(const QJSValue &target, const QJSValue &key, const QJSValue &value,
                         const QJSValue &receiver)
    {
        Q_UNUSED(target);
        Q_UNUSED(receiver);

        const QString name = key.toString();

        if (m_resolvedProperty != m_properties->end()) {
            *m_resolvedProperty = value;
            const auto decl = m_item->propertyDeclaration(name);
            m_evaluator->convertToPropertyType(*m_resolvedProperty, m_item, decl);
            return true;
        }
        else {
            Q_ASSERT(false); // TODO: Can this happen?
            m_evaluator->throwError(QStringLiteral("Type Error: %1 is not  writable").arg(name));
            return false;
        }
    }

    void init(const Item *item, const ImportHelper::FileContextScope *fileContext,
              QMap<QString, QJSValue> *properties) {

        m_item = item;

        m_fileContext = fileContext;
        if (fileContext) {
            m_resolvedConvenienceProperty = fileContext->convenienceProperties.cend();
            m_resolvedImport = fileContext->convenienceProperties.cend();
        }

        m_properties = properties;
        if (m_properties) {
            m_resolvedProperty = m_properties->end();
        }
    }

private:
    Evaluator *m_evaluator;
    const Item *m_item;
    const ImportHelper::FileContextScope *m_fileContext = nullptr;
    QMap<QString, QJSValue> *m_properties = nullptr;
    QMap<QString, QJSValue>::iterator m_resolvedProperty;
    QMap<QString, QJSValue>::const_iterator m_resolvedImport;
    QMap<QString, QJSValue>::const_iterator m_resolvedConvenienceProperty;
};

Evaluator::Evaluator(ScriptEngine *scriptEngine)
    : m_scriptEngine(scriptEngine)
{
    QJSValue proxy;

    m_unqualifiedLookupProxy = new UnqualifiedLookupProxyHandler(this);
    proxy = engine()->newProxyObject(engine()->newObject(), m_unqualifiedLookupProxy);
    engine()->globalObject().setProperty(QStringLiteral("__jsvalueproxy"), proxy);

    m_probeScriptProxy = new ProbeScriptProxyHandler(this);
    proxy = engine()->newProxyObject(engine()->newObject(), m_probeScriptProxy);
    engine()->globalObject().setProperty(QStringLiteral("__probescriptproxy"), proxy);

    ImportHelper *importer = ImportHelper::instance(engine());
    importer->setEvaluator(this);
}


QJSValue Evaluator::createItemProxy(const Item *item)
{
    const QJSValue proxy =
            engine()->newProxyObject(engine()->newObject(),  new ItemProxyHandler(item, this));

    char*const* id = reinterpret_cast<char*const*>(&proxy);
    quintptr valueId = **reinterpret_cast<quintptr*const*>(id);

    Q_ASSERT(!m_itemMap.contains(valueId));
    m_itemMap.insert(valueId, const_cast<Item *>(item));
    return proxy;
}

static bool isExclusiveListValue(ValueConstPtr value) {
    const auto jsv = std::dynamic_pointer_cast<const JSSourceValue>(value);
    return (jsv && jsv->isExclusiveListValue());
}

void Evaluator::clearCache(const Item *item, const QString &name)
{
    if (!m_evaluationDataMap.contains(item))
        return;

    if (name.isNull())
        m_evaluationDataMap[item].cache.clear();
    else
        m_evaluationDataMap[item].cache.remove(name);
}

QJSValue Evaluator::value(const Item *item, const QString &name, bool *propertyWasSet)
{
    QJSValue v;
    evaluateProperty(&v, item, name, propertyWasSet);
    return v;
}

bool Evaluator::boolValue(const Item *item, const QString &name,
                          bool *propertyWasSet)
{
    return value(item, name, propertyWasSet).toBool();
}

int Evaluator::intValue(const Item *item, const QString &name, int defaultValue,
                        bool *propertyWasSet)
{
    QJSValue v;
    if (!evaluateProperty(&v, item, name, propertyWasSet))
        return defaultValue;
    return v.toInt();
}

FileTags Evaluator::fileTagsValue(const Item *item, const QString &name, bool *propertySet)
{
    return FileTags::fromStringList(stringListValue(item, name, propertySet));
}

QString Evaluator::stringValue(const Item *item, const QString &name,
                               const QString &defaultValue, bool *propertyWasSet)
{
    QJSValue v;
    if (!evaluateProperty(&v, item, name, propertyWasSet))
        return defaultValue;
    return v.toString();
}

static QStringList toStringList(const QJSValue &scriptValue)
{
    if (scriptValue.isString()) {
        return {scriptValue.toString()};
    } else if (scriptValue.isArray()) {
        QStringList lst;
        int i = 0;
        forever {
            QJSValue elem = scriptValue.property(i++);
            if (elem.isNull() || elem.isUndefined() || elem.isError())
                break;
            lst.push_back(elem.toString());
        }
        return lst;
    }
    return {};
}

QStringList Evaluator::stringListValue(const Item *item, const QString &name, bool *propertyWasSet)
{
    QJSValue v = evaluateProperty(item, name);
    if (propertyWasSet)
        *propertyWasSet = isNonDefaultValue(item, name);
    return toStringList(v);
}

bool Evaluator::isNonDefaultValue(const Item *item, const QString &name) const
{
    const ValueConstPtr v = item->property(name);
    return v && (v->type() != Value::JSSourceValueType
                 || !static_cast<const JSSourceValue *>(v.get())->isBuiltinDefaultValue());
}

EvaluationData &Evaluator::evaluationData(const Item *item)
{
    EvaluationData &edata = m_evaluationDataMap[item];
    if (!edata.item) {
        edata.item = item;
        edata.item->setObserver(this);
        edata.itemProxy = createItemProxy(item);
    }
    return edata;
}

QJSValue Evaluator::scriptValue(const Item *item)
{
    const auto &edata = evaluationData(item);
    return edata.itemProxy;
}

void Evaluator::onItemPropertyChanged(Item *item)
{
    const EvaluationData &data = evaluationData(item);
    data.cache.clear();
}

void Evaluator::setPathPropertiesBaseDir(const QString &dirPath)
{
    m_pathPropertiesBaseDir = dirPath;
}

void Evaluator::clearPathPropertiesBaseDir()
{
    m_pathPropertiesBaseDir = QString();
}

bool Evaluator::evaluateProperty(QJSValue *result, const Item *item, const QString &name,
                                 bool *propertyWasSet)
{
    *result = evaluateProperty(item, name);
    if (propertyWasSet)
        *propertyWasSet = isNonDefaultValue(item, name);

    return !result->isError() && !result->isUndefined();
}

QJSValue Evaluator::evaluateProperty(const Item *item, const QString &name)
{
    const ErrorGuard errorGuard(this);
    ValueConstPtr v;
    const Item *itemOfProperty;
    for (itemOfProperty = item; itemOfProperty; itemOfProperty = itemOfProperty->prototype()) {
        v = itemOfProperty->ownProperty(name);
        if (v)
            break;
    }

    if (!v) {
        return QJSValue();
    }

    PropertyStackManager propertyStackManager(itemOfProperty, name, v.get(),
                                              m_requestedProperties, m_propertyDependencies);

    const EvaluationData &data = evaluationData(item);
    if (m_cacheEnabled && data.cache.contains(name)) {
        QJSValue result = data.cache[name];
        return result;
    }

    const auto &decl = item->propertyDeclaration(name);
    QJSValue result = evaluateValue(v, item, name, decl, itemOfProperty);
    if (m_errors.hasError())
        return QJSValue();

    if (!isExclusiveListValue(v)) {
        // TODO This is a suboptimal implementation, but the code seems to be hardly used
        for (auto nv = v->next(); nv; nv = nv->next()) {
            QBS_CHECK(result.isArray() || result.isUndefined());

            QJSValue appendix = evaluateValue(nv, nv->definingItem(), name, decl, itemOfProperty);
            if (m_errors.hasError())
                return QJSValue();

            if (isExclusiveListValue(nv))
                return appendix;

            if (appendix.isUndefined())
                continue;

            if (result.isUndefined())
                result = m_scriptEngine->newArray();

            quint32 resultLength = result.property(QLatin1String("length")).toUInt();
            quint32 appendixLength = appendix.property(QLatin1String("length")).toUInt();
            quint32 mergedLength = resultLength + appendixLength;
            QJSValue merged = engine()->newArray(mergedLength);

            for (quint32 i = 0; i < resultLength; ++i)
                merged.setProperty(i, result.property(i));

            for (quint32 i = 0; i < appendixLength; ++i)
                merged.setProperty(i + resultLength, appendix.property(i));

            result = merged;
        }
    }

    if (m_cacheEnabled)
        data.cache.insert(name, result);

    return result;

}

QJSValue Evaluator::evaluateValue(ValueConstPtr value, const Item *item, const QString &name,
                                  const PropertyDeclaration &decl, const Item *propertyLookupItem)
{
    QJSValue result;
    const ErrorGuard errorGuard(this);

    if (value->type() == Value::VariantValueType) {
        const QVariant vv = std::static_pointer_cast<const VariantValue>(value)->value();
        result = engine()->toScriptValue(vv);
        convertToPropertyType(result, item, decl, value->location());
        if (m_errors.hasError())
            return QJSValue();

    } else if (value->type() == Value::JSSourceValueType) {
        const JSSourceValueConstPtr jsv = std::static_pointer_cast<const JSSourceValue>(value);

        result = evaluateJSSourceValue(jsv, item, name, decl, propertyLookupItem);
        if (m_errors.hasError())
            return QJSValue();

        for (const auto &alternative: jsv->alternatives()) {

            // TODO: This is different from the QtScript implementation. We should not attempt
            //       to evaluate property blocks for non-instantiated modules.
            if (item->type() == ItemType::ModuleInstance
                    && !item->hasProperty(StringConstants::presentProperty())) {
                break;
            }

            static const PropertyDeclaration conditionDecl(StringConstants::conditionProperty(),
                                                           PropertyDeclaration::Boolean);
            static const PropertyDeclaration overrideDecl(
                            StringConstants::overrideListPropertiesProperty(),
                            PropertyDeclaration::Boolean);
            static const JSSourceValuePtr fakeValue = JSSourceValue::create();
            fakeValue->setFile(alternative.value->file());
            fakeValue->setDefiningItem(alternative.value->definingItem());

            const auto &condition = alternative.condition;
            fakeValue->setLocation(condition.location.line(), condition.location.column());
            fakeValue->setSourceCode(QStringRef(&condition.value));
            QJSValue conditionValue = evaluateJSSourceValue(fakeValue,
                                                            item,
                                                            StringConstants::conditionProperty(),
                                                            conditionDecl,
                                                            propertyLookupItem);
            if (m_errors.hasError())
                return QJSValue();

            if (!conditionValue.toBool())
                continue;

            const auto &override = alternative.overrideListProperties;
            fakeValue->setLocation(override.location.line(), override.location.column());
            fakeValue->setSourceCode(QStringRef(&override.value));
            QJSValue overrideValue =
                    evaluateJSSourceValue(fakeValue,
                                          item,
                                          StringConstants::overrideListPropertiesProperty(),
                                          overrideDecl,
                                          propertyLookupItem);
            if (m_errors.hasError())
                return QJSValue();

            // Properties blocks do not use item->outerItem() to get the outer value.
            // Instead, above result is used as "outer".
            QJSValue *outerValue = &result;

            // TODO: Ugly workaround for tst_language::propertiesBlockInGroup.
            //       Looks like jsv in that case is "base" for whatever reason.
            //       But that doesn't make any sense because the value we are
            //       interested in, is the outer one.
            if (jsv->sourceUsesBase() && !jsv->baseValue() && item->outerItem()) {
                *outerValue = evaluateProperty(item->outerItem(), name);
                if (m_errors.hasError())
                    return QJSValue();
            }

            // TODO: Use definingItem instead of item?
            result = evaluateJSSourceValue(alternative.value, item, name, decl, propertyLookupItem,
                                           outerValue);
            if (m_errors.hasError())
                return QJSValue();

            // TODO: We should not modify the value, but store that information somewhere else.
            if (overrideValue.toBool())
                std::const_pointer_cast<JSSourceValue>(jsv)->setIsExclusiveListValue();

            break;
         }

    } else if (value->type() == Value::ItemValueType) {
        const ItemValueConstPtr iv = std::static_pointer_cast<const ItemValue>(value);
        result = evaluationData(iv->item()).itemProxy;
    }

    return result;
}

QJSValue Evaluator::evaluateJSSourceValue(JSSourceValueConstPtr sourceValue,
                                          const Item *item,
                                          const QString &name,
                                          const PropertyDeclaration &decl,
                                          const Item *itemOfValue,
                                          const QJSValue *outerValue)
{

    if (sourceValue->sourceCode() == QStringLiteral("undefined"))
        return QJSValue();

    QMap<QString, QJSValue> convenienceProperties;
    // This method is likely stacked, but only the outermost call instance is allowed to throw
    // in order to not corrupt QJSEngine's state.
    const ErrorGuard errorGuard(this);

//    if (decl.type() == PropertyDeclaration::Script) {
//        if (sourceValue->hasFunctionForm())
//            return sourceValue->sourceCode().toString();
//        else if (!sourceValue->sourceCode().isEmpty())
//            return QStringLiteral("{ %1; }").arg(sourceValue->sourceCode());
//        else
//            return QJSValue();
//    }

    ImportHelper *importer = ImportHelper::instance(engine());
    Q_ASSERT(importer);
    const ImportHelper::FileContextScope *fileContext =
            importer->fileContextScope(sourceValue->file());

    if (sourceValue->sourceUsesBase()) {
        QJSValue baseValue;
        auto baseJsv = sourceValue->baseValue();
        if (baseJsv) {
            // TODO: This is not done in the QtScript implementation, but it is necessary
            //       when evaluating module properties referring to base inside Export items
            QBS_CHECK(!baseJsv->definingItem()
                      || baseJsv->definingItem() == sourceValue->definingItem());
            baseJsv->setDefiningItem(sourceValue->definingItem());

            baseValue = evaluateValue(baseJsv, item, name, decl, itemOfValue);
            if (m_errors.hasError())
                return QJSValue();

        }
        baseValue = sanitizeUndefinedValue(decl, baseValue);
        convenienceProperties.insert(StringConstants::baseVar(), baseValue);
    }

    if (item->parent()) {
        auto *handler = new ParentProxyHandler(item->parent(), this);
        convenienceProperties.insert(QStringLiteral("parent"), engine()->newProxyObject(handler));
    }

    if (sourceValue->sourceUsesOriginal()) {
        QJSValue originalResult;
        if (decl.isScalar()) {
            const Item *original = item;
            while (original->type() == ItemType::ModuleInstance)
                original = original->prototype();
            if (original->type() != ItemType::Module && original->type() != ItemType::Export) {
                const QString errorMessage = Tr::tr("The special value 'original' can only "
                                                    "be used with module properties.");
                throwError(errorMessage, sourceValue->location());
                return QJSValue();
            }
            const ValueConstPtr ov = original->property(name);

            // This can happen when resolving shadow products. The error will be ignored
            // in that case.
            if (!ov) {
                const QString errorMessage = Tr::tr("Error setting up 'original'.");
                throwError(errorMessage, sourceValue->location());
                return QJSValue();
            }

            if (ov->type() == Value::JSSourceValueType) {
                const JSSourceValueConstPtr ojsv =
                        std::static_pointer_cast<const JSSourceValue>(ov);
                // TODO: THis is not necessarily a JS value
                if (ojsv->sourceUsesOriginal()) {
                    const QString errorMessage = Tr::tr("The special value 'original' cannot "
                            "be used on the right-hand side of a property declaration.");
                    throwError(errorMessage, ojsv->location());
                    return QJSValue();
                }
            }

            // Evaluate the original value in the original context, but further property lookup
            originalResult = evaluateValue(ov, item, name, decl, original);
            // TODO We should return here, shouldn't we
            //            if (m_errors.hasError())
            //                return QJSValue();

        } else {
            originalResult = engine()->newArray(0);
        }
        originalResult = sanitizeUndefinedValue(decl, originalResult);
        convenienceProperties.insert(StringConstants::originalVar(), originalResult);
    }

    if (sourceValue->sourceUsesOuter()) {
        if (outerValue) {
            convenienceProperties.insert(StringConstants::outerVar(),
                                         sanitizeUndefinedValue(decl, *outerValue));
        } else if (item->outerItem()) {
            QJSValue outerValue = evaluateProperty(item->outerItem(), name);
            // TODO We should return here, shouldn't we
    //        if (m_errors.hasError())
    //            return QJSValue();
            convenienceProperties.insert(StringConstants::outerVar(),
                                         sanitizeUndefinedValue(decl, outerValue));
        }
    }

    QList<const Item *> scopeList;
    if (sourceValue->definingItem()) {
        for (const Item *i = sourceValue->definingItem()->scope(); i; i = i->scope()) {
            scopeList << i;
            if (i->parent())
                scopeList << i->parent();
        }
    }
    if (itemOfValue->type() != ItemType::ModuleInstance) {
        // Own properties of module instances must not have the instance itself
        // and its parent in the scope.
        scopeList << item;
        if (item->parent())
            scopeList << item->parent();
    }
    for (const Item *scope = item->scope(); scope; scope = scope->scope()) {
        scopeList << scope;
        if (scope->parent())
            scopeList << scope->parent();
    }

    if (const auto idScope = sourceValue->file()->idScope()) {
        scopeList << idScope;
        if (idScope->parent())
            scopeList << idScope->parent();
    }

    QJSValue result;
    if (decl.type() == PropertyDeclaration::Variant
            || decl.type() == PropertyDeclaration::VariantList) {
        // Variant properties may be functions which are later executed in a different
        // context. The function is bound to the proxy during evaluation and thus,
        // the proxy needs stays alive. We cannot re-use the global proxy here because
        // the global proxy is reconfigured every time.
        auto proxy = new UnqualifiedLookupProxyHandler(this);
        // The resulting evaluation prefix must have a fixed length because
        // if the result is a function, we need to extract the source code.
        QString proxyName = QStringLiteral("__jsvalueproxy_%1")
                .arg((uint64_t)proxy, 16, 16, QLatin1Char('0'));
        QJSValue proxyValue = engine()->newProxyObject(engine()->newObject(), proxy);
        engine()->globalObject().setProperty(proxyName, proxyValue);

        const QString prefix = QStringLiteral("with(%1){").arg(proxyName);
        static const QString suffix = QStringLiteral("}");
        QString sourceCodeForEvaluation = prefix % sourceValue->sourceCodeForEvaluation() % suffix;
        const auto *config = new UnqualifiedLookupProxyHandler::Config{
                fileContext,
                new QList<const Item *>(scopeList),
                new QMap<QString, QJSValue>(convenienceProperties)};
        proxy->setConfiguration(config);
        result = evaluateCode(sourceCodeForEvaluation, sourceValue->location());

    } else {
        static const QString prefix = QStringLiteral("with(__jsvalueproxy){");
        static const QString suffix = QStringLiteral("}");
        QString sourceCodeForEvaluation = prefix % sourceValue->sourceCodeForEvaluation() % suffix;

        const auto oldConfig = m_unqualifiedLookupProxy->configuration();
        const auto newConfig =
                UnqualifiedLookupProxyHandler::Config{ fileContext,
                                                       &scopeList,
                                                       &convenienceProperties };
        m_unqualifiedLookupProxy->setConfiguration(&newConfig);
        auto cleanup = qScopeGuard(
                    [this, oldConfig](){ m_unqualifiedLookupProxy->setConfiguration(oldConfig); });
        result = evaluateCode(sourceCodeForEvaluation, sourceValue->location());
    }

    convertToPropertyType(result, item, decl, sourceValue->location());
    return result;
}

void Evaluator::evaluateConfigureScript(const Item *probeItem, JSSourceValueConstPtr sourceValue,
                                        QMap<QString, QJSValue> *properties)
{
    const ErrorGuard errorGuard(this);
    ImportHelper *importHelper = ImportHelper::instance(engine());

    static const QString prefix = QStringLiteral("with(__probescriptproxy){");
    static const QString suffix = QStringLiteral("}");
    QString sourceCodeForEvaluation = prefix % sourceValue->sourceCodeForEvaluation() % suffix;

    m_probeScriptProxy->init(probeItem, importHelper->fileContextScope(sourceValue->file()),
                             properties);
    auto cleanup = qScopeGuard(
                [this](){ m_probeScriptProxy->init(nullptr, nullptr, nullptr); });
    evaluateCode(sourceCodeForEvaluation, sourceValue->location());
}

QJSValue Evaluator::evaluateCode(const QString &code, const CodeLocation &location)
{
    ErrorGuard guard(this);
    QStringList exceptionStack;

    ++m_nestLevel;
    QJSValue result = engine()->evaluate(code, location.filePath(), location.line(),
                                         location.column(), &exceptionStack);
    --m_nestLevel;

    // TODO: Put into the script engine when moving the evaluate() function
    //       This is not recoverable.
    if (engine()->isInterrupted())
        throw ScriptEngine::CancelException();

    if (result.isError()) {
        const QString fileName =
                QUrl(result.property(QStringLiteral("fileName")).toString()).path();
        int lineNumber = result.property(QStringLiteral("lineNumber")).toInt();
        const QString message = result.property(QStringLiteral("message")).toString();
        CodeLocation errorLocation(fileName, lineNumber, 0, false);
        throwError(message, errorLocation);
    }

    if (!exceptionStack.isEmpty()) {
        const QString frame = exceptionStack.first();
        QRegularExpression reg(
                    QStringLiteral(":(?<line>\\d+):(?<column>-?\\d+):(?<url>file://.*)"));
        const auto match = reg.match(frame);
        QBS_CHECK(match.hasMatch());
        const QString fileName = QUrl(match.captured(QStringLiteral("url"))).path();
        int lineNumber = match.captured(QStringLiteral("line")).toInt();
        const QString message = result.toString();
        CodeLocation errorLocation(fileName, lineNumber, 0, false);
        throwError(message, errorLocation);

    }

    return result;
}

// In order to avoid lots of checks in user code, convenience properties of
// array-type must not be undefined.
QJSValue Evaluator::sanitizeUndefinedValue(const PropertyDeclaration &decl,
                                           const QJSValue &value)
{
    const auto type = decl.type();
    // TODO: If Variant would not be there, would simply use decl.isScalar()
    const bool isArray = type == PropertyDeclaration::StringList
            || type == PropertyDeclaration::PathList
            || type == PropertyDeclaration::Variant
            || type == PropertyDeclaration::VariantList;

    if (isArray && value.isUndefined())
        return engine()->newArray();
    else
        return value;
}

void Evaluator::throwError(const QString &description, CodeLocation location)
{
    if (m_nestLevel > 0) {
        engine()->throwError(description);
    } else {
        m_errors.append(description, location);
    }
}

void Evaluator::convertToPropertyType(QJSValue &v,
                                      const Item *item,
                                      const PropertyDeclaration &decl,
                                      const CodeLocation &loc)
{
    if (v.isError())
        return;

    if (v.isUndefined())
        return;

    bool isPath = false;

    const auto actualBaseDir = [this, &item]() {
        QBS_CHECK(item);
        if (const auto v = item->variantProperty(StringConstants::qbsSourceDirPropertyInternal())) {
            return v->value().toString();
        } else if (!m_pathPropertiesBaseDir.isEmpty()) {
            if (const auto v = item->variantProperty(StringConstants::sourceDirectoryProperty()))
                return v->value().toString();
        }
        return m_pathPropertiesBaseDir;
    };

    switch (decl.type()) {
//    case PropertyDeclaration::Script:
    case PropertyDeclaration::UnknownType:
    case PropertyDeclaration::Variant:
        break;
    case PropertyDeclaration::Boolean:
        if (!v.isBool())
            v = v.toBool();
        break;
    case PropertyDeclaration::Integer:
        if (!v.isNumber()) {
            throwError(QStringLiteral("Type Error: Value assigned to property '%1' does not have type '%2'.")
                                   .arg(decl.name(), decl.typeString()), loc);
        }
        break;
    case PropertyDeclaration::Path:
        isPath = true;
        // Fall-through.
    case PropertyDeclaration::String:
        if (!v.isString()) {
            throwError(QStringLiteral("Type Error: Value assigned to property '%1' does not have type '%2'.")
                                   .arg(decl.name(), decl.typeString()), loc);
        }
        else if (isPath) {
            const QString baseDir = actualBaseDir();
            if (!baseDir.isEmpty())
                v = QDir::cleanPath(FileInfo::resolvePath(baseDir, v.toString()));
        }
        break;
    case PropertyDeclaration::PathList:
        isPath = true;
        // Fall-through.
    case PropertyDeclaration::StringList:
    {
        quint32 length;
        // TODO For QStringList VariantValue converted to QJSValue
        //          isVariant() returns false and isObject() returns true.
        //          They do have a length property, but they are not arrays.
        //          This might confuse other functions, so we convert them
        //          into proper JS arrays here.
        // TODO Try harder to avoid copying
        const bool hasLengthProperty = v.hasProperty(StringConstants::lengthProperty());
        if (hasLengthProperty)
            length = v.property(StringConstants::lengthProperty()).toUInt();
        else
            length = 1;

        if (v.isObject() && hasLengthProperty) {
            QJSValue sv = v;
            v = engine()->newArray(length);
            for (quint32 i = 0; i < length; ++i)
                v.setProperty(i, sv.property(i));

        } else if (!v.isArray()) {
            QJSValue sv = v;
            v = engine()->newArray(1);
            v.setProperty(0, sv);
        }

        const QString baseDir = actualBaseDir();
        for (quint32 i = 0; i < length; ++i) {
            QJSValue elem = v.property(i);
            if (elem.isUndefined()) {
                throwError(Tr::tr("Element at index %1 of list property '%2' is undefined."
                                   " String expected.").arg(i).arg(decl.name()), loc);
                break;
            }
            if (elem.isNull()) {
                throwError(Tr::tr("Element at index %1 of list property '%2' is null. "
                                   "String expected.").arg(i).arg(decl.name()), loc);
                break;
            }
            if (!elem.isString()) {
                throwError(Tr::tr("Element at index %1 of list property '%2' does not have"
                                   " string type.").arg(i).arg(decl.name()), loc);
                break;
            }
            if (isPath && !baseDir.isEmpty()) {
                elem = QDir::cleanPath(FileInfo::resolvePath(baseDir, elem.toString()));
                v.setProperty(i, elem);
            }
        }
        break;
    }
    case PropertyDeclaration::VariantList:
        if (!v.isArray()) {
            QJSValue x = engine()->newArray(1);
            x.setProperty(0, v);
            v = x;
        }
        break;
    }
}

QVariantMap Evaluator::itemToVariantMap(const Item *item)
{
    QVariantMap result;
    const auto properties = item->properties();
    for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) {
        const auto &name = it.key();
        QJSValue value = evaluateProperty(item, name);
        // Objects can be all kind of things: stringLists,
        // plain objects, but also items wrapped by proxies.
        if (value.isObject()) {
            Item *wrappedItem = itemFromValue(value);
            if (wrappedItem)
                result[name] = itemToVariantMap(wrappedItem);
            else
                result[name] = value.toVariant();
        } else {
            result[name] = value.toVariant();
        }
    }
    return result;
}

Item *Evaluator::itemFromValue(const QJSValue &value)
{
    QBS_CHECK(value.isObject());
    QBS_CHECK(sizeof(value) == sizeof(quintptr));

    // QJSValue contains a pointer to another pointer. The value of the
    // latter is persistent and can be used as an identifier.
    quintptr valueId = **reinterpret_cast<quintptr*const*>(&value);
    Item *item = m_itemMap.value(valueId, nullptr);
    return item;
}

void Evaluator::setCachingEnabled(bool enabled)
{
    m_cacheEnabled = enabled;
}

void Evaluator::setProperty(Item *item, const QString &name, const QJSValue &value)
{
    QJSValue jsValue = value;
    convertToPropertyType(jsValue, item, item->propertyDeclaration(name));
    item->setProperty(name, VariantValue::create(jsValue.toVariant()));
}

PropertyDependencies Evaluator::propertyDependencies() const
{
    return m_propertyDependencies;
}

void Evaluator::clearPropertyDependencies()
{
    m_propertyDependencies.clear();
}

} // namespace Internal
} // namespace qbs

#include <evaluator.moc>
