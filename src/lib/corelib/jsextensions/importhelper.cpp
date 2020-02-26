#include "importhelper.h"
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

#include <language/scriptengine.h>
#include <language/scriptimporter.h>
#include <logging/translator.h>
#include <tools/error.h>
#include <tools/fileinfo.h>
#include "tools/profiling.h"
#include <tools/proxyhandler.h>
#include "tools/stringconstants.h"

#include <QtCore/qdiriterator.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qobject.h>
#include <QtCore/qtemporaryfile.h>
#include <QtQml/qjsvalue.h>
#include <QtQml/qjsvalueiterator.h>

namespace qbs {
namespace Internal {

namespace {
    QString requireCode(QStringLiteral("\
function require(resource) {\
    var result = ImportHelper.requireExtension(resource);\
    return result;\
}\
\
function loadExtension(name) {\
    console.warn(\"The loadExtension() function is deprecated and will be\" +\
                 \"removed in a future version of Qbs. Use require() instead.\");\
    return require(name);\
}\
\
function loadFile(name) {\
   console.warn(\"The loadFile() function is deprecated and will be\" +\
                \"removed in a future version of Qbs. Use require() instead.\");\
   return require(name);\
}\
"));
}


class ImportProxyHandler: public AbstractProxyHandler {
    Q_OBJECT
public:
    ImportProxyHandler(const QStringList &filePaths) : m_filePaths(filePaths)
    {
    }

    Q_INVOKABLE bool has(const QJSValue &target, const QJSValue &key) override
    {
        Q_UNUSED(target);
        const auto name = key.toString();
        return target.hasProperty(name);
    }

