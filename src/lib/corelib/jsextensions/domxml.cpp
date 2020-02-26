/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Copyright (C) 2016 BogDan Vatra <bogdan@kde.org>
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

#include <QtCore/qfile.h>
#include <QtCore/qobject.h>
#include <QtCore/qvariant.h>

#include <QtQml/qjsvalue.h>

#include <QtXml/qdom.h>


namespace qbs {
namespace Internal {

class XmlDomDocument;

class XmlDomNode: public JsExtension
{
    Q_OBJECT
public:
    Q_INVOKABLE XmlDomNode(const QDomNode &other = QDomNode());

    Q_INVOKABLE bool isElement() const;
    Q_INVOKABLE bool isCDATASection() const;
    Q_INVOKABLE bool isText() const;

    Q_INVOKABLE QString attribute(const QString & name, const QString & defValue = QString());
    Q_INVOKABLE void setAttribute(const QString & name, const QString & value);
    Q_INVOKABLE bool hasAttribute(const QString & name) const;
    Q_INVOKABLE QString tagName() const;
    Q_INVOKABLE void setTagName(const QString & name);

    Q_INVOKABLE QString text() const;

    Q_INVOKABLE QString data() const;
    Q_INVOKABLE void setData(const QString &v) const;

    Q_INVOKABLE void clear();
    Q_INVOKABLE bool hasAttributes() const;
    Q_INVOKABLE bool hasChildNodes() const;
    Q_INVOKABLE QJSValue parentNode() const;
    Q_INVOKABLE QJSValue firstChild(const QString & tagName = QString());
    Q_INVOKABLE QJSValue lastChild(const QString & tagName = QString()) const;
    Q_INVOKABLE QJSValue previousSibling(const QString & tagName = QString()) const;
    Q_INVOKABLE QJSValue nextSibling(const QString & tagName = QString()) const;

    Q_INVOKABLE QJSValue appendChild(const QJSValue &newChild);
    Q_INVOKABLE QJSValue insertBefore(const QJSValue& newChild, const QJSValue& refChild);
    Q_INVOKABLE QJSValue insertAfter(const QJSValue& newChild, const QJSValue& refChild);
    Q_INVOKABLE QJSValue replaceChild(const QJSValue& newChild, const QJSValue& oldChild);
    Q_INVOKABLE QJSValue removeChild(const QJSValue& oldChild);

protected:
    friend class XmlDomDocument;
    QDomNode m_domNode;
};

class XmlDomDocument: public XmlDomNode
{
    Q_OBJECT
public:
    Q_INVOKABLE XmlDomDocument(QObject *engine, const QString &name);
    Q_INVOKABLE QJSValue documentElement();
    Q_INVOKABLE QJSValue createElement(const QString & tagName);
    Q_INVOKABLE QJSValue createCDATASection(const QString & value);
    Q_INVOKABLE QJSValue createTextNode(const QString & value);

    Q_INVOKABLE bool setContent(const QString & content);
    Q_INVOKABLE QString toString(int indent = 1);

    Q_INVOKABLE void save(const QString & filePath, int indent = 1);
    Q_INVOKABLE void load(const QString & filePath);

private:
    QDomDocument m_domDocument;
};

XmlDomDocument::XmlDomDocument(QObject *engine, const QString &name):m_domDocument(name)
{
    m_domNode = m_domDocument;
    qobject_cast<ScriptEngine *>(engine)->setUsesIo();
}

QJSValue XmlDomDocument::documentElement()
{
    return engine()->newQObject(new XmlDomNode(m_domDocument.documentElement()));
}

QJSValue XmlDomDocument::createElement(const QString &tagName)
{
    return engine()->newQObject(new XmlDomNode(m_domDocument.createElement(tagName)));
}

QJSValue XmlDomDocument::createCDATASection(const QString &value)
{
    return engine()->newQObject(new XmlDomNode(m_domDocument.createCDATASection(value)));
}

QJSValue XmlDomDocument::createTextNode(const QString &value)
{
    return engine()->newQObject(new XmlDomNode(m_domDocument.createTextNode(value)));
}

bool XmlDomDocument::setContent(const QString &content)
{
    return m_domDocument.setContent(content);
}

QString XmlDomDocument::toString(int indent)
{
    return m_domDocument.toString(indent);
}

void XmlDomDocument::save(const QString &filePath, int indent)
{
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly)) {
        engine()->throwError(QStringLiteral("unable to open '%1'")
                              .arg(filePath));
        return;
    }

