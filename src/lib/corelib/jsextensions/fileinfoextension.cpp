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
#include <tools/stringconstants.h>

#include <QtCore/qdir.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qobject.h>

#include <QtQml/qjsvalue.h>

#include <regex>

namespace qbs {
namespace Internal {

// removes duplicate separators from the path
static QString uniqueSeparators(QString path)
{
    const auto it = std::unique(path.begin(), path.end(), [](QChar c1, QChar c2) {
        return c1 == c2 && c1 == QLatin1Char('/');
    });
    path.resize(int(it - path.begin()));
    return path;
}

class FileInfoExtension: public JsExtension
{
    Q_OBJECT
public:
    Q_INVOKABLE QJSValue path(const QJSValue &path, const QStringList &hostOs = QStringList());
    Q_INVOKABLE QJSValue fileName(const QJSValue &path);
    Q_INVOKABLE QJSValue baseName(const QJSValue &path);
    Q_INVOKABLE QJSValue suffix(const QJSValue &path);
    Q_INVOKABLE QJSValue completeSuffix(const QJSValue &path);
    Q_INVOKABLE QJSValue canonicalPath(const QJSValue &path);
    Q_INVOKABLE QJSValue cleanPath(const QJSValue &path);
    Q_INVOKABLE QJSValue completeBaseName(const QJSValue &path);
    Q_INVOKABLE QJSValue relativePath(const QJSValue &baseDir, const QJSValue &filePath);
    Q_INVOKABLE QJSValue resolvePath(const QJSValue &baseJS, const QJSValue &relJS);
    Q_INVOKABLE QJSValue isAbsolutePath(const QJSValue &path, const QStringList &hostOS = QStringList());
    Q_INVOKABLE QJSValue toWindowsSeparators(const QJSValue &path);
    Q_INVOKABLE QJSValue fromWindowsSeparators(const QJSValue &path);
    Q_INVOKABLE QJSValue toNativeSeparators(const QJSValue &path);
    Q_INVOKABLE QJSValue fromNativeSeparators(const QJSValue &path);
    Q_INVOKABLE QString joinPaths(const QString &s1,
                                  const QString &s2 = QString(),
                                  const QString &s3 = QString(),
                                  const QString &s4 = QString(),
                                  const QString &s5 = QString(),
                                  const QString &s6 = QString(),
                                  const QString &s7 = QString(),
                                  const QString &s8 = QString(),
                                  const QString &s9 = QString(),
                                  const QString &sa = QString(),
                                  const QString &sb = QString(),
                                  const QString &sc = QString(),
                                  const QString &sd = QString(),
                                  const QString &se = QString(),
                                  const QString &sf = QString()
                                  );

};

QJSValue FileInfoExtension::path(const QJSValue &path, const QStringList &hostOS)
{
    HostOsInfo::HostOs hostOs = HostOsInfo::hostOs();
    if (!hostOS.isEmpty()) {
        hostOs = hostOS.contains(QLatin1String("windows"))
                ? HostOsInfo::HostOsWindows : HostOsInfo::HostOsOtherUnix;
    }
    return FileInfo::path(path.toString(), hostOs);
}

QJSValue FileInfoExtension::fileName(const QJSValue &path)
{
    if (path.isUndefined())
        return QJSValue();

    return FileInfo::fileName(path.toString());
}

QJSValue FileInfoExtension::baseName(const QJSValue &path)
{
    if (path.isUndefined())
        return QJSValue();

    return FileInfo::baseName(path.toString());
}

QJSValue FileInfoExtension::suffix(const QJSValue &path)
{
    if (path.isUndefined())
        return QJSValue();

    return FileInfo::suffix(path.toString());
}

QJSValue FileInfoExtension::completeSuffix(const QJSValue &path)
{
    if (path.isUndefined())
        return QJSValue();

    return FileInfo::completeSuffix(path.toString());
}

QJSValue FileInfoExtension::canonicalPath(const QJSValue &path)
{
    if (path.isUndefined())
        return QJSValue();

    return QFileInfo(path.toString()).canonicalFilePath();
}

QJSValue FileInfoExtension::cleanPath(const QJSValue &path)
{
    if (path.isUndefined())
        return QJSValue();

    return QDir::cleanPath(path.toString());
}

QJSValue FileInfoExtension::completeBaseName(const QJSValue &path)
{
    if (path.isUndefined())
        return QJSValue();

    return FileInfo::completeBaseName(path.toString());
}

QJSValue FileInfoExtension::relativePath(const QJSValue &baseDirJS, const QJSValue &filePathJS)
{
    if (baseDirJS.isUndefined() || filePathJS.isUndefined())
        return QJSValue();

    const QString baseDir = baseDirJS.toString();
    const QString filePath = filePathJS.toString();
    if (!FileInfo::isAbsolute(baseDir)) {
        qjsEngine(this)->throwError(QJSValue::SyntaxError,
                                   Tr::tr("FileInfo.relativePath() expects an absolute path as "
                                          "its first argument, but it is '%1'.").arg(baseDir));
        return QJSValue();
    }
    if (!FileInfo::isAbsolute(filePath)) {
        qjsEngine(this)->throwError(QJSValue::SyntaxError,
                                   Tr::tr("FileInfo.relativePath() expects an absolute path as "
                                          "its second argument, but it is '%1'.").arg(filePath));
    }
    return QDir(baseDir).relativeFilePath(filePath);
}

QJSValue FileInfoExtension::resolvePath(const QJSValue &baseJS, const QJSValue &relJS)
{
    if (baseJS.isUndefined() || relJS.isUndefined())
        return QJSValue();

    const QString base = baseJS.toString();
    const QString rel = relJS.toString();
    return FileInfo::resolvePath(base, rel);
}

QJSValue FileInfoExtension::isAbsolutePath(const QJSValue &path, const QStringList &hostOS)
{
    if (path.isUndefined())
        return QJSValue();

    HostOsInfo::HostOs hostOs = HostOsInfo::hostOs();
    if (!hostOS.isEmpty()) {
        hostOs = hostOS.contains(QLatin1String("windows"))
                ? HostOsInfo::HostOsWindows : HostOsInfo::HostOsOtherUnix;
    }
    return FileInfo::isAbsolute(path.toString(), hostOs);
}

QJSValue FileInfoExtension::toWindowsSeparators(const QJSValue &path)
{
    if (path.isUndefined())
        return QJSValue();

    return path.toString().replace(QLatin1Char('/'), QLatin1Char('\\'));
}

QJSValue FileInfoExtension::fromWindowsSeparators(const QJSValue &path)
{
    if (path.isUndefined())
        return path;

    return path.toString().replace(QLatin1Char('\\'), QLatin1Char('/'));
}

QJSValue FileInfoExtension::toNativeSeparators(const QJSValue &path)
{
    if (path.isUndefined())
        return QJSValue();

    return QDir::toNativeSeparators(path.toString());
}


QJSValue FileInfoExtension::fromNativeSeparators(const QJSValue &path)
{
    if (path.isUndefined())
        return path;

    return QDir::fromNativeSeparators(path.toString());
}

QString FileInfoExtension::joinPaths(const QString &s1,
                                       const QString &s2,
                                       const QString &s3,
                                       const QString &s4,
                                       const QString &s5,
                                       const QString &s6,
                                       const QString &s7,
                                       const QString &s8,
                                       const QString &s9,
                                       const QString &sa,
                                       const QString &sb,
                                       const QString &sc,
                                       const QString &sd,
                                       const QString &se,
                                       const QString &sf)
{
    QStringList parts;
    if (!s1.isEmpty())
        parts << s1;
    if (!s2.isEmpty())
        parts << s2;
    if (!s3.isEmpty())
        parts << s3;
    if (!s4.isEmpty())
        parts << s4;
    if (!s5.isEmpty())
        parts << s5;
    if (!s6.isEmpty())
        parts << s6;
    if (!s7.isEmpty())
        parts << s7;
    if (!s8.isEmpty())
        parts << s8;
    if (!s9.isEmpty())
        parts << s9;
    if (!sa.isEmpty())
        parts << sa;
    if (!sb.isEmpty())
        parts << sb;
    if (!sc.isEmpty())
        parts << sc;
    if (!sd.isEmpty())
        parts << sd;
    if (!se.isEmpty())
        parts << se;
    if (!sf.isEmpty())
        engine()->throwError(Tr::tr("'FileInfo.joinPaths()' can only handle 15 arguments."));

    return uniqueSeparators(parts.join(QLatin1Char('/')));
}

QJSValue createFileInfoExtension(QJSEngine *engine)
{
    return engine->newQObject(new FileInfoExtension());
}

QBS_REGISTER_JS_EXTENSION("FileInfo", createFileInfoExtension)

}
}

#include "fileinfoextension.moc"
