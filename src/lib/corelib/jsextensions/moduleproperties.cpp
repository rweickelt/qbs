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

#include "moduleproperties.h"

#include <buildgraph/artifact.h>
#include <buildgraph/artifactsscriptvalue.h>
#include <buildgraph/dependencyparametersscriptvalue.h>
#include <language/language.h>
#include <language/propertymapinternal.h>
#include <language/qualifiedid.h>
#include <language/scriptengine.h>
#include <logging/translator.h>
#include <tools/error.h>
#include <tools/qbsassert.h>
#include <tools/qttools.h>
#include <tools/stlutils.h>
#include <tools/stringconstants.h>

namespace qbs {
namespace Internal {

void ModuleProperties::setModuleScriptValue(QJSValue &targetObject,
        const QJSValue &moduleObject, const QString &moduleName, ScriptEngine *engine)
{
    const QualifiedId name = QualifiedId::fromString(moduleName);
    QJSValue obj = targetObject;
    for (int i = 0; i < name.size() - 1; ++i) {
        QJSValue tmp = obj.property(name.at(i));
        if (!tmp.isObject())
            tmp = engine->newObject();
        obj.setProperty(name.at(i), tmp);
        obj = tmp;
    }
    obj.setProperty(name.last(), moduleObject);
    if (moduleName.size() > 1)
        targetObject.setProperty(moduleName, moduleObject);
}

} // namespace Internal
} // namespace qbs
