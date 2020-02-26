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
#include <tools/fileinfo.h>

#include <QtCore/qdir.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qobject.h>

#include <QtQml/qjsvalue.h>

namespace qbs {
namespace Internal {

class File : public JsExtension
{
    Q_OBJECT
public:
    enum Filter {
        Dirs = 0x001,
        Files = 0x002,
        Drives = 0x004,
        NoSymLinks = 0x008,
        AllEntries = Dirs | Files | Drives,
        TypeMask = 0x00f,

        Readable = 0x010,
        Writable = 0x020,
        Executable = 0x040,
        PermissionMask = 0x070,

        Modified = 0x080,
        Hidden = 0x100,
        System = 0x200,

        AccessMask  = 0x3F0,

        AllDirs = 0x400,
        CaseSensitive = 0x800,
        NoDot = 0x2000,
        NoDotDot = 0x4000,
        NoDotAndDotDot = NoDot | NoDotDot,

        NoFilter = -1
    };
    Q_DECLARE_FLAGS(Filters, Filter)
    Q_FLAG(Filters)

    Q_INVOKABLE QString canonicalFilePath(const QString &filePath);
    Q_INVOKABLE bool copy(const QString &sourceFilePath, const QString &targetFilePath);
    Q_INVOKABLE QStringList directoryEntries(const QString &path, Filters filtersJS);
    Q_INVOKABLE bool exists(const QString &filePath);
    Q_INVOKABLE double lastModified(const QString &filePath);
    Q_INVOKABLE bool makePath(const QString &path);
    Q_INVOKABLE bool move(const QString &oldPath, const QString &newPath, bool overwrite = true);
    Q_INVOKABLE bool remove(const QString &filePath);
};

QString File::canonicalFilePath(const QString &filePath)
{
    QString result = QFileInfo(filePath).canonicalFilePath();
    engine()->addCanonicalFilePathResult(filePath, result);
    return result;
}

bool File::copy(const QString &source, const QString &destination)
{
    auto se = engine();
    const DubiousContextList dubiousContexts({
            DubiousContext(EvalContext::PropertyEvaluation),
            DubiousContext(EvalContext::RuleExecution, DubiousContext::SuggestMoving)
    });
    se->checkContext(QStringLiteral("File.copy()"), dubiousContexts);

    QString errorMessage;
    if (Q_UNLIKELY(!copyFileRecursion(source, destination, true, true, &errorMessage))) {
        se->throwError(errorMessage);
        return false;
    }

    return true;
}

QStringList File::directoryEntries(const QString &path, File::Filters filters)
{
    auto se = engine();
    const DubiousContextList dubiousContexts({
            DubiousContext(EvalContext::PropertyEvaluation, DubiousContext::SuggestMoving)
    });
    se->checkContext(QStringLiteral("File.directoryEntries()"), dubiousContexts);

    using FilterIntType = std::underlying_type<QDir::Filter>::type;
    auto filtersInt = static_cast<FilterIntType>(filters);
    QDir dir(path);
    const QStringList entries = dir.entryList(QDir::Filters(filtersInt), QDir::Name);
    se->addDirectoryEntriesResult(path, QDir::Filters(filtersInt), entries);
    return entries;
}

bool File::exists(const QString &filePath)
{
    const bool exists = FileInfo::exists(filePath);
    engine()->addFileExistsResult(filePath, exists);
    return exists;
}

double File::lastModified(const QString &filePath)
{
    const FileTime timestamp = FileInfo(filePath).lastModified();
    engine()->addFileLastModifiedResult(filePath, timestamp);
    return timestamp.asDouble();
}

bool File::makePath(const QString &path)
{
    const DubiousContextList dubiousContexts({ DubiousContext(EvalContext::PropertyEvaluation) });
    engine()->checkContext(QStringLiteral("File.makePath()"), dubiousContexts);

    return QDir::root().mkpath(path);
}

bool File::move(const QString &oldPath, const QString &newPath, bool overwrite)
{
    auto se = engine();
    const DubiousContextList dubiousContexts({ DubiousContext(EvalContext::PropertyEvaluation) });
    se->checkContext(QStringLiteral("File.move()"), dubiousContexts);

    if (Q_UNLIKELY(QFileInfo(oldPath).isDir())) {
        se->throwError(QStringLiteral("Could not move '%1' to '%2': "
                                      "Source file path is a directory.")
                       .arg(oldPath, newPath));
        return false;
    }

    if (Q_UNLIKELY(QFileInfo(newPath).isDir())) {
        se->throwError(QStringLiteral("Could not move '%1' to '%2': "
                                      "Destination file path is a directory.")
                       .arg(oldPath, newPath));
        return false;
    }

    QFile f(newPath);
    if (overwrite && f.exists() && !f.remove())
        se->throwError(QStringLiteral("Could not move '%1' to '%2': %3")
                       .arg(oldPath, newPath, f.errorString()));

    if (QFile::exists(newPath)) {
        se->throwError(QStringLiteral("Could not move '%1' to '%2': "
                                      "Destination file exists.")
                       .arg(oldPath, newPath));
        return false;
    }

    QFile f2(oldPath);
    if (Q_UNLIKELY(!f2.rename(newPath))) {
        se->throwError(QStringLiteral("Could not move '%1' to '%2': %3")
                       .arg(oldPath, newPath, f2.errorString()));
        return false;
    }

    return true;
}

bool File::remove(const QString &filePath)
{
    auto se = engine();
    const DubiousContextList dubiousContexts({ DubiousContext(EvalContext::PropertyEvaluation) });
    se->checkContext(QStringLiteral("File.remove()"), dubiousContexts);

    QString errorMessage;
    if (Q_UNLIKELY(!removeFileRecursion(QFileInfo(filePath), &errorMessage))) {
        se->throwError(errorMessage);
        return false;
    }
    return true;
}

#define COPY_ENUM_PROPERTY(name) \
    extension.setProperty(QStringLiteral(name), proxy.property(QStringLiteral(name)))

QJSValue createFileExtension(QJSEngine *engine)
{
    QJSValue extension = engine->newQObject(new File());

    // We need to make enums available in a class that is treated as a singleton.
    // This use-case seems to be unsupported by QJSEngine. The workaround is to
    // copy all enum properties from the meta object proxy to the singleton.
    QJSValue proxy = engine->newQMetaObject(&File::staticMetaObject);
    COPY_ENUM_PROPERTY("Files");
    COPY_ENUM_PROPERTY("Dirs");
    COPY_ENUM_PROPERTY("Files");
    COPY_ENUM_PROPERTY("Drives");
    COPY_ENUM_PROPERTY("NoSymLinks");
    COPY_ENUM_PROPERTY("AllEntries");
    COPY_ENUM_PROPERTY("TypeMask");

    COPY_ENUM_PROPERTY("Readable");
    COPY_ENUM_PROPERTY("Writable");
    COPY_ENUM_PROPERTY("Executable");
    COPY_ENUM_PROPERTY("PermissionMask");

    COPY_ENUM_PROPERTY("Modified");
    COPY_ENUM_PROPERTY("Hidden");
    COPY_ENUM_PROPERTY("System");

    COPY_ENUM_PROPERTY("AccessMask ");

    COPY_ENUM_PROPERTY("AllDirs");
    COPY_ENUM_PROPERTY("CaseSensitive");
    COPY_ENUM_PROPERTY("NoDot");
    COPY_ENUM_PROPERTY("NoDotDot");
    COPY_ENUM_PROPERTY("NoDotAndDotDot");

    COPY_ENUM_PROPERTY("NoFilter");

    return extension;
}

QBS_REGISTER_JS_EXTENSION("File", createFileExtension)

}
}

#include "file.moc"
