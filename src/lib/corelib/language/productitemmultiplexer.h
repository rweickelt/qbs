/****************************************************************************
**
** Copyright (C) 2023 The Qt Company Ltd.
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

#pragma once

#include <QList>
#include <QVariantMap>

#include <functional>

namespace qbs {
class SetupProjectParameters;
namespace Internal {
class Evaluator;
class Item;
class Logger;

// This class deals with product multiplexing over the various defined axes.
// For instance, a product with qbs.architectures: ["x86", "arm"] will get multiplexed into
// two products with qbs.architecture: "x86" and qbs.architecture: "arm", respectively.
class ProductItemMultiplexer
{
public:
    using QbsItemRetriever = std::function<Item *(Item *)>;
    ProductItemMultiplexer(const SetupProjectParameters &parameters, Evaluator &evaluator,
                           Logger &logger, const QbsItemRetriever &qbsItemRetriever);
    ~ProductItemMultiplexer();

    // Checks whether the product item is to be multiplexed and returns the list of additional
    // product items. In the normal, non-multiplex case, this list is empty.
    QList<Item *> multiplex(
        const QString &productName,
        Item *productItem,
        Item *tempQbsModuleItem,
        const std::function<void()> &dropTempQbsModule
        );

    QVariantMap multiplexIdToVariantMap(const QString &multiplexId);

    static QString fullProductDisplayName(const QString &name, const QString &multiplexId);

private:
    class Private;
    Private * const d;
};

} // namespace Internal
} // namespace qbs