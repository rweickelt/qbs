/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Copyright (C) 2019 Jochen Ulrich <jochenulrich@t-online.de>
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

#include "jscommandexecutor.h"

#include "artifact.h"
#include "buildgraph.h"
#include "rulecommands.h"
#include "transformer.h"

#include <language/language.h>
#include <language/preparescriptobserver.h>
#include <language/resolvedfilecontext.h>
#include <language/scriptengine.h>
#include <logging/logger.h>
#include <tools/codelocation.h>
#include <tools/error.h>
#include <tools/qbsassert.h>
#include <tools/qttools.h>
#include <tools/stringconstants.h>

#include <QtCore/qeventloop.h>
#include <QtCore/qpointer.h>
#include <QtCore/qthread.h>
#include <QtCore/qtimer.h>

#include <QtQml/QJSValueIterator>

namespace qbs {
namespace Internal {

struct JavaScriptCommandResult
{
    bool success = false;
    QString errorMessage;
    CodeLocation errorLocation;
};

class JsCommandExecutorThreadObject : public QObject
{
    Q_OBJECT
public:
    JsCommandExecutorThreadObject(Logger logger)
        : m_logger(std::move(logger))
        , m_scriptEngine(nullptr)
    {
    }

    const JavaScriptCommandResult &result() const
    {
        return m_result;
    }

    void cancel(const qbs::ErrorInfo &reason)
    {
        std::lock_guard lock(m_mutex);
        if (m_scriptEngine)
            m_scriptEngine->cancel();
        m_cancelled = true;
        m_result.success = !reason.hasError();
        m_result.errorMessage = reason.toString();
    }

signals:
    void finished();

public:
    void start(const JavaScriptCommand *cmd, Transformer *transformer)
    {
        std::lock_guard lock(m_mutex);
        if (!m_cancelled) {
            m_running = true;
            try {
                doStart(cmd, transformer);
            } catch (const qbs::ErrorInfo &error) {
                setError(error.toString(), cmd->codeLocation());
            }
            m_running = false;
        }
        emit finished();
    }

private:
    void doStart(const JavaScriptCommand *cmd, Transformer *transformer)
    {
        m_result.success = true;
        m_result.errorMessage.clear();
        ScriptEngine *scriptEngine = provideScriptEngine();
        QJSValue scope = scriptEngine->newObject();
        m_scriptEngine->clearRequestedProperties();
        setupScriptEngineForFile(scriptEngine,
                                 transformer->rule->prepareScript.fileContext(), scope,
                                 ObserveMode::Enabled);

        for (QVariantMap::const_iterator it = cmd->properties().constBegin();
                it != cmd->properties().constEnd(); ++it) {
            scope.setProperty(it.key(), scriptEngine->toScriptValue(it.value()));
        }

        setupScriptEngineForProduct(scriptEngine, transformer->product().get(),
                                    transformer->rule->module.get(), scope, true);
        transformer->setupInputs(scriptEngine, scope);
        transformer->setupOutputs(scriptEngine, scope);
        transformer->setupExplicitlyDependsOn(scriptEngine, scope);

        // If JavaScriptCommand is created inside a X.js file, all properties must
        // be accessible without explicitly writing X.y. We use prototype lookup for
        // simplicity. But since the ImportProxyHandler misses a lot of traps and does
        // not inherit from Object, it must become prototype after we are done setting
        // up the scope object.
        QString scopeName = cmd->scopeName();
        if (!scopeName.isEmpty()) {
            QJSValue importScopeForSourceCode = scriptEngine->globalObject().property(scopeName);
            scope.setPrototype(importScopeForSourceCode);
        }

        QStringList stackTrace;
        // After unlocking the mutex the script engine might be set into interrupted state
        // at any time by the cancellation handler. That is OK even if the actual evaluation
        // has not started yet because in interrupted state, the engine will not even start.
        m_mutex.unlock();
        QJSValue result = scriptEngine->evaluate(scope, cmd->sourceCode(),
                                                 cmd->codeLocation().filePath(),
                                                 cmd->codeLocation().line(),
                                                 cmd->codeLocation().column(),
                                                 &stackTrace);
        // Locking the mutex ensures that the cancellation handler completes if it is
        // active.
        m_mutex.lock();

        scriptEngine->releaseResourcesOfScriptObjects();
        transformer->propertiesRequestedInCommands
                += scriptEngine->propertiesRequestedInScript();
        unite(transformer->propertiesRequestedFromArtifactInCommands,
              scriptEngine->propertiesRequestedFromArtifact());
        const std::vector<QString> &importFilesUsedInCommand
                = scriptEngine->importedFilesUsedInScript();
        transformer->importedFilesUsedInCommands.insert(
                    transformer->importedFilesUsedInCommands.cend(),
                    importFilesUsedInCommand.cbegin(), importFilesUsedInCommand.cend());
        transformer->depsRequestedInCommands.add(scriptEngine->productsWithRequestedDependencies());
        transformer->artifactsMapRequestedInCommands.unite(scriptEngine->requestedArtifacts());
        for (const ResolvedProduct * const p : scriptEngine->requestedExports()) {
            transformer->exportedModulesAccessedInCommands.insert(
                        std::make_pair(p->uniqueName(), p->exportedModule));
        }
        scriptEngine->clearRequestedProperties();
        if (result.isError() && !m_cancelled) {
            ErrorInfo error = ScriptEngine::toErrorInfo(result);
            setError(error.items().first().description(), error.items().first().codeLocation());
        }
    }

