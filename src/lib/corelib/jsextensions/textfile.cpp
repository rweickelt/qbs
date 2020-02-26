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
#include <tools/hostosinfo.h>

#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qobject.h>
#include <QtCore/qtextstream.h>
#include <QtCore/qvariant.h>

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QtCore5Compat/qtextcodec.h>
#else
#include <QtCore/qtextcodec.h>
#endif

#include <QtQml/qjsvalue.h>

namespace qbs {
namespace Internal {

class TextFile : public JsExtension, public ResourceAcquiringScriptObject
{
    Q_OBJECT
    Q_ENUMS(OpenMode)
public:
    enum OpenMode
    {
        ReadOnly = 1,
        WriteOnly = 2,
        ReadWrite = ReadOnly | WriteOnly,
        Append = 4
    };

    Q_INVOKABLE TextFile(QObject *engine, const QJSValue &filePath, OpenMode mode = ReadOnly,
                         const QString &codec = QLatin1String("UTF-8"));
    ~TextFile() override;

    Q_INVOKABLE void close();
    Q_INVOKABLE QString filePath();
    Q_INVOKABLE void setCodec(const QString &codec);
    Q_INVOKABLE QString readLine();
    Q_INVOKABLE QString readAll();
    Q_INVOKABLE bool atEof() const;
    Q_INVOKABLE void truncate();
    Q_INVOKABLE void write(const QString &str);
    Q_INVOKABLE void writeLine(const QString &str);

private:
    bool checkForClosed() const;

    // ResourceAcquiringScriptObject implementation
    void releaseResources() override;

    QFile *m_file = nullptr;
    QTextCodec *m_codec = nullptr;
};

TextFile::TextFile(QObject *engine, const QJSValue &filePath, OpenMode mode, const QString &codec)
    : ResourceAcquiringScriptObject(qobject_cast<ScriptEngine *>(engine))
{
    Q_UNUSED(codec)
    auto *se = qobject_cast<ScriptEngine *>(engine);

    m_file = new QFile(filePath.toString());
    const auto newCodec = QTextCodec::codecForName(qPrintable(codec));
    m_codec = newCodec ? newCodec : QTextCodec::codecForName("UTF-8");
    QIODevice::OpenMode m = QIODevice::NotOpen;
    if (mode & ReadOnly)
        m |= QIODevice::ReadOnly;
    if (mode & WriteOnly)
        m |= QIODevice::WriteOnly;
    if (mode & Append)
        m |= QIODevice::Append;
    m |= QIODevice::Text;
    if (Q_UNLIKELY(!m_file->open(m))) {
        se->throwError(Tr::tr("Unable to open file '%1': %2")
                       .arg(filePath.toString())
                       .arg(m_file->errorString()));
        delete m_file;
        m_file = nullptr;
        return;
    }

    se->addResourceAcquiringScriptObject(this);
    const DubiousContextList dubiousContexts({
            DubiousContext(EvalContext::PropertyEvaluation, DubiousContext::SuggestMoving)
    });
    se->checkContext(QStringLiteral("qbs.TextFile"), dubiousContexts);
    se->setUsesIo();
}

TextFile::~TextFile()
{
    delete m_file;
}

void TextFile::close()
{
    if (checkForClosed())
        return;
    m_file->close();
    delete m_file;
    m_file = nullptr;
}

QString TextFile::filePath()
{
    if (checkForClosed())
        return {};
    return QFileInfo(*m_file).absoluteFilePath();
}

void TextFile::setCodec(const QString &codec)
{
    if (checkForClosed())
        return;
    const auto newCodec = QTextCodec::codecForName(qPrintable(codec));
    if (newCodec)
        m_codec = newCodec;
}

QString TextFile::readLine()
{
    if (checkForClosed())
        return {};
    auto result = m_codec->toUnicode(m_file->readLine());
    if (!result.isEmpty() && result.back() == QLatin1Char('\n'))
        result.chop(1);
    return result;
}

QString TextFile::readAll()
{
    if (checkForClosed())
        return {};
    return m_codec->toUnicode(m_file->readAll());
}

bool TextFile::atEof() const
{
    if (checkForClosed())
        return true;
    return m_file->atEnd();
}

void TextFile::truncate()
{
    if (checkForClosed())
        return;
    m_file->resize(0);
}

void TextFile::write(const QString &str)
{
    if (checkForClosed())
        return;
    m_file->write(m_codec->fromUnicode(str));
}

void TextFile::writeLine(const QString &str)
{
    if (checkForClosed())
        return;
    m_file->write(m_codec->fromUnicode(str));
    m_file->putChar('\n');
}

bool TextFile::checkForClosed() const
{
    if (m_file)
        return false;
    engine()->throwError(Tr::tr("Access to TextFile object that was already closed."));
    return true;
}

void TextFile::releaseResources()
{
    if (m_file)
        close();
}

static const QString textFileConstructor = QStringLiteral(R"QBS(
(function(m, e){
    return function(filepath, mode = m.ReadOnly, codec = "UTF-8") {
        return new m(e, filepath, mode, codec);
    }
})
)QBS");

QJSValue createTextFileExtension(QJSEngine *engine)
{
    QJSValue mo = engine->newQMetaObject(&TextFile::staticMetaObject);
    QJSValue factory = engine->evaluate(textFileConstructor);
    QJSValue wrapper = factory.call(QJSValueList{mo, engine->toScriptValue(engine)});
    wrapper.setProperty(QStringLiteral("ReadOnly"), TextFile::ReadOnly);
    wrapper.setProperty(QStringLiteral("WriteOnly"), TextFile::WriteOnly);
    wrapper.setProperty(QStringLiteral("ReadWrite"), TextFile::ReadWrite);
    wrapper.setProperty(QStringLiteral("Append"), TextFile::Append);
    return wrapper;
}

QBS_REGISTER_JS_EXTENSION("TextFile", createTextFileExtension)

} // namespace Internal
} // namespace qbs

#include "textfile.moc"
