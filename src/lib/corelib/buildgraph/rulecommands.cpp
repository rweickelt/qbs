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

#include "rulecommands.h"
#include <jsextensions/importhelper.h>
#include <language/language.h>
#include <language/scriptengine.h>
#include <logging/translator.h>
#include <tools/error.h>
#include <tools/fileinfo.h>
#include <tools/hostosinfo.h>
#include <tools/proxyhandler.h>
#include <tools/qbsassert.h>
#include <tools/stringconstants.h>

#include <QtCore/qfile.h>

#include <QtQml/qjsvalueiterator.h>

namespace qbs {
namespace Internal {

static QString argumentsProperty() { return QStringLiteral("arguments"); }
static QString environmentProperty() { return QStringLiteral("environment"); }
static QString extendedDescriptionProperty() { return QStringLiteral("extendedDescription"); }
static QString highlightProperty() { return QStringLiteral("highlight"); }
static QString ignoreDryRunProperty() { return QStringLiteral("ignoreDryRun"); }
static QString maxExitCodeProperty() { return QStringLiteral("maxExitCode"); }
static QString programProperty() { return QStringLiteral("program"); }
static QString responseFileArgumentIndexProperty()
{
    return QStringLiteral("responseFileArgumentIndex");
}
static QString responseFileThresholdProperty() { return QStringLiteral("responseFileThreshold"); }
static QString responseFileUsagePrefixProperty()
{
    return QStringLiteral("responseFileUsagePrefix");
}
static QString responseFileSeparatorProperty() { return QStringLiteral("responseFileSeparator"); }
static QString silentProperty() { return QStringLiteral("silent"); }
static QString stderrFilePathProperty() { return QStringLiteral("stderrFilePath"); }
static QString stderrFilterFunctionProperty() { return QStringLiteral("stderrFilterFunction"); }
static QString stdoutFilePathProperty() { return QStringLiteral("stdoutFilePath"); }
static QString stdoutFilterFunctionProperty() { return QStringLiteral("stdoutFilterFunction"); }
static QString timeoutProperty() { return QStringLiteral("timeout"); }
static QString workingDirProperty() { return QStringLiteral("workingDirectory"); }

enum class InvocationType {
    Deferred,
    Immediate,
};

static std::tuple<CodeLocation, QString>
invokedSourceCode(ScriptEngine *engine, const QJSValue &codeOrFunction,
                  const CodeLocation &fallbackLocation,
                  InvocationType invocationType = InvocationType::Deferred)
{
    CodeLocation location = fallbackLocation;
    QString sourceCode;
    if (codeOrFunction.isCallable()) {
        location = engine->extractFunctionLocation(codeOrFunction);
        sourceCode = engine->extractFunctionSourceCode(location);
        if (invocationType == InvocationType::Immediate)
            sourceCode = QStringLiteral("(") + sourceCode + QStringLiteral(")()");
    } else if (codeOrFunction.isString()) {
        sourceCode = codeOrFunction.toString();
        if (invocationType == InvocationType::Immediate)
            sourceCode = QStringLiteral("(function(){") + sourceCode + QStringLiteral("})()");
    }
    return std::make_tuple(location, sourceCode);
}

AbstractCommand::AbstractCommand()
    : m_description(defaultDescription()),
      m_extendedDescription(defaultExtendedDescription()),
      m_highlight(defaultHighLight()),
      m_ignoreDryRun(defaultIgnoreDryRun()),
      m_silent(defaultIsSilent()),
      m_timeout(defaultTimeout())
{
}

AbstractCommand::~AbstractCommand() = default;

bool AbstractCommand::equals(const AbstractCommand *other) const
{
    return type() == other->type()
            && m_description == other->m_description
            && m_extendedDescription == other->m_extendedDescription
            && m_highlight == other->m_highlight
            && m_ignoreDryRun == other->m_ignoreDryRun
            && m_silent == other->m_silent
            && m_jobPool == other->m_jobPool
            && m_timeout == other->m_timeout
            && m_properties == other->m_properties;
}

void AbstractCommand::fillFromScriptValue(const QJSValue *scriptValue,
                                          const CodeLocation &codeLocation)
{
    Q_UNUSED(scriptValue);
    m_codeLocation = codeLocation;
    m_predefinedProperties
            << StringConstants::descriptionProperty()
            << extendedDescriptionProperty()
            << highlightProperty()
            << ignoreDryRunProperty()
            << StringConstants::jobPoolProperty()
            << silentProperty()
            << timeoutProperty();
}

QString AbstractCommand::fullDescription(const QString &productName) const
{
    return description() + QLatin1String(" [") + productName + QLatin1Char(']');
}

void AbstractCommand::load(PersistentPool &pool)
{
    serializationOp<PersistentPool::Load>(pool);
}

void AbstractCommand::store(PersistentPool &pool)
{
    serializationOp<PersistentPool::Store>(pool);
}

void AbstractCommand::extractUnknownProperties(const QJSValue *scriptValue)
{
    QJSValueIterator it(*scriptValue);
    while (it.hasNext()) {
        it.next();
        if (m_predefinedProperties.contains(it.name()))
            continue;
        const bool isInvalid = it.value().toQObject()
                || AbstractProxyHandler::isProxiedObject(it.value());
        if (Q_UNLIKELY(isInvalid)) {
            throw ErrorInfo(Tr::tr("Property '%1' has a type unsuitable for storing in a command "
                                   "object.").arg(it.name()), m_codeLocation);
        }
        m_properties.insert(it.name(), it.value().toVariant());
    }
}

void ProcessCommand::setupForJavaScript(ScriptEngine *engine)
{
    QJSValue ctor = engine->newQMetaObject(&ProcessCommand::staticMetaObject);
    engine->globalObject().setProperty(StringConstants::commandType(), ctor);
}

ProcessCommand::ProcessCommand(const QString &program, const QStringList &arguments)
    : m_program(program)
    , m_arguments(arguments)
    , m_maxExitCode(0)
    , m_responseFileThreshold(defaultResponseFileThreshold())
    , m_responseFileArgumentIndex(0)
    , m_responseFileSeparator(QStringLiteral("\n"))
{
}

int ProcessCommand::defaultResponseFileThreshold() const
{
    // TODO: Non-Windows platforms likely have their own limits. Investigate.
    return HostOsInfo::isWindowsHost()
            ? 31000 // 32000 minus "safety offset"
            : -1;
}

void ProcessCommand::setEnvironmentFromList(const QStringList &envList)
{
    m_environment.clear();
    for (const QString &env : envList) {
        const int equalsIndex = env.indexOf(QLatin1Char('='));
        if (equalsIndex <= 0 || equalsIndex == env.size() - 1)
            continue;
        const QString &var = env.left(equalsIndex);
        const QString &value = env.mid(equalsIndex + 1);
        m_environment.insert(var, value);
    }
}

bool ProcessCommand::equals(const AbstractCommand *otherAbstractCommand) const
{
    if (!AbstractCommand::equals(otherAbstractCommand))
        return false;
    const auto other = static_cast<const ProcessCommand *>(otherAbstractCommand);
    return m_program == other->m_program
            && m_arguments == other->m_arguments
            && m_workingDir == other->m_workingDir
            && m_maxExitCode == other->m_maxExitCode
            && m_stdoutFilterFunction == other->m_stdoutFilterFunction
            && m_stderrFilterFunction == other->m_stderrFilterFunction
            && m_responseFileThreshold == other->m_responseFileThreshold
            && m_responseFileArgumentIndex == other->m_responseFileArgumentIndex
            && m_responseFileUsagePrefix == other->m_responseFileUsagePrefix
            && m_responseFileSeparator == other->m_responseFileSeparator
            && m_stdoutFilePath == other->m_stdoutFilePath
            && m_stderrFilePath == other->m_stderrFilePath
            && m_relevantEnvVars == other->m_relevantEnvVars
            && m_relevantEnvValues == other->m_relevantEnvValues
            && m_environment == other->m_environment;
}

void ProcessCommand::fillFromScriptValue(const QJSValue *scriptValue,
                                         const CodeLocation &codeLocation)
{
    ScriptEngine *engine = scriptEngine(this);
    AbstractCommand::fillFromScriptValue(scriptValue, codeLocation);
    CodeLocation _;
    if (!m_stderrFilterFunctionValue.isUndefined()) {
        std::tie(_, m_stderrFilterFunction) = invokedSourceCode(engine,
                                                                m_stderrFilterFunctionValue,
                                                                codeLocation);
        m_stderrFilterFunctionValue = QJSValue();
    }

    if (!m_stdoutFilterFunctionValue.isUndefined()) {
        std::tie(_, m_stdoutFilterFunction) = invokedSourceCode(engine,
                                                                m_stdoutFilterFunctionValue,
                                                                codeLocation);
        m_stdoutFilterFunctionValue = QJSValue();
    }
    m_predefinedProperties
            << programProperty()
            << argumentsProperty()
            << workingDirProperty()
            << maxExitCodeProperty()
            << stdoutFilterFunctionProperty()
            << stderrFilterFunctionProperty()
            << responseFileThresholdProperty()
            << responseFileSeparatorProperty()
            << responseFileArgumentIndexProperty()
            << responseFileUsagePrefixProperty()
            << environmentProperty()
            << stdoutFilePathProperty()
            << stderrFilePathProperty();
    extractUnknownProperties(scriptValue);
}

QStringList ProcessCommand::relevantEnvVars() const
{
    QStringList vars = m_relevantEnvVars;
    if (!FileInfo::isAbsolute(program()))
        vars << StringConstants::pathEnvVar();
    return vars;
}

void ProcessCommand::addRelevantEnvValue(const QString &key, const QString &value)
{
    m_relevantEnvValues.insert(key, value);
}

void ProcessCommand::load(PersistentPool &pool)
{
    AbstractCommand::load(pool);
    serializationOp<PersistentPool::Load>(pool);
}

void ProcessCommand::store(PersistentPool &pool)
{
    AbstractCommand::store(pool);
    serializationOp<PersistentPool::Store>(pool);
}

void JavaScriptCommand::setupForJavaScript(ScriptEngine *engine)
{
    QJSValue ctor = engine->newQMetaObject(&JavaScriptCommand::staticMetaObject);
    engine->globalObject().setProperty(StringConstants::javaScriptCommandType(), ctor);
}

JavaScriptCommand::JavaScriptCommand()
{
}

bool JavaScriptCommand::equals(const AbstractCommand *otherAbstractCommand) const
{
    if (!AbstractCommand::equals(otherAbstractCommand))
        return false;
    auto const other = static_cast<const JavaScriptCommand *>(otherAbstractCommand);
    return m_sourceCode == other->m_sourceCode;
}

void JavaScriptCommand::fillFromScriptValue(const QJSValue *scriptValue,
                                            const CodeLocation &codeLocation)
{
    ScriptEngine *engine = scriptEngine(this);
    CodeLocation preciseLocation;
    std::tie(preciseLocation, m_sourceCode) =
            invokedSourceCode(engine, m_sourceCodeValue, codeLocation, InvocationType::Immediate);
    m_sourceCodeValue = QJSValue();
    AbstractCommand::fillFromScriptValue(scriptValue, preciseLocation);

    ImportHelper *importer = ImportHelper::instance(engine);
    m_scopeName = importer->scopeNameForFilepath(preciseLocation.filePath());
    m_predefinedProperties << StringConstants::classNameProperty()
                           << StringConstants::sourceCodeProperty()
                           << StringConstants::importScopeNamePropertyInternal();
    extractUnknownProperties(scriptValue);
}

void JavaScriptCommand::load(PersistentPool &pool)
{
    AbstractCommand::load(pool);
    serializationOp<PersistentPool::Load>(pool);
}

void JavaScriptCommand::store(PersistentPool &pool)
{
    AbstractCommand::store(pool);
    serializationOp<PersistentPool::Store>(pool);
}

void CommandList::load(PersistentPool &pool)
{
    m_commands.clear();
    int count = pool.load<int>();
    m_commands.reserve(count);
    while (--count >= 0) {
        const auto cmdType = pool.load<quint8>();
        AbstractCommandPtr cmd;
        switch (cmdType) {
        case AbstractCommand::JavaScriptCommandType:
            cmd = pool.load<JavaScriptCommandPtr>();
            break;
        case AbstractCommand::ProcessCommandType:
            cmd = pool.load<ProcessCommandPtr>();
            break;
        default:
            QBS_CHECK(false);
        }
        addCommand(cmd);
    }
}

void CommandList::store(PersistentPool &pool) const
{
    pool.store(int(m_commands.size()));
    for (const AbstractCommandPtr &cmd : m_commands) {
        pool.store(static_cast<quint8>(cmd->type()));
        pool.store(cmd);
    }
}

bool operator==(const CommandList &l1, const CommandList &l2)
{
    if (l1.size() != l2.size())
        return false;
    for (int i = 0; i < l1.size(); ++i)
        if (!l1.commandAt(i)->equals(l2.commandAt(i).get()))
            return false;
    return true;
}

} // namespace Internal
} // namespace qbs
