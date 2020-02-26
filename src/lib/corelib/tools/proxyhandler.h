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
#ifndef QBS_PROXYHANDLER_H
#define QBS_PROXYHANDLER_H

#include <language/forward_decls.h>
#include <tools/qbsassert.h>

#include <QtCore/qobject.h>
#include <QtQml/qjsvalue.h>
#include <QtQml/qjsvalueiterator.h>
#include <QtCore/qdebug.h>

namespace qbs {
namespace Internal {
class Artifact;
class ResolvedModule;
class ScriptEngine;

// TODO: Merge with DefaultProxyHandler
class AbstractProxyHandler: public QObject {
Q_OBJECT
public:
    virtual ~AbstractProxyHandler();
    Q_INVOKABLE virtual QJSValue get(QJSValue target, const QJSValue &key,
                                     const QJSValue &receiver) = 0;
    Q_INVOKABLE virtual bool has(const QJSValue &target, const QJSValue &key);
    Q_INVOKABLE virtual bool set(QJSValue target, const QJSValue &key, const QJSValue &value,
                                 const QJSValue &receiver);

    static bool isProxiedObject(const QJSValue &value);

protected:
    QJSValue defaultGetOwnPropertyDescriptor(QJSValue target, const QJSValue &key);
    ScriptEngine *engine() const;
    static QStringList getPropertyNames(const QJSValue &object);
    QJSValue reflectGet(const QJSValue &target, const QJSValue &key, const QJSValue receiver);

private:
    QJSValue m_reflect;
    QJSValue m_reflectGet;
};

class DefaultProxyHandler: public AbstractProxyHandler {
Q_OBJECT
public:

    QJSValue get(QJSValue target, const QJSValue &key, const QJSValue &receiver) override;
    Q_INVOKABLE virtual QStringList ownKeys(const QJSValue &target);
    Q_INVOKABLE QJSValue getOwnPropertyDescriptor(QJSValue target, const QJSValue &key) {
        return defaultGetOwnPropertyDescriptor(target, key);
    }
};

class ModuleProxyHandler: public DefaultProxyHandler {
    Q_OBJECT
public:
    QJSValue get(QJSValue target, const QJSValue &key, const QJSValue &receiver) override;
    QStringList ownKeys(const QJSValue &target) override;

public:
    static void init(ScriptEngine *engine, QJSValue &target, const ResolvedModule *module,
                     const Artifact *artifact = nullptr);
    static QJSValue create(ScriptEngine *engine, const ResolvedModule *module,
                     const Artifact *artifact = nullptr);
    static QJSValue create(ScriptEngine *engine, QJSValue &target, const ResolvedModule *module,
                     const Artifact *artifact = nullptr);

private:
    ModuleProxyHandler();

    const Artifact *artifact(const QJSValue &target) const;
    const ResolvedModule *module(const QJSValue &target) const;
    const QJSValue dependencyParameters(const QJSValue &target) const;
    QJSValue dependenciesValue(const ResolvedModule *module) const;
};

} // namespace Internal
} // namespace qbs

#endif // QBS_PROXYHANDLER_H
