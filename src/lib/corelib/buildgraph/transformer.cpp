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
#include "transformer.h"

#include "artifact.h"
#include "artifactsscriptvalue.h"
#include <jsextensions/moduleproperties.h>
#include <language/language.h>
#include <language/preparescriptobserver.h>
#include <language/scriptengine.h>
#include <logging/translator.h>
#include <tools/error.h>
#include <tools/fileinfo.h>
#include <tools/scripttools.h>
#include <tools/qbsassert.h>
#include <tools/stringconstants.h>
#include <tools/stlutils.h>

#include <QtCore/qdir.h>

#include <algorithm>

namespace qbs {
namespace Internal {

Transformer::Transformer() : alwaysRun(false)
{
}

Transformer::~Transformer() = default;

QJSValue Transformer::translateFileConfig(ScriptEngine *scriptEngine, const Artifact *artifact,
                                          const QString &defaultModuleName)
{
    QJSValue obj = scriptValueForArtifact(scriptEngine, artifact, defaultModuleName);
    return obj;
}

static bool compareByFilePath(const Artifact *a1, const Artifact *a2)
{
    return a1->filePath() < a2->filePath();
}

QJSValue Transformer::translateInOutputs(ScriptEngine *scriptEngine,
                                         const ArtifactSet &artifacts,
                                         const QString &defaultModuleName)
{
    using TagArtifactsMap = QMap<QString, QList<Artifact*>>;
    TagArtifactsMap tagArtifactsMap;
    for (Artifact *artifact : artifacts)
        for (const FileTag &fileTag : artifact->fileTags())
            tagArtifactsMap[fileTag.toString()].push_back(artifact);
    for (auto it = tagArtifactsMap.begin(); it != tagArtifactsMap.end(); ++it)
        std::sort(it.value().begin(), it.value().end(), compareByFilePath);

    QJSValue jsTagFiles = scriptEngine->newObject();
    for (auto tag = tagArtifactsMap.constBegin(); tag != tagArtifactsMap.constEnd(); ++tag) {
        const QList<Artifact*> &artifacts = tag.value();
        QJSValue jsFileConfig = scriptEngine->newArray(artifacts.size());
        int i = 0;
        for (Artifact * const artifact : artifacts) {
            jsFileConfig.setProperty(i++, translateFileConfig(scriptEngine, artifact,
                                                              defaultModuleName));
        }
        jsTagFiles.setProperty(tag.key(), jsFileConfig);
    }

    return jsTagFiles;
}

ResolvedProductPtr Transformer::product() const
{
    if (outputs.empty())
        return {};
    return (*outputs.cbegin())->product.lock();
}

void Transformer::setupInputs(ScriptEngine *engine, QJSValue &targetScriptValue)
{
    const QString &defaultModuleName = rule->module->name;
    QJSValue scriptValue = translateInOutputs(engine, inputs, defaultModuleName);
    targetScriptValue.setProperty(StringConstants::inputsVar(), scriptValue);
    QJSValue inputScriptValue;
    if (inputs.size() == 1) {
        Artifact *input = *inputs.cbegin();
        const FileTags &fileTags = input->fileTags();
        QBS_ASSERT(!fileTags.empty(), return);
        QJSValue inputsForFileTag = scriptValue.property(fileTags.cbegin()->toString());
        inputScriptValue = inputsForFileTag.property(0);
    }
    targetScriptValue.setProperty(StringConstants::inputVar(), inputScriptValue);
}

void Transformer::setupOutputs(ScriptEngine *engine, QJSValue &targetScriptValue)
{
    const QString &defaultModuleName = rule->module->name;
    QJSValue scriptValue = translateInOutputs(engine, outputs, defaultModuleName);
    targetScriptValue.setProperty(StringConstants::outputsVar(), scriptValue);
    QJSValue outputScriptValue;
    if (outputs.size() == 1) {
        Artifact *output = *outputs.cbegin();
        const FileTags &fileTags = output->fileTags();
        QBS_ASSERT(!fileTags.empty(), return);
        QJSValue outputsForFileTag = scriptValue.property(fileTags.cbegin()->toString());
        outputScriptValue = outputsForFileTag.property(0);
    }
    targetScriptValue.setProperty(StringConstants::outputVar(), outputScriptValue);
}

void Transformer::setupExplicitlyDependsOn(ScriptEngine *engine, QJSValue &targetScriptValue)
{
    const QString &defaultModuleName = rule->module->name;
    QJSValue scriptValue = translateInOutputs(engine, explicitlyDependsOn, defaultModuleName);
    targetScriptValue.setProperty(StringConstants::explicitlyDependsOnVar(), scriptValue);
}

AbstractCommandPtr Transformer::createCommandFromScriptValue(const QJSValue &scriptValue,
                                                             const CodeLocation &codeLocation)
{
    AbstractCommandPtr cmdBase(qobject_cast<AbstractCommand *>(scriptValue.toQObject()));
    if (!cmdBase)
        return cmdBase;
    QQmlEngine::setObjectOwnership(cmdBase.get(), QQmlEngine::CppOwnership);
    cmdBase->fillFromScriptValue(&scriptValue, codeLocation);
    if (cmdBase->type() == AbstractCommand::ProcessCommandType) {
        auto procCmd = static_cast<ProcessCommand *>(cmdBase.get());
        procCmd->clearRelevantEnvValues();
        const auto envVars = procCmd->relevantEnvVars();
        for (const QString &key : envVars)
            procCmd->addRelevantEnvValue(key, product()->buildEnvironment.value(key));
    }
    return cmdBase;
}

void Transformer::createCommands(ScriptEngine *engine, const PrivateScriptFunction &script,
                                 const QJSValueList &args)
{
    if (script.scriptFunction.isUndefined()) {
        script.scriptFunction = engine->evaluate2(script.sourceCode(),
                                                 script.location().filePath(),
                                                 script.location().line(),
                                                 script.location().column());
        if (script.scriptFunction.isError())
            throw ScriptEngine::toErrorInfo(script.scriptFunction);
        if (Q_UNLIKELY(!script.scriptFunction.isCallable()))
            throw ErrorInfo(Tr::tr("Invalid prepare script."), script.location());
    }

    QJSValue scriptValue = engine->call(&script.scriptFunction, args);
    // engine->collectGarbage(); // Prevents ASSERT: "m->inUse()" in qv4mm.cpp on Windows and macOS
    engine->releaseResourcesOfScriptObjects();
    propertiesRequestedInPrepareScript = engine->propertiesRequestedInScript();
    propertiesRequestedFromArtifactInPrepareScript = engine->propertiesRequestedFromArtifact();
    importedFilesUsedInPrepareScript = engine->importedFilesUsedInScript();
    depsRequestedInPrepareScript = engine->requestedDependencies();
    artifactsMapRequestedInPrepareScript = engine->requestedArtifacts();
    lastPrepareScriptExecutionTime = FileTime::currentTime();
    for (const ResolvedProduct * const p : engine->requestedExports()) {
        exportedModulesAccessedInPrepareScript.insert(std::make_pair(p->uniqueName(),
                                                                     p->exportedModule));
    }
    engine->clearRequestedProperties();
    if (Q_UNLIKELY(scriptValue.isError()))
        throw ScriptEngine::toErrorInfo(scriptValue);
    commands.clear();
    if (scriptValue.isArray()) {
        const int count = scriptValue.property(StringConstants::lengthProperty()).toInt();
        for (qint32 i = 0; i < count; ++i) {
            QJSValue item = scriptValue.property(i);
            if (!item.isUndefined()) {
                const AbstractCommandPtr cmd
                        = createCommandFromScriptValue(item, script.location());
                if (cmd)
                    commands.addCommand(cmd);
            }
        }
    } else {
        const AbstractCommandPtr cmd = createCommandFromScriptValue(scriptValue,
                                                                    script.location());
        if (cmd)
            commands.addCommand(cmd);
    }
}

void Transformer::rescueChangeTrackingData(const TransformerConstPtr &other)
{
    if (!other)
        return;
    propertiesRequestedInPrepareScript = other->propertiesRequestedInPrepareScript;
    propertiesRequestedInCommands = other->propertiesRequestedInCommands;
    propertiesRequestedFromArtifactInPrepareScript
            = other->propertiesRequestedFromArtifactInPrepareScript;
    propertiesRequestedFromArtifactInCommands = other->propertiesRequestedFromArtifactInCommands;
    importedFilesUsedInPrepareScript = other->importedFilesUsedInPrepareScript;
    importedFilesUsedInCommands = other->importedFilesUsedInCommands;
    depsRequestedInPrepareScript = other->depsRequestedInPrepareScript;
    depsRequestedInCommands = other->depsRequestedInCommands;
    artifactsMapRequestedInPrepareScript = other->artifactsMapRequestedInPrepareScript;
    artifactsMapRequestedInCommands = other->artifactsMapRequestedInCommands;
    lastCommandExecutionTime = other->lastCommandExecutionTime;
    lastPrepareScriptExecutionTime = other->lastPrepareScriptExecutionTime;
    prepareScriptNeedsChangeTracking = other->prepareScriptNeedsChangeTracking;
    commandsNeedChangeTracking = other->commandsNeedChangeTracking;
    markedForRerun = other->markedForRerun;
    exportedModulesAccessedInPrepareScript = other->exportedModulesAccessedInPrepareScript;
    exportedModulesAccessedInCommands = other->exportedModulesAccessedInCommands;
}

Set<QString> Transformer::jobPools() const
{
    Set<QString> pools;
    for (const AbstractCommandPtr &c : commands.commands()) {
        if (!c->jobPool().isEmpty())
            pools.insert(c->jobPool());
    }
    return pools;
}

} // namespace Internal
} // namespace qbs
