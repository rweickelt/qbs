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

#include "jsextensions.h"
#include <language/scriptengine.h>
#include <logging/translator.h>
#include <tools/executablefinder.h>
#include <tools/hostosinfo.h>
#include <tools/shellutils.h>
#include <tools/stringconstants.h>

#include <QtCore/qobject.h>
#include <QtCore/qprocess.h>
#include <QtCore/qtextstream.h>
#include <QtCore/qvariant.h>
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QtCore5Compat/qtextcodec.h>
#else
#include <QtCore/qtextcodec.h>
#endif

#include <QtQml/qjsvalue.h>

namespace qbs {
namespace Internal {

class Process : public JsExtension, public ResourceAcquiringScriptObject
{
    Q_OBJECT
public:
    Q_INVOKABLE Process(QObject *engine);
    ~Process() override;
    Q_INVOKABLE QString getEnv(const QString &name);
    Q_INVOKABLE void setEnv(const QString &name, const QString &value);
    Q_INVOKABLE void setCodec(const QString &codec);

    Q_INVOKABLE QString workingDirectory();
    Q_INVOKABLE void setWorkingDirectory(const QString &dir);

    Q_INVOKABLE bool start(const QString &program, const QStringList &arguments);
    Q_INVOKABLE int exec(const QString &program, const QStringList &arguments,
                         bool throwOnError = false);
    Q_INVOKABLE void close();
    Q_INVOKABLE bool waitForFinished(int msecs = 30000);
    Q_INVOKABLE void terminate();
    Q_INVOKABLE void kill();

    Q_INVOKABLE QString readLine();
    Q_INVOKABLE bool atEnd() const;
    Q_INVOKABLE QString readStdOut();
    Q_INVOKABLE QString readStdErr();

    Q_INVOKABLE void closeWriteChannel();

    Q_INVOKABLE void write(const QString &str);
    Q_INVOKABLE void writeLine(const QString &str);

    Q_INVOKABLE int exitCode() const;
private:
    QString findExecutable(const QString &filePath) const;

    // ResourceAcquiringScriptObject implementation
    void releaseResources() override;

