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

#include "jsextensions.h"

#include <language/scriptengine.h>
#include <QtCore/qlogging.h>
#include <QtCore/qobject.h>
#include <QtQml/qjsvalue.h>

namespace qbs {
namespace Internal {

class ConsoleJsExtension : public JsExtension
{
    Q_OBJECT
public:
    Q_INVOKABLE void debug(const QString &value);
    Q_INVOKABLE void error(const QString &value);
    Q_INVOKABLE void info(const QString &value);
    Q_INVOKABLE void log(const QString &value);
    Q_INVOKABLE void warn(const QString &value);
};

void ConsoleJsExtension::debug(const QString &value)
{
    engine()->logger().qbsDebug() << value;
}

void ConsoleJsExtension::error(const QString &value)
{
    engine()->logger().qbsLog(LoggerError) << value;
}

void ConsoleJsExtension::info(const QString &value)
{
    engine()->logger().qbsInfo() << value;
}

void ConsoleJsExtension::log(const QString &value)
{
    engine()->logger().qbsDebug() << value;
}

void ConsoleJsExtension::warn(const QString &value)
{
    engine()->logger().qbsWarning() << value;
}

QJSValue createConsoleJsExtension(QJSEngine *engine)
{
    return engine->newQObject(new ConsoleJsExtension());
}

QBS_REGISTER_JS_EXTENSION("console", createConsoleJsExtension)

}
}

#include "consoleextension.moc"