    void setError(const QString &errorMessage, const CodeLocation &codeLocation)
    {
        m_result.success = false;
        m_result.errorMessage = errorMessage;
        m_result.errorLocation = codeLocation;
    }

    ScriptEngine *provideScriptEngine()
    {
        if (!m_scriptEngine)
            m_scriptEngine = ScriptEngine::create(m_logger, EvalContext::JsCommand, this);
        return m_scriptEngine;
    }

    Logger m_logger;
    ScriptEngine *m_scriptEngine;
    JavaScriptCommandResult m_result;
    bool m_running = false;
    bool m_cancelled = false;
    std::mutex m_mutex;
};


JsCommandExecutor::JsCommandExecutor(const Logger &logger, QObject *parent)
    : AbstractCommandExecutor(logger, parent)
    , m_thread(new QThread(this))
    , m_objectInThread(new JsCommandExecutorThreadObject(logger))
    , m_running(false)
{
    qRegisterMetaType<Transformer *>("Transformer *");
    qRegisterMetaType<const JavaScriptCommand *>("const JavaScriptCommand *");

    m_objectInThread->moveToThread(m_thread);
    connect(m_objectInThread, &JsCommandExecutorThreadObject::finished,
            this, &JsCommandExecutor::onJavaScriptCommandFinished);
    connect(this, &JsCommandExecutor::startRequested,
            m_objectInThread, &JsCommandExecutorThreadObject::start);
}

JsCommandExecutor::~JsCommandExecutor()
{
    waitForFinished();
    m_thread->quit();
    m_thread->wait();
    delete m_objectInThread;
}

void JsCommandExecutor::doReportCommandDescription(const QString &productName)
{
    if ((m_echoMode == CommandEchoModeCommandLine
         || m_echoMode == CommandEchoModeCommandLineWithEnvironment)
            && !command()->extendedDescription().isEmpty()) {
        emit reportCommandDescription(command()->highlight(), command()->extendedDescription());
        return;
    }

    AbstractCommandExecutor::doReportCommandDescription(productName);
}

void JsCommandExecutor::waitForFinished()
{
    if (!m_running)
        return;
    QEventLoop loop;
    connect(this, &AbstractCommandExecutor::finished, &loop, &QEventLoop::quit);
    loop.exec();
}

bool JsCommandExecutor::doStart()
{
    QBS_ASSERT(!m_running, return false);

    if (dryRun() && !command()->ignoreDryRun()) {
        QTimer::singleShot(0, this, [this] { emit finished(); }); // Don't call back on the caller.
        return false;
    }

    m_thread->start();
    m_running = true;
    emit startRequested(jsCommand(), transformer());
    return true;
}

void JsCommandExecutor::cancel(const qbs::ErrorInfo &reason)
{
    if (m_running && (!dryRun() || command()->ignoreDryRun()))
        m_objectInThread->cancel(reason);
}

void JsCommandExecutor::onJavaScriptCommandFinished()
{
    m_running = false;
    const JavaScriptCommandResult &result = m_objectInThread->result();
    ErrorInfo err;
    if (!result.success) {
        logger().qbsDebug() << "JS context:\n" << jsCommand()->properties();
        logger().qbsDebug() << "JS code:\n" << jsCommand()->sourceCode();
        err.append(result.errorMessage);
        // ### We don't know the line number of the command's sourceCode property assignment.
        err.appendBacktrace(QStringLiteral("JavaScriptCommand.sourceCode"), result.errorLocation);
        err.appendBacktrace(QStringLiteral("Rule.prepare"),
                            transformer()->rule->prepareScript.location());
    }
    emit finished(err);
}

const JavaScriptCommand *JsCommandExecutor::jsCommand() const
{
    return static_cast<const JavaScriptCommand *>(command());
}

} // namespace Internal
} // namespace qbs

#include "jscommandexecutor.moc"