    QProcess *m_qProcess = nullptr;
    QProcessEnvironment m_environment;
    QString m_workingDirectory;
    QTextCodec *m_codec = nullptr;
};

Process::Process(QObject *engine)
    : ResourceAcquiringScriptObject(qobject_cast<ScriptEngine *>(engine))
{
    m_qProcess = new QProcess;
    m_codec = QTextCodec::codecForName("UTF-8");

    auto *se = qobject_cast<ScriptEngine *>(engine);
    Q_ASSERT(se);
    se->addResourceAcquiringScriptObject(this);
    const DubiousContextList dubiousContexts ({
            DubiousContext(EvalContext::PropertyEvaluation, DubiousContext::SuggestMoving)
    });
    se->checkContext(QStringLiteral("qbs.Process"), dubiousContexts);

    // Get environment
    QVariant v = se->property(StringConstants::qbsProcEnvVarInternal());
    if (v.isNull()) {
        // The build environment is not initialized yet.
        // This can happen if one uses Process on the RHS of a binding like Group.name.
        m_environment = se->environment();
    } else {
        m_environment
            = QProcessEnvironment(*reinterpret_cast<QProcessEnvironment*>(v.value<void*>()));
    }
    se->setUsesIo();
}

Process::~Process()
{
    close();
}

QString Process::getEnv(const QString &name)
{
    return m_environment.value(name);
}

void Process::setEnv(const QString &name, const QString &value)
{
    m_environment.insert(name, value);
}

QString Process::workingDirectory()
{
    return m_workingDirectory;
}

void Process::setWorkingDirectory(const QString &dir)
{
    m_workingDirectory = dir;
}

bool Process::start(const QString &program, const QStringList &arguments)
{
    if (!m_workingDirectory.isEmpty())
        m_qProcess->setWorkingDirectory(m_workingDirectory);

    m_qProcess->setProcessEnvironment(m_environment);
    m_qProcess->start(findExecutable(program), arguments, QIODevice::ReadWrite | QIODevice::Text);
    return m_qProcess->waitForStarted();
}

int Process::exec(const QString &program, const QStringList &arguments, bool throwOnError)
{
    if (!start(findExecutable(program), arguments)) {
        if (throwOnError) {
            engine()->throwError(Tr::tr("Error running '%1': %2")
                                  .arg(program, m_qProcess->errorString()));
        }
        return -1;
    }
    m_qProcess->closeWriteChannel();
    m_qProcess->waitForFinished(-1);
    if (throwOnError) {
        if (m_qProcess->error() != QProcess::UnknownError
                && m_qProcess->error() != QProcess::Crashed) {
            engine()->throwError(Tr::tr("Error running '%1': %2")
                                  .arg(program, m_qProcess->errorString()));
        } else if (m_qProcess->exitStatus() == QProcess::CrashExit || m_qProcess->exitCode() != 0) {
            QString errorMessage = m_qProcess->error() == QProcess::Crashed
                    ? Tr::tr("Error running '%1': %2").arg(program, m_qProcess->errorString())
                    : Tr::tr("Process '%1 %2' finished with exit code %3.").arg(program).
                      arg(arguments.join(QStringLiteral(" "))).arg(m_qProcess->exitCode());
            const QString stdOut = readStdOut();
            if (!stdOut.isEmpty())
                errorMessage.append(Tr::tr(" The standard output was:\n")).append(stdOut);
            const QString stdErr = readStdErr();
            if (!stdErr.isEmpty())
                errorMessage.append(Tr::tr(" The standard error output was:\n")).append(stdErr);
            engine()->throwError(errorMessage);
        }
    }
    if (m_qProcess->error() != QProcess::UnknownError)
        return -1;
    return m_qProcess->exitCode();
}

void Process::close()
{
    if (!m_qProcess)
        return;

    delete m_qProcess;
    m_qProcess = nullptr;
}

bool Process::waitForFinished(int msecs)
{
    if (m_qProcess->state() == QProcess::NotRunning)
        return true;
    return m_qProcess->waitForFinished(msecs);
}

void Process::terminate()
{
    m_qProcess->terminate();
}

void Process::kill()
{
    m_qProcess->kill();
}

void Process::setCodec(const QString &codec)
{
    const auto newCodec = QTextCodec::codecForName(qPrintable(codec));
    if (newCodec)
        m_codec = newCodec;
}

QString Process::readLine()
{
    auto result = m_codec->toUnicode(m_qProcess->readLine());
    if (!result.isEmpty() && result.back() == QLatin1Char('\n'))
        result.chop(1);
    return result;
}

bool Process::atEnd() const
{
    return m_qProcess->atEnd();
}

QString Process::readStdOut()
{
    return m_codec->toUnicode(m_qProcess->readAllStandardOutput());
}

QString Process::readStdErr()
{
    return m_codec->toUnicode(m_qProcess->readAllStandardError());
}

void Process::closeWriteChannel()
{
    m_qProcess->closeWriteChannel();
}

int Process::exitCode() const
{
    return m_qProcess->exitCode();
}

QString Process::findExecutable(const QString &filePath) const
{
    ExecutableFinder exeFinder(ResolvedProductPtr(), m_environment);
    return exeFinder.findExecutable(filePath, m_workingDirectory);
}

void Process::releaseResources()
{
    close();
}

void Process::write(const QString &str)
{
    m_qProcess->write(m_codec->fromUnicode(str));
}

void Process::writeLine(const QString &str)
{
    m_qProcess->write(m_codec->fromUnicode(str));
    m_qProcess->putChar('\n');
}

QJSValue createProcessExtension(QJSEngine *engine)
{
    QJSValue mo = engine->newQMetaObject(&Process::staticMetaObject);
    QJSValue factory = engine->evaluate(
                QStringLiteral("(function(m, e){ return function(){ return new m(e); } })"));
    QJSValue wrapper = factory.call(QJSValueList{mo, engine->toScriptValue(engine)});

    return wrapper;
}

QBS_REGISTER_JS_EXTENSION("Process", createProcessExtension)

} // namespace Internal
} // namespace qbs

#include "process.moc"