    Q_INVOKABLE QJSValue get(QJSValue target, const QJSValue &key,
                             const QJSValue &receiver) override
    {
        Q_UNUSED(receiver);
        const auto name = key.toString();
        if (!key.isString()) {
            return QJSValue();
        }

        QJSValue property = target.property(name);
        // Properties that are the result of require() in js files do not count
        // as property accesses in the current import scope.
        if (!property.hasOwnProperty(StringConstants::importScopeNamePropertyInternal())) {
            for (const auto &filePath: m_filePaths)
                engine()->addImportedFileUsedInScript(filePath);
        }
        return property;
    }

private:
    const QStringList m_filePaths;
};


static QString findExtensionDir(const QStringList &searchPaths, const QString &extensionPath)
{
    for (const QString &searchPath : searchPaths) {
        const QString dirPath = searchPath + QStringLiteral("/imports/") + extensionPath;
        QFileInfo fi(dirPath);
        if (fi.exists() && fi.isDir())
            return dirPath;
    }
    return {};
}

QJSValue ImportHelper::mergeExtensionObjects(const QJSValueList &lst)
{
    QJSValue result = engine()->newObject();
    for (const QJSValue &v : lst) {
        if (result.isError()) {
            result = v;
            continue;
        }
        QJSValueIterator svit(v);
        while (svit.hasNext()) {
            svit.next();
            result.setProperty(svit.name(), svit.value());
        }
    }
    return result;
}


ImportHelper::ImportHelper(ScriptEngine *engine):
    m_scriptImporter(new ScriptImporter(engine))
{
}

ImportHelper::~ImportHelper()
{
    delete (m_scriptImporter);
}

qint64 ImportHelper::elapsedTime() const
{
    return m_elapsedTime;
}

void ImportHelper::importJsFile(const QString &filePath, QJSValue &targetObject)
{
    AccumulatingTimer importTimer(m_elapsedTime != -1 ? &m_elapsedTime : nullptr);
    QJSValue &evaluationResult = m_jsFileCache[filePath];
    // TODO Copy properties instead?
    if (!evaluationResult.isUndefined()) {
        ScriptImporter::copyProperties(evaluationResult, targetObject);
        return;
    }

    QFile file(filePath);
    if (Q_UNLIKELY(!file.open(QFile::ReadOnly)))
        throw ErrorInfo(tr("Cannot open '%1'.").arg(filePath));
    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    const QString sourceCode = stream.readAll();
    file.close();
    m_currentDirPathStack.push(FileInfo::path(filePath));
    evaluationResult = m_scriptImporter->importSourceCode(sourceCode, filePath, targetObject);
    m_currentDirPathStack.pop();
}

QString ImportHelper::scopeNameForFilepath(const QString &filePath) const
{
    return m_filePathScopeNameMap.value(filePath);
}

QJSValue ImportHelper::wrapInProxy(const QJSValue &extension,
                                              const QStringList &filePaths)
{
    auto *handler = new ImportProxyHandler(filePaths);
    return engine()->newProxyObject(extension, handler);
}

QJSValue ImportHelper::requireExtension(const QString &moduleName)
{
    // First try to load a named module if the argument doesn't look like a file path
    if (!moduleName.contains(QLatin1Char('/'))) {
        if (m_extensionSearchPathsStack.empty()) {
            engine()->throwError(QStringLiteral("require: internal error. No search paths."));
            return QJSValue();
        }

        if (engine()->logger().debugEnabled()) {
            engine()->logger().qbsDebug()
                    << "[require] loading extension " << moduleName;
        }

        QString moduleNameAsPath = moduleName;
        moduleNameAsPath.replace(QLatin1Char('.'), QLatin1Char('/'));
        const QStringList searchPaths = m_extensionSearchPathsStack.top();
        const QString dirPath = findExtensionDir(searchPaths, moduleNameAsPath);
        if (dirPath.isEmpty()) {
            QJSValue unused = engine()->newObject();
            if (moduleName.startsWith(QStringLiteral("qbs.")))
                return JsExtensions::loadExtension(engine(), moduleName.mid(4));
        } else {
            QString scopeName = moduleName;
            QDirIterator dit(dirPath, StringConstants::jsFileWildcards(),
                             QDir::Files | QDir::Readable);
            QJSValueList values;
            QStringList filePaths;
            try {
                while (dit.hasNext()) {
                    const QString filePath = dit.next();
                    if (engine()->logger().debugEnabled()) {
                        engine()->logger().qbsDebug()
                                << "[require] importing file " << filePath;
                    }
                    QJSValue obj = engine()->newObject();
                    importJsFile(filePath, obj);
                    values << obj;
                    filePaths.append(filePath);
                    m_filePathScopeNameMap[filePath] = scopeName;
                }
            } catch (const ErrorInfo &e) {
                engine()->throwError(e.toString());
                return QJSValue();
            }

            if (!values.empty()) {
                QJSValue result = mergeExtensionObjects(values);
//                engine()->m_requireResults.push_back(mergedValue);
//                engine()->m_filePathsPerImport[mergedValue.objectId()] = filePaths;
                result.setProperty(StringConstants::importScopeNamePropertyInternal(), scopeName);
                result = wrapInProxy(result, QStringList{filePaths});
                engine()->globalObject().setProperty(scopeName, result);
                return result;
            }
        }

        // The module name might be a file name component, which is assumed to be to a JavaScript
        // file located in the current directory search path; try that next
    }

    if (m_currentDirPathStack.empty()) {
        engine()->throwError(
            QStringLiteral("require: internal error. No current directory."));
        return QJSValue();
    }

    QJSValue result;
    try {
        const QString filePath = FileInfo::resolvePath(m_currentDirPathStack.top(),
                                                       moduleName);
        result = engine()->newObject();
        importJsFile(filePath, result);
        static const QString scopeNamePrefix = QStringLiteral("_qbs_scope_");
        const QString scopeName = scopeNamePrefix + QString::number(qHash(filePath), 16);
        result.setProperty(StringConstants::importScopeNamePropertyInternal(), scopeName);
        result = wrapInProxy(result, QStringList{filePath});
        engine()->globalObject().setProperty(scopeName, result);
        m_filePathScopeNameMap[filePath] = scopeName;
        return result;
    } catch (const ErrorInfo &e) {
        engine()->throwError(e.toString());
        result = QJSValue();
    }

    return result;
}

void ImportHelper::setEvaluator(Evaluator *evaluator)
{
    m_evaluator = evaluator;
}

const ImportHelper::FileContextScope *ImportHelper::fileContextScope(
        const FileContextBaseConstPtr &fileCtx)
{
    FileContextScope *result = &m_fileContextScopesMap[fileCtx];

    if (result->initialized)
        return result;

    for (const auto &name: fileCtx->jsExtensions()) {
        QJSValue extension = JsExtensions::loadExtension(engine(), name);
        QBS_CHECK(!extension.isUndefined());
        result->imports.insert(name, extension);
    }

    m_currentDirPathStack.push(FileInfo::path(fileCtx->filePath()));
    m_extensionSearchPathsStack.push(fileCtx->searchPaths());

    for (const JsImport &jsImport : fileCtx->jsImports()) {
        QBS_CHECK(!jsImport.scopeName.isEmpty());

        QJSValue mergedImport = engine()->newObject();
        for (const auto &filePath: jsImport.filePaths) {
            importJsFile(filePath, mergedImport);
            m_filePathScopeNameMap[filePath] = jsImport.scopeName;
        }
        QBS_CHECK(!mergedImport.isUndefined());

        mergedImport.setProperty(StringConstants::importScopeNamePropertyInternal(),
                                 jsImport.scopeName);

        QJSValue wrappedImport = wrapInProxy(mergedImport, jsImport.filePaths);
        engine()->globalObject().setProperty(jsImport.scopeName, wrappedImport);
        result->imports.insert(jsImport.scopeName, wrappedImport);
    }

    m_extensionSearchPathsStack.pop();
    m_currentDirPathStack.pop();

    result->convenienceProperties.insert(StringConstants::filePathGlobalVar(), fileCtx->filePath());
    result->convenienceProperties.insert(StringConstants::pathGlobalVar(), fileCtx->dirPath());

    result->initialized = true;

    return result;
}

Set<QString> ImportHelper::importedFiles() const {
    return Set<QString>::fromList(m_jsFileCache.keys());
}

void ImportHelper::setProfilingEnabled(bool enabled)
{
    m_elapsedTime = enabled ? 0 : -1;
}

ImportHelper *ImportHelper::instance(ScriptEngine *engine)
{
    Q_ASSERT(engine != nullptr);

    QJSValue instance = engine->QJSEngine::globalObject().property(QLatin1String("ImportHelper"));
    if (instance.isUndefined()) {
        instance = engine->newQObject(new ImportHelper(engine));
        engine->QJSEngine::globalObject().setProperty(QStringLiteral("ImportHelper"), instance);
        const auto result = engine->evaluate(requireCode);
        Q_ASSERT_X(!result.isError(), "", qPrintable(result.toString()));
    }

    Q_ASSERT(!instance.isUndefined());
    Q_ASSERT(instance.isQObject());
    return qobject_cast<ImportHelper *>(instance.toQObject());
}

const ImportHelper *ImportHelper::instance(const ScriptEngine *engine)
{
    Q_ASSERT(engine != nullptr);

    QJSValue instance = engine->QJSEngine::globalObject().property(QLatin1String("ImportHelper"));
    Q_ASSERT(!instance.isUndefined());

    return qobject_cast<const ImportHelper *>(instance.toQObject());
}

}
}

#include "importhelper.moc"
