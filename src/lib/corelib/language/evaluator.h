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

#ifndef QBS_EVALUATOR_H
#define QBS_EVALUATOR_H

#include "evaluationdata.h"
#include "forward_decls.h"
#include "itemobserver.h"
#include "qualifiedid.h"

#include <jsextensions/importhelper.h>

#include <QtCore/qhash.h>
#include <QtQml/qjsvalue.h>
#include <QtQml/qqmlerror.h>

#include <functional>
#include <stack>

namespace qbs {
namespace Internal {
class FileTags;
class UnqualifiedLookupProxyHandler;
class Logger;
class ProbeScriptProxyHandler;
class PropertyDeclaration;
class ScriptEngine;

class QBS_AUTOTEST_EXPORT Evaluator : private ItemObserver
{
    friend class ErrorGuard;
    friend class ItemProxyHandler;
    friend class ParentProxyHandler;
    friend class UnqualifiedLookupProxyHandler;
    friend class ProbeScriptProxyHandler;

public:
    Evaluator(ScriptEngine *scriptEngine);

    void clearCache(const Item *item, const QString &name = QString());
    ScriptEngine *engine() const { return m_scriptEngine; }
    EvaluationData &evaluationData(const Item *item);

    QJSValue property(const Item *item, const QString &name) { return value(item, name); }
    QJSValue value(const Item *item, const QString &name, bool *propertySet = nullptr);
    bool boolValue(const Item *item, const QString &name, bool *propertyWasSet = nullptr);
    int intValue(const Item *item, const QString &name, int defaultValue = 0,
                 bool *propertyWasSet = nullptr);
    FileTags fileTagsValue(const Item *item, const QString &name, bool *propertySet = nullptr);
    QJSValue scriptValue(const Item *item);
    QString stringValue(const Item *item, const QString &name,
                        const QString &defaultValue = QString(), bool *propertyWasSet = nullptr);
    QStringList stringListValue(const Item *item, const QString &name,
                                bool *propertyWasSet = nullptr);

    QVariantMap itemToVariantMap(const Item *item);
    Item *itemFromValue(const QJSValue& value);

    void setCachingEnabled(bool enabled);
    void setProperty(Item *item, const QString &name, const QJSValue &value);

    PropertyDependencies propertyDependencies() const;
    void clearPropertyDependencies();

    void handleEvaluationError(const Item *item, const QString &name,
            const QJSValue &scriptValue);

    void setPathPropertiesBaseDir(const QString &dirPath);
    void clearPathPropertiesBaseDir();

    bool isNonDefaultValue(const Item *item, const QString &name) const;

    void evaluateConfigureScript(const Item *probeItem, JSSourceValueConstPtr source,
                                 QMap<QString, QJSValue> *properties);

private:
    QJSValue createItemProxy(const Item *item);

    QString mergeJSCollection(const QStringList &filePaths);

    void onItemPropertyChanged(Item *item) override;

    bool evaluateProperty(QJSValue *result,
                          const Item *item,
                          const QString &name,
                          bool *propertyWasSet);

    QJSValue evaluateProperty(const Item *item,
                              const QString &name);

    QJSValue evaluateValue(ValueConstPtr value,
                           const Item *item,
                           const QString &name,
                           const PropertyDeclaration &decl,
                           const Item *propertyLookupItem);

    QJSValue evaluateJSSourceValue(JSSourceValueConstPtr sourceValue,
                             const Item *item,
                             const QString &name,
                             const PropertyDeclaration &decl,
                             const Item *propertyLookupItem,
                             const QJSValue *outerValue = nullptr);

    QJSValue evaluateCode(const QString &code, const CodeLocation &location);


    void convertToPropertyType(QJSValue &value,
                               const Item *item,
                               const PropertyDeclaration &decl,
                               const CodeLocation &loc = CodeLocation());


    QJSValue sanitizeUndefinedValue(const PropertyDeclaration &decl, const QJSValue &value);

    void throwError(const QString & description, CodeLocation location = CodeLocation());

    ScriptEngine *m_scriptEngine;
    mutable QHash<const Item *, EvaluationData> m_evaluationDataMap;
    mutable QHash<quintptr, Item *> m_itemMap;
    QString m_pathPropertiesBaseDir;
    int m_nestLevel = 0;
    ErrorInfo m_errors;
    bool m_cacheEnabled = false;
    PropertyDependencies m_propertyDependencies;
    std::stack<QualifiedId> m_requestedProperties;
    UnqualifiedLookupProxyHandler *m_unqualifiedLookupProxy;
    ProbeScriptProxyHandler *m_probeScriptProxy;
};

class EvalCacheEnabler
{
public:
    EvalCacheEnabler(Evaluator *evaluator) : m_evaluator(evaluator)
    {
        m_evaluator->setCachingEnabled(true);
    }

    ~EvalCacheEnabler() { m_evaluator->setCachingEnabled(false); }

private:
    Evaluator * const m_evaluator;
};

} // namespace Internal
} // namespace qbs

#endif // QBS_EVALUATOR_H
