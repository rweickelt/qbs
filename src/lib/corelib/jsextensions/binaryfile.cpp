/****************************************************************************
**
** Copyright (C) 2017 Sergey Belyashov <sergey.belyashov@gmail.com>
** Copyright (C) 2017 Denis Shienkov <denis.shienkov@gmail.com>
** Copyright (C) 2017 The Qt Company Ltd.
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
#include <tools/hostosinfo.h>

#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qobject.h>
#include <QtCore/qvariant.h>
#include <QtQml/qjsvalue.h>

namespace qbs {
namespace Internal {

class BinaryFile : public JsExtension, public ResourceAcquiringScriptObject
{
    Q_OBJECT
    Q_ENUMS(OpenMode)
public:
    enum OpenMode
    {
        ReadOnly = 1,
        WriteOnly = 2,
        ReadWrite = ReadOnly | WriteOnly
    };

    Q_INVOKABLE BinaryFile(QObject *engine, const QJSValue &filePath,
                                      OpenMode mode = ReadOnly);
    ~BinaryFile() override;

    Q_INVOKABLE void close();
    Q_INVOKABLE QString filePath();
    Q_INVOKABLE bool atEof() const;
    Q_INVOKABLE qint64 size() const;
    Q_INVOKABLE void resize(qint64 size);
    Q_INVOKABLE qint64 pos() const;
    Q_INVOKABLE void seek(qint64 pos);
    Q_INVOKABLE QVariantList read(qint64 size);
    Q_INVOKABLE void write(const QVariantList &data);

private:
    bool checkForClosed() const;

    // ResourceAcquiringScriptObject implementation
    void releaseResources() override;

    QFile *m_file;
};

BinaryFile::BinaryFile(QObject *engine, const QJSValue &filePath,
                                             OpenMode mode)
    : ResourceAcquiringScriptObject(qobject_cast<ScriptEngine *>(engine))
{
    auto *se = qobject_cast<ScriptEngine *>(engine);

    QIODevice::OpenMode m = QIODevice::NotOpen;
    switch (mode) {
    case ReadWrite:
        m = QIODevice::ReadWrite;
        break;
    case ReadOnly:
        m = QIODevice::ReadOnly;
        break;
    case WriteOnly:
        m = QIODevice::WriteOnly;
        break;
    default:
        se->throwError(Tr::tr("Unable to open file '%1': Undefined mode '%2'")
                       .arg(filePath.toString(), mode));
        return;
    }

    m_file = new QFile(filePath.toString());
    if (Q_UNLIKELY(!m_file->open(m))) {
        se->throwError(Tr::tr("Unable to open file '%1': %2").arg(filePath.toString()));
        delete m_file;
        m_file = nullptr;
        return;
    }

    se->addResourceAcquiringScriptObject(this);
    const DubiousContextList dubiousContexts({
            DubiousContext(EvalContext::PropertyEvaluation, DubiousContext::SuggestMoving)
    });
    se->checkContext(QStringLiteral("qbs.BinaryFile"), dubiousContexts);
    se->setUsesIo();
}

BinaryFile::~BinaryFile()
{
    delete m_file;
}

void BinaryFile::close()
{
    if (checkForClosed())
        return;
    m_file->close();
    delete m_file;
    m_file = nullptr;
}

QString BinaryFile::filePath()
{
    if (checkForClosed())
        return {};
    return QFileInfo(*m_file).absoluteFilePath();
}

bool BinaryFile::atEof() const
{
    if (checkForClosed())
        return true;
    return m_file->atEnd();
}

qint64 BinaryFile::size() const
{
    if (checkForClosed())
        return -1;
    return m_file->size();
}

void BinaryFile::resize(qint64 size)
{
    if (checkForClosed())
        return;
    if (Q_UNLIKELY(!m_file->resize(size))) {
        engine()->throwError(Tr::tr("Could not resize '%1': %2")
                             .arg(m_file->fileName(), m_file->errorString()));
    }
}

qint64 BinaryFile::pos() const
{
    if (checkForClosed())
        return -1;
    return m_file->pos();
}

void BinaryFile::seek(qint64 pos)
{
    if (checkForClosed())
        return;
    if (Q_UNLIKELY(!m_file->seek(pos))) {
        engine()->throwError(Tr::tr("Could not seek '%1': %2")
                             .arg(m_file->fileName(), m_file->errorString()));
    }
}

QVariantList BinaryFile::read(qint64 size)
{
    if (checkForClosed())
        return {};
    const QByteArray bytes = m_file->read(size);
    if (Q_UNLIKELY(bytes.size() == 0 && m_file->error() != QFile::NoError)) {
        engine()->throwError(Tr::tr("Could not read from '%1': %2")
                             .arg(m_file->fileName(), m_file->errorString()));
    }

    QVariantList data;
    std::for_each(bytes.constBegin(), bytes.constEnd(), [&data](const char &c) {
        data.append(c); });
    return data;
}

void BinaryFile::write(const QVariantList &data)
{
    if (checkForClosed())
        return;

    QByteArray bytes;
    std::for_each(data.constBegin(), data.constEnd(), [&bytes](const QVariant &v) {
        bytes.append(char(v.toUInt() & 0xFF)); });

    const qint64 size = m_file->write(bytes);
    if (Q_UNLIKELY(size == -1)) {
        engine()->throwError(Tr::tr("Could not write to '%1': %2")
                             .arg(m_file->fileName(), m_file->errorString()));
    }
}

bool BinaryFile::checkForClosed() const
{
    if (m_file)
        return false;
    engine()->throwError(Tr::tr("Access to BinaryFile object that was already closed."));
    return true;
}

void BinaryFile::releaseResources()
{
    if (m_file)
        close();
}

QJSValue createBinaryFileExtension(QJSEngine *engine)
{
    QJSValue mo = engine->newQMetaObject(&BinaryFile::staticMetaObject);
    QJSValue factory = engine->evaluate(
                QStringLiteral("(function(m, e){ return function(filepath, mode){ "
                               "return new m(e, filepath, mode); } })"));
    QJSValue wrapper = factory.call(QJSValueList{mo, engine->toScriptValue(engine)});
    wrapper.setProperty(QStringLiteral("ReadOnly"), BinaryFile::ReadOnly);
    wrapper.setProperty(QStringLiteral("WriteOnly"), BinaryFile::WriteOnly);
    wrapper.setProperty(QStringLiteral("ReadWrite"), BinaryFile::ReadWrite);
    return wrapper;
}

QBS_REGISTER_JS_EXTENSION("BinaryFile", createBinaryFileExtension)

}
}


#include "binaryfile.moc"