    QByteArray buff(m_domDocument.toByteArray(indent));
    if (buff.size() != f.write(buff))
    {
        engine()->throwError(f.errorString());
        f.close();
        return;
    }

    f.close();
    if (f.error() != QFile::NoError)
        engine()->throwError(f.errorString());
}

void XmlDomDocument::load(const QString &filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        engine()->throwError(QStringLiteral("unable to open '%1'")
                              .arg(filePath));
        return;
    }

    QString errorMsg;
    if (!m_domDocument.setContent(&f, &errorMsg)) {
        engine()->throwError(errorMsg);
        return;
    }
}

bool XmlDomNode::isElement() const
{
    return m_domNode.isElement();
}

bool XmlDomNode::isCDATASection() const
{
    return m_domNode.isCDATASection();
}

bool XmlDomNode::isText() const
{
    return m_domNode.isText();
}

QString XmlDomNode::attribute(const QString &name, const QString &defValue)
{
    QDomElement el = m_domNode.toElement();
    if (el.isNull()) {
        engine()->throwError(QStringLiteral("Node '%1' is not an element node").arg(m_domNode.nodeName()));
        return defValue;
    }
    return el.attribute(name, defValue);
}

void XmlDomNode::setAttribute(const QString &name, const QString &value)
{
    QDomElement el = m_domNode.toElement();
    if (el.isNull()) {
        engine()->throwError(QStringLiteral("Node '%1' is not an element node").arg(m_domNode.nodeName()));
        return;
    }
    el.setAttribute(name, value);
}

bool XmlDomNode::hasAttribute(const QString &name) const
{
    QDomElement el = m_domNode.toElement();
    if (el.isNull()) {
        engine()->throwError(QStringLiteral("Node '%1' is not an element node").arg(m_domNode.nodeName()));
        return false;
    }
    return el.hasAttribute(name);
}

QString XmlDomNode::tagName() const
{
    QDomElement el = m_domNode.toElement();
    if (el.isNull()) {
        engine()->throwError(QStringLiteral("Node '%1' is not an element node").arg(m_domNode.nodeName()));
        return {};
    }
    return el.tagName();
}

void XmlDomNode::setTagName(const QString &name)
{
    QDomElement el = m_domNode.toElement();
    if (el.isNull()) {
        engine()->throwError(QStringLiteral("Node '%1' is not an element node").arg(m_domNode.nodeName()));
        return;
    }
    el.setTagName(name);
}

QString XmlDomNode::text() const
{
    QDomElement el = m_domNode.toElement();
    if (el.isNull()) {
        engine()->throwError(QStringLiteral("Node '%1' is not an element node").arg(m_domNode.nodeName()));
        return {};
    }
    return el.text();
}

QString XmlDomNode::data() const
{
    if (m_domNode.isText())
        return m_domNode.toText().data();
    if (m_domNode.isCDATASection())
        return m_domNode.toCDATASection().data();
    if (m_domNode.isCharacterData())
        return m_domNode.toCharacterData().data();
    engine()->throwError(QStringLiteral("Node '%1' is not a character data node").arg(m_domNode.nodeName()));
    return {};
}

void XmlDomNode::setData(const QString &v) const
{
    if (m_domNode.isText())
        return m_domNode.toText().setData(v);
    if (m_domNode.isCDATASection())
        return m_domNode.toCDATASection().setData(v);
    if (m_domNode.isCharacterData())
        return m_domNode.toCharacterData().setData(v);
    engine()->throwError(QStringLiteral("Node '%1' is not a character data node").arg(m_domNode.nodeName()));
}

void XmlDomNode::clear()
{
    m_domNode.clear();
}

bool XmlDomNode::hasAttributes() const
{
    return m_domNode.hasAttributes();
}

bool XmlDomNode::hasChildNodes() const
{
    return m_domNode.hasChildNodes();
}

QJSValue XmlDomNode::parentNode() const
{
    return engine()->newQObject(new XmlDomNode(m_domNode.parentNode()));
}

QJSValue XmlDomNode::firstChild(const QString &tagName)
{
    if (tagName.isEmpty())
        return engine()->newQObject(new XmlDomNode(m_domNode.firstChild()));
    return engine()->newQObject(new XmlDomNode(m_domNode.firstChildElement(tagName)));
}

QJSValue XmlDomNode::lastChild(const QString &tagName) const
{
    if (tagName.isEmpty())
        return engine()->newQObject(new XmlDomNode(m_domNode.lastChild()));
    return engine()->newQObject(new XmlDomNode(m_domNode.lastChildElement(tagName)));
}

