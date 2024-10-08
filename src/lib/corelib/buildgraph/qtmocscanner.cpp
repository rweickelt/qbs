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

#include "qtmocscanner.h"

#include "artifact.h"
#include "depscanner.h"
#include "productbuilddata.h"
#include "projectbuilddata.h"
#include "rawscanresults.h"
#include <cppscanner/cppscanner.h>
#include <language/scriptengine.h>
#include <logging/categories.h>
#include <logging/translator.h>
#include <plugins/scanner/scanner.h>
#include <tools/fileinfo.h>
#include <tools/scannerpluginmanager.h>
#include <tools/scripttools.h>

#include <QtCore/qdebug.h>

namespace qbs {
namespace Internal {

struct CommonFileTags
{
    const FileTag cpp = "cpp";
    const FileTag cppCombine = "cpp.combine";
    const FileTag hpp = "hpp";
    const FileTag moc_cpp = "moc_cpp";
    const FileTag moc_cpp_plugin = "moc_cpp_plugin";
    const FileTag moc_hpp_plugin = "moc_hpp_plugin";
    const FileTag moc_hpp = "moc_hpp";
    const FileTag objcpp = "objcpp";
    const FileTag objcppCombine = "objcpp.combine";
};

Q_GLOBAL_STATIC(CommonFileTags, commonFileTags)

static QString qtMocScannerJsName() { return QStringLiteral("QtMocScanner"); }

QtMocScanner::QtMocScanner(const ResolvedProductPtr &product, ScriptEngine *engine, JSValue targetScriptValue)
    : m_engine(engine)
    , m_tags(*commonFileTags())
    , m_product(product)
    , m_targetScriptValue(JS_DupValue(engine->context(), targetScriptValue))
{
    JSValue scannerObj = JS_NewObjectClass(engine->context(), engine->dataWithPtrClass());
    attachPointerTo(scannerObj, this);
    setJsProperty(engine->context(), targetScriptValue, qtMocScannerJsName(), scannerObj);
    JSValue applyFunction = JS_NewCFunction(engine->context(), &js_apply, "QtMocScanner", 1);
    setJsProperty(engine->context(), scannerObj, std::string_view("apply"), applyFunction);
}

QtMocScanner::~QtMocScanner()
{
    setJsProperty(m_engine->context(), m_targetScriptValue, qtMocScannerJsName(), JS_UNDEFINED);
    JS_FreeValue(m_engine->context(), m_targetScriptValue);
}

static RawScanResult runScannerForArtifact(const Artifact *artifact)
{
    const QString &filepath = artifact->filePath();
    RawScanResults &rawScanResults
            = artifact->product->topLevelProject()->buildData->rawScanResults;
    auto predicate = [](const PropertyMapConstPtr &, const PropertyMapConstPtr &) { return true; };
    RawScanResults::ScanData &scanData = rawScanResults.findScanData(
        artifact, qtMocScannerJsName(), artifact->properties, predicate);
    if (scanData.lastScanTime < artifact->timestamp()) {
        FileTags tags = artifact->fileTags();
        if (tags.contains(commonFileTags->cppCombine)) {
            tags.remove(commonFileTags->cppCombine);
            tags.insert(commonFileTags->cpp);
        }
        if (tags.contains(commonFileTags->objcppCombine)) {
            tags.remove(commonFileTags->objcppCombine);
            tags.insert(commonFileTags->objcpp);
        }
        const QByteArray tagsForScanner = tags.toStringList().join(QLatin1Char(',')).toLatin1();
        CppScannerContext context;
        const bool ok = scanCppFile(
            context, filepath, {tagsForScanner.data(), size_t(tagsForScanner.size())}, true, true);
        if (!ok)
            return scanData.rawScanResult;

        scanData.rawScanResult.additionalFileTags.clear();
        scanData.rawScanResult.deps.clear();

        for (const auto tag : additionalFileTags(context))
            scanData.rawScanResult.additionalFileTags += FileTag(tag.data());

        QString baseDirOfInFilePath = artifact->dirPath();
        for (const auto &scanResult : context.includedFiles) {
            int flags = scanResult.flags;
            QString includedFilePath = QString::fromLocal8Bit(
                scanResult.fileName.data(), scanResult.fileName.size());
            if (includedFilePath.isEmpty())
                continue;
            bool isLocalInclude = (flags & SC_LOCAL_INCLUDE_FLAG);
            if (isLocalInclude) {
                QString localFilePath = FileInfo::resolvePath(baseDirOfInFilePath, includedFilePath);
                if (FileInfo::exists(localFilePath))
                    includedFilePath = localFilePath;
            }
            scanData.rawScanResult.deps.emplace_back(includedFilePath);
        }

        scanData.lastScanTime = FileTime::currentTime();
    }
    return scanData.rawScanResult;
}

void QtMocScanner::findIncludedMocCppFiles()
{
    if (!m_includedMocCppFiles.empty())
        return;

    qCDebug(lcMocScan) << "looking for included moc_XXX.cpp files";

    static const FileTags mocCppTags = {m_tags.cpp, m_tags.objcpp};
    for (Artifact *artifact : m_product->lookupArtifactsByFileTags(mocCppTags)) {
        const RawScanResult scanResult = runScannerForArtifact(artifact);
        for (const RawScannedDependency &dependency : scanResult.deps) {
            QString includedFileName = dependency.fileName();
            if (includedFileName.startsWith(QLatin1String("moc_"))
                    && includedFileName.endsWith(QLatin1String(".cpp"))) {
                qCDebug(lcMocScan) << artifact->fileName() << "includes" << includedFileName;
                includedFileName.remove(0, 4);
                includedFileName.chop(4);
                m_includedMocCppFiles.insert(includedFileName, artifact->fileName());
            }
        }
    }
}

JSValue QtMocScanner::js_apply(JSContext *ctx, JSValue this_val, int argc, JSValue *argv)
{
    if (argc < 1)
        return throwError(ctx, Tr::tr("QtMocScanner.apply() requires an argument."));
    ScriptEngine * const engine = ScriptEngine::engineForContext(ctx);
    const auto scanner = attachedPointer<QtMocScanner>(this_val, engine->dataWithPtrClass());
    return scanner->apply(engine, attachedPointer<Artifact>(argv[0], engine->dataWithPtrClass()));
}

JSValue QtMocScanner::apply(ScriptEngine *engine, const Artifact *artifact)
{
    qCDebug(lcMocScan).noquote() << "scanning" << artifact->toString();

    bool hasQObjectMacro = false;
    bool mustCompile = false;
    bool hasPluginMetaDataMacro = false;
    const bool isHeaderFile = artifact->fileTags().contains(m_tags.hpp);

    RawScanResult scanResult = runScannerForArtifact(artifact);
    if (scanResult.additionalFileTags.empty() && artifact->fileTags().contains("mocable")) {
        if (isHeaderFile) {
            scanResult.additionalFileTags.insert(m_tags.moc_hpp);
        } else if (artifact->fileTags().contains(m_tags.cpp)
                   || artifact->fileTags().contains(m_tags.cppCombine)
                   || artifact->fileTags().contains(m_tags.objcpp)
                   || artifact->fileTags().contains(m_tags.objcppCombine)) {
            scanResult.additionalFileTags.insert(m_tags.moc_cpp);
        }
    }
    if (!scanResult.additionalFileTags.empty()) {
        if (isHeaderFile) {
            if (scanResult.additionalFileTags.contains(m_tags.moc_hpp))
                hasQObjectMacro = true;
            if (scanResult.additionalFileTags.contains(m_tags.moc_hpp_plugin)) {
                hasQObjectMacro = true;
                hasPluginMetaDataMacro = true;
            }
            findIncludedMocCppFiles();
            if (!m_includedMocCppFiles.contains(FileInfo::completeBaseName(artifact->fileName())))
                mustCompile = true;
        } else {
            if (scanResult.additionalFileTags.contains(m_tags.moc_cpp))
                hasQObjectMacro = true;
            if (scanResult.additionalFileTags.contains(m_tags.moc_cpp_plugin)) {
                hasQObjectMacro = true;
                hasPluginMetaDataMacro = true;
            }
        }
    }

    qCDebug(lcMocScan) << "hasQObjectMacro:" << hasQObjectMacro
                          << "mustCompile:" << mustCompile
                          << "hasPluginMetaDataMacro:" << hasPluginMetaDataMacro;

    JSValue obj = engine->newObject();
    JSContext * const ctx = m_engine->context();
    setJsProperty(ctx, obj, std::string_view("hasQObjectMacro"), JS_NewBool(ctx, hasQObjectMacro));
    setJsProperty(ctx, obj, std::string_view("mustCompile"), JS_NewBool(ctx, mustCompile));
    setJsProperty(
        ctx,
        obj,
        std::string_view("hasPluginMetaDataMacro"),
        JS_NewBool(ctx, hasPluginMetaDataMacro));
    engine->setUsesIo();
    return obj;
}

} // namespace Internal
} // namespace qbs
