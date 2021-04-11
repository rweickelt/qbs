/****************************************************************************
**
** Copyright (C) 2015 Petroules Corporation.
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
#include "propertylistutils.h"
#include "propertylist_darwin.h"

#include <language/scriptengine.h>
#include <tools/hostosinfo.h>

#include <QtCore/qfile.h>
#include <QtCore/qobject.h>
#include <QtCore/qstring.h>
#include <QtCore/qtextstream.h>
#include <QtCore/qvariant.h>

#include <QtQml/qjsvalue.h>

// Same values as CoreFoundation and Foundation APIs
enum {
    QPropertyListOpenStepFormat = 1,
    QPropertyListXMLFormat_v1_0 = 100,
    QPropertyListBinaryFormat_v1_0 = 200,
    QPropertyListJSONFormat = 1000 // If this conflicts someday, just change it :)
};

namespace qbs {
namespace Internal {

class PropertyListPrivate
{
public:
    PropertyListPrivate();

    QVariant propertyListObject;
    int propertyListFormat;

    void readFromData(QJSEngine *engine, const QByteArray &data);
    QByteArray writeToData(QJSEngine *engine, const QString &format);
};

PropertyListPrivate::PropertyListPrivate()
    : propertyListObject(), propertyListFormat(0)
{
}

void PropertyListPrivate::readFromData(QJSEngine *engine, const QByteArray &data)
{
    @autoreleasepool {
        NSPropertyListFormat format;
        int internalFormat = 0;
        NSString *errorString = nil;
        id plist = [NSPropertyListSerialization propertyListWithData:data.toNSData()
                                                             options:0
                                                              format:&format error:nil];
        if (plist) {
            internalFormat = format;
        } else {
            NSError *error = nil;
            plist = [NSJSONSerialization JSONObjectWithData:data.toNSData()
                                                    options:0
                                                      error:&error];
            if (Q_UNLIKELY(!plist)) {
                errorString = [error localizedDescription];
            } else {
                internalFormat = QPropertyListJSONFormat;
            }
        }

        if (Q_UNLIKELY(!plist)) {
            engine->throwError(QString::fromNSString(errorString));
        } else {
            QVariant obj = QPropertyListUtils::fromPropertyList(plist);
            if (!obj.isNull()) {
                propertyListObject = obj;
                propertyListFormat = internalFormat;
            } else {
                engine->throwError(QStringLiteral("error converting property list"));
            }
        }
    }
}

QByteArray PropertyListPrivate::writeToData(QJSEngine *engine, const QString &format)
{
    @autoreleasepool {
        NSError *error = nil;
        NSString *errorString = nil;
        NSData *data = nil;

        id obj = QPropertyListUtils::toPropertyList(propertyListObject);
        if (!obj) {
            engine->throwError(QStringLiteral("error converting property list"));
            return QByteArray();
        }

        if (format == QLatin1String("json") || format == QLatin1String("json-pretty") ||
            format == QLatin1String("json-compact")) {
            if ([NSJSONSerialization isValidJSONObject:obj]) {
                error = nil;
                errorString = nil;
                const NSJSONWritingOptions options = format == QLatin1String("json-pretty")
                        ? NSJSONWritingPrettyPrinted : 0;
                data = [NSJSONSerialization dataWithJSONObject:obj
                                                       options:options
                                                         error:&error];
                if (Q_UNLIKELY(!data)) {
                    errorString = [error localizedDescription];
                }
            } else {
                errorString = @"Property list object cannot be converted to JSON data";
            }
        } else if (format == QLatin1String("xml1") || format == QLatin1String("binary1")) {
            const NSPropertyListFormat plistFormat = format == QLatin1String("xml1")
                                                          ? NSPropertyListXMLFormat_v1_0
                                                          : NSPropertyListBinaryFormat_v1_0;

            error = nil;
            errorString = nil;
            data = [NSPropertyListSerialization dataWithPropertyList:obj
                                                              format:plistFormat
                                                             options:0
                                                               error:&error];
            if (Q_UNLIKELY(!data)) {
                errorString = [error localizedDescription];
            }
        } else {
            errorString = [NSString stringWithFormat:@"Property lists cannot be written in the '%s' "
                                                     @"format", format.toUtf8().constData()];
        }

        if (Q_UNLIKELY(!data)) {
            engine->throwError(QString::fromNSString(errorString));
        }

        return QByteArray::fromNSData(data);
    }
}

PropertyList::PropertyList(QObject *engine)
    : d(new PropertyListPrivate)
{
    auto *se = qobject_cast<ScriptEngine *>(engine);
    const DubiousContextList dubiousContexts({
            DubiousContext(EvalContext::PropertyEvaluation, DubiousContext::SuggestMoving)
    });
    se->checkContext(QStringLiteral("qbs.PropertyList"), dubiousContexts);
    Q_ASSERT(se);
}

PropertyList::~PropertyList()
{
    delete d;
}

bool PropertyList::isEmpty() const
{
    return d->propertyListObject.isNull();
}

void PropertyList::clear()
{
    d->propertyListObject = QVariant();
    d->propertyListFormat = 0;
}

void PropertyList::readFromObject(const QJSValue &value)
{
    d->propertyListObject = value.toVariant();
    d->propertyListFormat = 0; // wasn't deserialized from any external format
}

void PropertyList::readFromString(const QString &input)
{
    readFromData(input.toUtf8());
}

void PropertyList::readFromFile(const QString &filePath)
{
    QFile file(filePath);
    if (file.open(QIODevice::ReadOnly)) {
        const QByteArray data = file.readAll();
        if (file.error() == QFile::NoError) {
            d->readFromData(engine(), data);
            return;
        }
    }

    engine()->throwError(QStringLiteral("%1: %2").arg(filePath).arg(file.errorString()));
}

void PropertyList::readFromData(const QByteArray &data)
{
    d->readFromData(engine(), data);
}

void PropertyList::writeToFile(const QString &filePath, const QString &plistFormat)
{
    QFile file(filePath);
    QByteArray data = d->writeToData(engine(), plistFormat);
    if (Q_LIKELY(!data.isEmpty())) {
        if (file.open(QIODevice::WriteOnly) && file.write(data) == data.size()) {
            return;
        }
    }

    engine()->throwError(QStringLiteral("%1: %2").arg(filePath).arg(file.errorString()));
}

QJSValue PropertyList::format() const
{
    switch (d->propertyListFormat)
    {
    case QPropertyListOpenStepFormat:
        return QStringLiteral("openstep");
    case QPropertyListXMLFormat_v1_0:
        return QStringLiteral("xml1");
    case QPropertyListBinaryFormat_v1_0:
        return QStringLiteral("binary1");
    case QPropertyListJSONFormat:
        return QStringLiteral("json");
    default:
        return QJSValue();
    }
}

QJSValue PropertyList::toObject() const
{
    return engine()->toScriptValue(d->propertyListObject);
}

QString PropertyList::toFormattedString(const QString &plistFormat) const
{
    if (plistFormat == QLatin1String("binary1")) {
        engine()->throwError(QStringLiteral("Property list object cannot be converted to a "
                                            "string in the binary1 format; this format can only "
                                            "be written directly to a file"));
        return {};
    }

    if (!isEmpty())
        return QString::fromUtf8(d->writeToData(engine(), plistFormat));

    return {};
}

QString PropertyList::toXMLString() const
{
    return toFormattedString(QStringLiteral("xml1"));
}

QString PropertyList::toJSON(const QString &style) const
{
    QString format = QLatin1String("json");
    if (!style.isEmpty())
        format += QLatin1String("-") + style;

    return toFormattedString(format);
}

QJSValue createPropertyListExtension(QJSEngine *engine)
{
    QJSValue mo = engine->newQMetaObject(&PropertyList::staticMetaObject);
    QJSValue factory = engine->evaluate(
                QStringLiteral("(function(m, e){ return function(){ return new m(e); } })"));
    QJSValue wrapper = factory.call(QJSValueList{mo, engine->toScriptValue(engine)});
    return wrapper;
}

QBS_REGISTER_JS_EXTENSION("PropertyList", createPropertyListExtension)

}
}