QJSValue XmlDomNode::previousSibling(const QString &tagName) const
{
    if (tagName.isEmpty())
        return engine()->newQObject(new XmlDomNode(m_domNode.previousSibling()));
    return engine()->newQObject(new XmlDomNode(m_domNode.previousSiblingElement(tagName)));
}

QJSValue XmlDomNode::nextSibling(const QString &tagName) const
{
    if (tagName.isEmpty())
        return engine()->newQObject(new XmlDomNode(m_domNode.nextSibling()));
    return engine()->newQObject(new XmlDomNode(m_domNode.nextSiblingElement(tagName)));
}

QJSValue XmlDomNode::appendChild(const QJSValue &newChild)
{
    auto newNode = qobject_cast<XmlDomNode*>(newChild.toQObject());
    if (!newNode) {
        engine()->throwError(QStringLiteral("First argument is not a XmlDomNode object"));
        return {};
    }
    return engine()->newQObject(new XmlDomNode(m_domNode.appendChild(newNode->m_domNode)));
}

QJSValue XmlDomNode::insertBefore(const QJSValue &newChild, const QJSValue &refChild)
{
    auto newNode = qobject_cast<XmlDomNode*>(newChild.toQObject());
    if (!newNode) {
        engine()->throwError(QStringLiteral("First argument is not a XmlDomNode object"));
        return {};
    }

    auto refNode = qobject_cast<XmlDomNode*>(refChild.toQObject());
    if (!refNode) {
        engine()->throwError(QStringLiteral("Second argument is not a XmlDomNode object"));
        return {};
    }

    return engine()->newQObject(new XmlDomNode(m_domNode.insertBefore(newNode->m_domNode, refNode->m_domNode)));
}

QJSValue XmlDomNode::insertAfter(const QJSValue &newChild, const QJSValue &refChild)
{
    auto newNode = qobject_cast<XmlDomNode*>(newChild.toQObject());
    if (!newNode) {
        engine()->throwError(QStringLiteral("First argument is not a XmlDomNode object"));
        return {};
    }

    auto refNode = qobject_cast<XmlDomNode*>(refChild.toQObject());
    if (!refNode) {
        engine()->throwError(QStringLiteral("Second argument is not a XmlDomNode object"));
        return {};
    }

    return engine()->newQObject(new XmlDomNode(m_domNode.insertAfter(newNode->m_domNode, refNode->m_domNode)));
}

QJSValue XmlDomNode::replaceChild(const QJSValue &newChild, const QJSValue &oldChild)
{
    auto newNode = qobject_cast<XmlDomNode*>(newChild.toQObject());
    if (!newNode) {
        engine()->throwError(QStringLiteral("First argument is not a XmlDomNode object"));
        return {};
    }

    auto oldNode = qobject_cast<XmlDomNode*>(oldChild.toQObject());
    if (!oldNode) {
        engine()->throwError(QStringLiteral("Second argument is not a XmlDomNode object"));
        return {};
    }

    return engine()->newQObject(new XmlDomNode(m_domNode.replaceChild(newNode->m_domNode, oldNode->m_domNode)));
}

QJSValue XmlDomNode::removeChild(const QJSValue &oldChild)
{
    auto oldNode = qobject_cast<XmlDomNode*>(oldChild.toQObject());
    if (!oldNode) {
        engine()->throwError(QStringLiteral("First argument is not a XmlDomNode object"));
        return {};
    }

    return engine()->newQObject(new XmlDomNode(m_domNode.removeChild(oldNode->m_domNode)));
}

XmlDomNode::XmlDomNode(const QDomNode &other): m_domNode(other)
{
}

QJSValue createXmlExtension(QJSEngine *engine)
{
    QJSValue extension = engine->newObject();

    QJSValue docMetaObject = engine->newQMetaObject(&XmlDomDocument::staticMetaObject);
    QJSValue docFactory = engine->evaluate(
                QStringLiteral("(function(m, e){ return function(name){ return new m(e, name); } })"));
    QJSValue docWrapper = docFactory.call(QJSValueList{docMetaObject, engine->toScriptValue(engine)});

    extension.setProperty(QStringLiteral("DomDocument"), docWrapper);
    extension.setProperty(QStringLiteral("DomElement"), engine->newQMetaObject(&XmlDomNode::staticMetaObject));

    return extension;
}

QBS_REGISTER_JS_EXTENSION("Xml", createXmlExtension)

} // namespace Internal
} // namespace qbs


#include "domxml.moc"
