/****************************************************************************
**
** Copyright (C) 2026 Ivan Komissarov (abbapoh@gmail.com)
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

#include "settingsjson.h"

#include <QtCore/qdatastream.h>
#include <QtCore/qiodevice.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonobject.h>

#include <algorithm>

namespace qbs {
namespace Internal {

namespace {
QString serializeVariant(const QVariant &value)
{
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream << value;
    return QString::fromLatin1("@Variant(" + data.toBase64() + ')');
}

bool deserializeVariant(const QJsonValue &value, QVariant &result)
{
    if (!value.isString())
        return false;
    const QString string = value.toString();
    if (string.startsWith(QLatin1String("@@"))) {
        result = string.mid(1);
        return true;
    }
    if (!string.startsWith(QLatin1String("@Variant(")) || !string.endsWith(QLatin1Char(')')))
        return false;
    QByteArray data = QByteArray::fromBase64(string.mid(9, string.size() - 10).toLatin1());
    QDataStream stream(&data, QIODevice::ReadOnly);
    stream >> result;
    return stream.status() == QDataStream::Ok && stream.atEnd();
}

QJsonValue settingsValueToJson(const QVariant &value)
{
    if (value.userType() == QMetaType::QVariantMap || value.userType() == QMetaType::QVariantHash) {
        return serializeVariant(value);
    }
    if (value.userType() == QMetaType::QString && value.toString().startsWith(QLatin1Char('@')))
        return {(QLatin1Char('@') + value.toString())};
    return QJsonValue::fromVariant(value);
}

void readJsonValue(const QJsonValue &value, const QString &key, QSettings::SettingsMap &map)
{
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it)
            readJsonValue(it.value(), key + it.key() + QLatin1Char('/'), map);
        return;
    }

    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        const bool isSettingsArray = !array.isEmpty()
                                     && std::all_of(
                                         array.cbegin(), array.cend(), [](const QJsonValue &item) {
                                             return item.isObject();
                                         });
        if (isSettingsArray) {
            map.insert(key + QStringLiteral("size"), array.size());
            for (int i = 0; i < array.size(); ++i)
                readJsonValue(array.at(i), key + QString::number(i + 1) + QLatin1Char('/'), map);
            return;
        }
    }

    QVariant variant;
    if (!deserializeVariant(value, variant))
        variant = value.toVariant();
    map.insert(key.chopped(1), variant);
}

bool readJsonFile(QIODevice &device, QSettings::SettingsMap &map)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(device.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    readJsonValue(document.object(), {}, map);
    return true;
}

QJsonValue writeJsonValue(const QJsonValue &root, const QString &key, const QVariant &value)
{
    const int slashPos = key.indexOf(QLatin1Char('/'));
    QJsonObject object = root.toObject();
    if (slashPos < 0) {
        if (key == QLatin1String("size")) {
            bool ok = false;
            const int size = value.toInt(&ok);
            QJsonArray array;
            if (ok && size >= 0) {
                for (int i = 1; i <= size; ++i) {
                    const auto it = object.constFind(QString::number(i));
                    if (it == object.constEnd())
                        break;
                    array.append(it.value());
                }
                if (array.size() == size)
                    return array;
            }
        }
        object.insert(key, settingsValueToJson(value));
        return object;
    }

    const QString groupName = key.left(slashPos);
    object.insert(groupName, writeJsonValue(object.value(groupName), key.mid(slashPos + 1), value));
    return object;
}

bool writeJsonFile(QIODevice &device, const QSettings::SettingsMap &map)
{
    QJsonValue result;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        const QString key = it.key().chopped(it.key().endsWith(QLatin1Char('/')) ? 1 : 0);
        if (!key.isEmpty())
            result = writeJsonValue(result, key, it.value());
    }
    const QByteArray data = QJsonDocument(result.toObject()).toJson();
    return device.write(data) == data.size();
}
} // namespace

QSettings::Format jsonSettingsFormat()
{
    static const QSettings::Format value = QSettings::registerFormat(
        QStringLiteral("json"), readJsonFile, writeJsonFile);
    return value;
}

} // namespace Internal
} // namespace qbs
