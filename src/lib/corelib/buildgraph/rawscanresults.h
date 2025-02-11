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

#ifndef QBS_RAWSCANRESULTS_H
#define QBS_RAWSCANRESULTS_H

#include "rawscanneddependency.h"

#include <language/filetags.h>
#include <language/forward_decls.h>
#include <language/propertymapinternal.h>
#include <tools/filetime.h>
#include <tools/persistence.h>

#include <QtCore/qhash.h>
#include <QtCore/qstring.h>

#include <vector>

namespace qbs {
namespace Internal {
class DependencyScanner;
class FileResourceBase;

class RawScanResult
{
public:
    std::vector<RawScannedDependency> deps;
    FileTags additionalFileTags;
    // TODO: does this belong here?
    QString providesModule;
    bool isInterfaceModule{false};
    QStringList requiresModules;

    template<PersistentPool::OpType opType>
    void completeSerializationOp(PersistentPool &pool)
    {
        pool.serializationOp<opType>(
            deps, additionalFileTags, providesModule, isInterfaceModule, requiresModules);
    }
};

class RawScanResults
{
public:
    struct ScanData
    {
        QString scannerId;
        PropertyMapConstPtr moduleProperties;
        FileTime lastScanTime;
        RawScanResult rawScanResult;

        template<PersistentPool::OpType opType> void completeSerializationOp(PersistentPool &pool)
        {
            pool.serializationOp<opType>(scannerId, moduleProperties, lastScanTime, rawScanResult);
        }
    };

    using FilterFunction
        = std::function<bool(const PropertyMapConstPtr &, const PropertyMapConstPtr)>;
    ScanData &findScanData(
        const FileResourceBase *file,
        const QString &scannerId,
        const PropertyMapConstPtr &moduleProperties,
        const FilterFunction &filter);

    ScanData &findScanData(
        const FileResourceBase *file,
        const DependencyScanner *scanner,
        const PropertyMapConstPtr &moduleProperties);

    template<PersistentPool::OpType opType> void completeSerializationOp(PersistentPool &pool)
    {
        pool.serializationOp<opType>(m_rawScanData);
    }

private:
    QHash<QString, std::vector<ScanData>> m_rawScanData;
};

} // namespace Internal
} // namespace qbs

#endif // Include guard
