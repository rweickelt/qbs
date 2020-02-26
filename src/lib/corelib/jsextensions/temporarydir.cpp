/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Copyright (C) 2015 Jake Petroules.
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

#include <QtCore/qfileinfo.h>
#include <QtCore/qobject.h>
#include <QtCore/qtemporarydir.h>
#include <QtCore/qvariant.h>

#include <QtQml/qjsvalue.h>

namespace qbs {
namespace Internal {

static bool tempDirIsCanonical()
{
#if QT_VERSION >= 0x050c00
    return true;
#endif
    return false;
}

class TemporaryDirExtension : public JsExtension
{
    Q_OBJECT
public:
    Q_INVOKABLE TemporaryDirExtension(QObject *engine);
    Q_INVOKABLE bool isValid() const;
    Q_INVOKABLE QString path() const;
    Q_INVOKABLE bool remove();

private:
    QTemporaryDir dir;
};

TemporaryDirExtension::TemporaryDirExtension(QObject *engine) {
    auto *se = qobject_cast<ScriptEngine *>(engine);
    Q_ASSERT(se);
    const DubiousContextList dubiousContexts({
            DubiousContext(EvalContext::PropertyEvaluation, DubiousContext::SuggestMoving)
    });
    se->checkContext(QStringLiteral("qbs.TemporaryDir"), dubiousContexts);
    dir.setAutoRemove(false);
}

bool TemporaryDirExtension::isValid() const {
    return dir.isValid();
}

QString TemporaryDirExtension::path() const {
    return tempDirIsCanonical() ? dir.path() : QFileInfo(dir.path()).canonicalFilePath();
}

bool TemporaryDirExtension::remove() {
    return dir.remove();
}

QJSValue createTemporaryDirExtension(QJSEngine *engine)
{
    QJSValue mo = engine->newQMetaObject(&TemporaryDirExtension::staticMetaObject);
    QJSValue factory = engine->evaluate(
                QStringLiteral("(function(m, e){ return function(){ return new m(e); } })"));
    QJSValue wrapper = factory.call(QJSValueList{mo, engine->toScriptValue(engine)});
    return wrapper;
}

QBS_REGISTER_JS_EXTENSION("TemporaryDir", createTemporaryDirExtension)

}
}

#include "temporarydir.moc"
