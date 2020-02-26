/****************************************************************************
**
** Copyright (C) 2020 Richard Weickelt
** Contact: richard@weickelt.de
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

#ifndef IMPORTHELPER_H
#define IMPORTHELPER_H

#include "jsextensions.h"

#include <language/filecontext.h>

#include <stack>

#include <QtCore/qhash.h>
#include <QtCore/qstring.h>
#include <QtCore/qstringlist.h>
#include <QtQml/qjsvalue.h>

namespace qbs {
namespace Internal {

class Evaluator;
class ScriptImporter;

class ImportHelper : public JsExtension
{
    Q_OBJECT
public:
    struct FileContextScope
    {
        bool initialized = false;
        QMap<QString, QJSValue> imports;
        QMap<QString, QJSValue> convenienceProperties;
    };

    ImportHelper(ScriptEngine *engine);
    ~ImportHelper();

    const FileContextScope *fileContextScope(const FileContextBaseConstPtr &fileCtx);
    QString scopeNameForFilepath(const QString &filePath) const;

    void setEvaluator(Evaluator *evaluator);
    Q_INVOKABLE QJSValue requireExtension(const QString &moduleName);
    Set<QString> importedFiles() const;

    static ImportHelper *instance(ScriptEngine *engine);
    static const ImportHelper *instance(const ScriptEngine *engine);

    qint64 elapsedTime() const;
    void setProfilingEnabled(bool enabled);

private:
    QJSValue wrapInProxy(const QJSValue &object, const QStringList &filePaths);

    void importJsFile(const QString &path, QJSValue &targetObject);
    QJSValue mergeExtensionObjects(const QJSValueList &lst);

    Evaluator *m_evaluator = nullptr;
    QHash<FileContextBaseConstPtr, FileContextScope> m_fileContextScopesMap;
    QHash<QString, QString> m_filePathScopeNameMap;
    QHash<QString, QJSValue> m_jsFileCache;
    std::stack<QString> m_currentDirPathStack;
    std::stack<QStringList> m_extensionSearchPathsStack;
    std::stack<FileContextScope *> m_fileContextScopeStack;
    qint64 m_elapsedTime = -1;
    ScriptImporter *m_scriptImporter;

//    Logger &m_logger;

};

} // namespace Internal
} // namespace qbs

#endif // Include guard.
