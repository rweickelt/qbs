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

#include <api/languageinfo.h>
#include <jsextensions/jsextensions.h>
#include <logging/ilogsink.h>
#include <logging/translator.h>
#include <tools/architectures.h>
#include <tools/hostosinfo.h>
#include <tools/shellutils.h>
#include <tools/stringconstants.h>
#include <tools/toolchains.h>
#include <tools/version.h>

#if defined(Q_OS_MACOS)
#include <tools/applecodesignutils.h>
#endif

#ifdef __APPLE__
#include <ar.h>
#include <mach/machine.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#ifndef FAT_MAGIC_64
#define FAT_MAGIC_64 0xcafebabf
#define FAT_CIGAM_64 0xbfbafeca
struct fat_arch_64 {
    cpu_type_t cputype;
    cpu_subtype_t cpusubtype;
    uint64_t offset;
    uint64_t size;
    uint32_t align;
    uint32_t reserved;
};
#endif
#endif


#ifdef Q_OS_WIN
#include <tools/clangclinfo.h>
#include <tools/msvcinfo.h>
#include <tools/vsenvironmentdetector.h>
#endif

#include <QtCore/qcryptographichash.h>
#include <QtCore/qdir.h>
#include <QtCore/qendian.h>
#include <QtCore/qfile.h>
#include <QtCore/qlibrary.h>

namespace qbs {
namespace Internal {

class DummyLogSink : public ILogSink {
    void doPrintMessage(LoggerLevel, const QString &, const QString &) override { }
};

// copied from src/corelib/tools/qtools_p.h
Q_DECL_CONSTEXPR inline char toHexUpper(uint value) Q_DECL_NOTHROW
{
    return "0123456789ABCDEF"[value & 0xF];
}

Q_DECL_CONSTEXPR inline int fromHex(uint c) Q_DECL_NOTHROW
{
    return ((c >= '0') && (c <= '9')) ? int(c - '0') :
           ((c >= 'A') && (c <= 'F')) ? int(c - 'A' + 10) :
           ((c >= 'a') && (c <= 'f')) ? int(c - 'a' + 10) :
           /* otherwise */              -1;
}

// copied from src/corelib/io/qdebug.cpp
static inline bool isPrintable(uchar c)
{ return c >= ' ' && c < 0x7f; }

// modified
template <typename Char>
static inline QString escapedString(const Char *begin, int length, bool isUnicode = true)
{
    QChar quote(QLatin1Char('"'));
    QString out = quote;

    bool lastWasHexEscape = false;
    const Char *end = begin + length;
    for (const Char *p = begin; p != end; ++p) {
        // check if we need to insert "" to break an hex escape sequence
        if (Q_UNLIKELY(lastWasHexEscape)) {
            if (fromHex(*p) != -1) {
                // yes, insert it
                out += QLatin1Char('"');
                out += QLatin1Char('"');
            }
            lastWasHexEscape = false;
        }

        if (sizeof(Char) == sizeof(QChar)) {
            // Surrogate characters are category Cs (Other_Surrogate), so isPrintable = false for them
            int runLength = 0;
            while (p + runLength != end &&
                   QChar::isPrint(p[runLength]) && p[runLength] != '\\' && p[runLength] != '"')
                ++runLength;
            if (runLength) {
                out += QString(reinterpret_cast<const QChar *>(p), runLength);
                p += runLength - 1;
                continue;
            }
        } else if (isPrintable(*p) && *p != '\\' && *p != '"') {
            QChar c = QLatin1Char(*p);
            out += c;
            continue;
        }

        // print as an escape sequence (maybe, see below for surrogate pairs)
        int buflen = 2;
        ushort buf[sizeof "\\U12345678" - 1];
        buf[0] = '\\';

        switch (*p) {
        case '"':
        case '\\':
            buf[1] = *p;
            break;
        case '\b':
            buf[1] = 'b';
            break;
        case '\f':
            buf[1] = 'f';
            break;
        case '\n':
            buf[1] = 'n';
            break;
        case '\r':
            buf[1] = 'r';
            break;
        case '\t':
            buf[1] = 't';
            break;
        default:
            if (!isUnicode) {
                // print as hex escape
                buf[1] = 'x';
                buf[2] = toHexUpper(uchar(*p) >> 4);
                buf[3] = toHexUpper(uchar(*p));
                buflen = 4;
                lastWasHexEscape = true;
                break;
            }
            if (QChar::isHighSurrogate(*p)) {
                if ((p + 1) != end && QChar::isLowSurrogate(p[1])) {
                    // properly-paired surrogates
                    uint ucs4 = QChar::surrogateToUcs4(*p, p[1]);
                    if (QChar::isPrint(ucs4)) {
                        buf[0] = *p;
                        buf[1] = p[1];
                        buflen = 2;
                    } else {
                        buf[1] = 'U';
                        buf[2] = '0'; // toHexUpper(ucs4 >> 32);
                        buf[3] = '0'; // toHexUpper(ucs4 >> 28);
                        buf[4] = toHexUpper(ucs4 >> 20);
                        buf[5] = toHexUpper(ucs4 >> 16);
                        buf[6] = toHexUpper(ucs4 >> 12);
                        buf[7] = toHexUpper(ucs4 >> 8);
                        buf[8] = toHexUpper(ucs4 >> 4);
                        buf[9] = toHexUpper(ucs4);
                        buflen = 10;
                    }
                    ++p;
                    break;
                }
                // improperly-paired surrogates, fall through
            }
            buf[1] = 'u';
            buf[2] = toHexUpper(ushort(*p) >> 12);
            buf[3] = toHexUpper(ushort(*p) >> 8);
            buf[4] = toHexUpper(*p >> 4);
            buf[5] = toHexUpper(*p);
            buflen = 6;
        }
        out += QString(reinterpret_cast<QChar *>(buf), buflen);
    }

    out += quote;
    return out;
}

#ifdef __APPLE__
template <typename T = uint32_t> T readInt(QIODevice *ioDevice, bool *ok,
                                           bool swap, bool peek = false) {
    const auto bytes = peek
            ? ioDevice->peek(sizeof(T))
            : ioDevice->read(sizeof(T));
    if (bytes.size() != sizeof(T)) {
        if (ok)
            *ok = false;
        return T();
    }
    if (ok)
        *ok = true;
    T n = *reinterpret_cast<const T *>(bytes.constData());
    return swap ? qbswap(n) : n;
}

static QString archName(cpu_type_t cputype, cpu_subtype_t cpusubtype)
{
    switch (cputype) {
    case CPU_TYPE_X86:
        switch (cpusubtype) {
        case CPU_SUBTYPE_X86_ALL:
            return QStringLiteral("i386");
        default:
            return {};
        }
    case CPU_TYPE_X86_64:
        switch (cpusubtype) {
        case CPU_SUBTYPE_X86_64_ALL:
            return QStringLiteral("x86_64");
        case CPU_SUBTYPE_X86_64_H:
            return QStringLiteral("x86_64h");
        default:
            return {};
        }
    case CPU_TYPE_ARM:
        switch (cpusubtype) {
        case CPU_SUBTYPE_ARM_V7:
            return QStringLiteral("armv7a");
        case CPU_SUBTYPE_ARM_V7S:
            return QStringLiteral("armv7s");
        case CPU_SUBTYPE_ARM_V7K:
            return QStringLiteral("armv7k");
        default:
            return {};
        }
    case CPU_TYPE_ARM64:
        switch (cpusubtype) {
        case CPU_SUBTYPE_ARM64_ALL:
            return QStringLiteral("arm64");
        default:
            return {};
        }
    default:
        return {};
    }
}

static QStringList detectMachOArchs(QIODevice *device)
{
    bool ok;
    bool foundMachO = false;
    qint64 pos = device->pos();

    char ar_header[SARMAG];
    if (device->read(ar_header, SARMAG) == SARMAG) {
        if (strncmp(ar_header, ARMAG, SARMAG) == 0) {
            while (!device->atEnd()) {
                static_assert(sizeof(ar_hdr) == 60, "sizeof(ar_hdr) != 60");
                ar_hdr header;
                if (device->read(reinterpret_cast<char *>(&header),
                                 sizeof(ar_hdr)) != sizeof(ar_hdr))
                    return {};

                // If the file name is stored in the "extended format" manner,
                // the real filename is prepended to the data section, so skip that many bytes
                int filenameLength = 0;
                if (strncmp(header.ar_name, AR_EFMT1, sizeof(AR_EFMT1) - 1) == 0) {
                    char arName[sizeof(header.ar_name)] = { 0 };
                    memcpy(arName, header.ar_name + sizeof(AR_EFMT1) - 1,
                           sizeof(header.ar_name) - (sizeof(AR_EFMT1) - 1) - 1);
                    filenameLength = strtoul(arName, nullptr, 10);
                    if (device->read(filenameLength).size() != filenameLength)
                        return {};
                }

                switch (readInt(device, nullptr, false, true)) {
                case MH_CIGAM:
                case MH_CIGAM_64:
                case MH_MAGIC:
                case MH_MAGIC_64:
                    foundMachO = true;
                    break;
                default: {
                    // Skip the data and go to the next archive member...
                    char szBuf[sizeof(header.ar_size) + 1] = { 0 };
                    memcpy(szBuf, header.ar_size, sizeof(header.ar_size));
                    int sz = static_cast<int>(strtoul(szBuf, nullptr, 10));
                    if (sz % 2 != 0)
                        ++sz;
                    sz -= filenameLength;
                    const auto data = device->read(sz);
                    if (data.size() != sz)
                        return {};
                }
                }

                if (foundMachO)
                    break;
            }
        }
    }

    // Wasn't an archive file, so try a fat file
    if (!foundMachO && !device->seek(pos))
        return {};

    pos = device->pos();

    fat_header fatheader;
    fatheader.magic = readInt(device, nullptr, false);
    if (fatheader.magic == FAT_MAGIC || fatheader.magic == FAT_CIGAM ||
        fatheader.magic == FAT_MAGIC_64 || fatheader.magic == FAT_CIGAM_64) {
        const bool swap = fatheader.magic == FAT_CIGAM || fatheader.magic == FAT_CIGAM_64;
        const bool is64bit = fatheader.magic == FAT_MAGIC_64 || fatheader.magic == FAT_CIGAM_64;
        fatheader.nfat_arch = readInt(device, &ok, swap);
        if (!ok)
            return {};

        QStringList archs;

        for (uint32_t n = 0; n < fatheader.nfat_arch; ++n) {
            fat_arch_64 fatarch;
            static_assert(sizeof(fat_arch_64) == 32, "sizeof(fat_arch_64) != 32");
            static_assert(sizeof(fat_arch) == 20, "sizeof(fat_arch) != 20");
            const qint64 expectedBytes = is64bit ? sizeof(fat_arch_64) : sizeof(fat_arch);
            if (device->read(reinterpret_cast<char *>(&fatarch), expectedBytes) != expectedBytes)
                return {};

            if (swap) {
                fatarch.cputype = qbswap(fatarch.cputype);
                fatarch.cpusubtype = qbswap(fatarch.cpusubtype);
            }

            const QString name = archName(fatarch.cputype, fatarch.cpusubtype);
            if (name.isEmpty()) {
                qWarning("Unknown cputype %d and cpusubtype %d",
                         fatarch.cputype, fatarch.cpusubtype);
                return {};
            }
            archs.push_back(name);
        }

        std::sort(archs.begin(), archs.end());
        return archs;
    }

    // Wasn't a fat file, so we just read a thin Mach-O from the original offset
    if (!device->seek(pos))
        return {};

    bool swap = false;
    mach_header header;
    header.magic = readInt(device, nullptr, swap);
    switch (header.magic) {
    case MH_CIGAM:
    case MH_CIGAM_64:
        swap = true;
        break;
    case MH_MAGIC:
    case MH_MAGIC_64:
        break;
    default:
        return {};
    }

    header.cputype = static_cast<cpu_type_t>(readInt(device, &ok, swap));
    if (!ok)
        return {};

    header.cpusubtype = static_cast<cpu_subtype_t>(readInt(device, &ok, swap));
    if (!ok)
        return {};

    const QString name = archName(header.cputype, header.cpusubtype);
    if (name.isEmpty()) {
        qWarning("Unknown cputype %d and cpusubtype %d",
                 header.cputype, header.cpusubtype);
        return {};
    }
    return {name};
}
#endif

} // namespace Internal
} // namespace qbs

#include "jsextensions.h"
#include <language/scriptengine.h>
#include <QtCore/qobject.h>
#include <QtQml/qjsvalue.h>

namespace qbs {
namespace Internal {

namespace {
    QJSValue toJSList(QJSEngine *engine, const QStringList &strings) {
        QJSValue result = engine->newArray(strings.length());
        for (int i = 0; i < strings.length(); i++)
            result.setProperty(i, QJSValue(strings[i]));
        return result;
    }
}

class UtilitiesExtension : public JsExtension
{
    Q_OBJECT
public:
    Q_INVOKABLE QJSValue canonicalArchitecture(const QString &architecture);
    Q_INVOKABLE QJSValue canonicalPlatform(const QJSValue &);
    Q_INVOKABLE QJSValue canonicalTargetArchitecture(const QString &architecture,
                                                     const QString &endianness = QString(),
                                                     const QString &vendor = QString(),
                                                     const QString &system = QString(),
                                                     const QString &abi = QString());
    Q_INVOKABLE QJSValue canonicalToolchain(const QJSValue &values);
    Q_INVOKABLE QJSValue cStringQuote(const QString &value);
    Q_INVOKABLE QJSValue getHash(const QString &value);
    Q_INVOKABLE QJSValue getNativeSetting(const QString &name, QString key = QString(),
                                          const QVariant &defaultValue = QVariant());
    Q_INVOKABLE QString kernelVersion();
    Q_INVOKABLE QStringList nativeSettingGroups(const QString &name);
    Q_INVOKABLE QJSValue rfc1034Identifier(const QString &identifier);

    Q_INVOKABLE QByteArray smimeMessageContent(const QString &filePath);
    Q_INVOKABLE QVariantMap certificateInfo(const QByteArray &data);
    Q_INVOKABLE QVariantMap signingIdentities();
    Q_INVOKABLE QVariantMap msvcCompilerInfo(const QString &compilerFilePath,
                                 const QString &compilerLanguage);
    Q_INVOKABLE QVariantMap clangClCompilerInfo(const QString &compilerFilePath,
                                    QString arch,
                                    const QString &vcvarsallPath,
                                    const QString &compilerLanguage);
    Q_INVOKABLE QVariantList installedMSVCs(const QString &arch);
    Q_INVOKABLE QVariantList installedClangCls(const QString &path);

    Q_INVOKABLE QJSValue versionCompare(const QJSValue &a, const QJSValue &b);

    Q_INVOKABLE QString qmlTypeInfo();
    Q_INVOKABLE QStringList builtinExtensionNames() const;
    Q_INVOKABLE bool isSharedLibrary(const QString &value) const;

    Q_INVOKABLE QStringList getArchitecturesFromBinary(const QString &filePath) const;

    Q_INVOKABLE QString shellQuote(const QString &program, const QStringList &args);
};

QJSValue UtilitiesExtension::canonicalPlatform(const QJSValue &value)
{
    if (value.isUndefined() || value.isNull())
        return qjsEngine(this)->newArray();

    if (value.isString()) {
        auto ids = HostOsInfo::canonicalOSIdentifiers(value.toString().toStdString());
        QJSValue list = qjsEngine(this)->newArray(ids.size());
        for (size_t i = 0; i < ids.size(); i++)
            list.setProperty(i, QJSValue(QString::fromStdString(ids[i])));
        return list;
    }

    qjsEngine(this)->throwError(QJSValue::SyntaxError,
        QStringLiteral("canonicalPlatform expects one argument of type string"));

    return qjsEngine(this)->newArray();
}

QJSValue UtilitiesExtension::canonicalTargetArchitecture(const QString &architecture,
                                                           const QString &endianness,
                                                           const QString &vendor,
                                                           const QString &system,
                                                           const QString &abi)
{
    if (architecture.isNull())
        return QJSValue();

    return qbs::canonicalTargetArchitecture(architecture, endianness, vendor, system, abi);
}

QJSValue UtilitiesExtension::canonicalArchitecture(const QString &architecture)
{
    if (architecture.isNull())
        return QJSValue();

    return qbs::canonicalArchitecture(architecture);
}

// TODO: Candidate for variadic argument length
QJSValue UtilitiesExtension::canonicalToolchain(const QJSValue& values)
{
    if (values.isUndefined())
        return qjsEngine(this)->newArray();

    return toJSList(qjsEngine(this), qbs::canonicalToolchain(values.toVariant().toStringList()));
}

QJSValue UtilitiesExtension::cStringQuote(const QString &value)
{
    return escapedString(reinterpret_cast<const ushort *>(value.constData()), value.size());
}

QJSValue UtilitiesExtension::getHash(const QString &value)
{
    if (value.isNull())
        return QJSValue();

    const QByteArray input = value.toLatin1();
    const QByteArray hash
            = QCryptographicHash::hash(input, QCryptographicHash::Sha1).toHex().left(16);
    return qjsEngine(this)->toScriptValue(QString::fromLatin1(hash));
}

QJSValue UtilitiesExtension::getNativeSetting(const QString &filepath, QString key,
                                                const QVariant &defaultValue)
{
    if (key.isEmpty()) {
        // We'll let empty string represent the default registry value on Windows.
        if (HostOsInfo::isWindowsHost())
            key = StringConstants::dot();
        else
            return defaultValue.toString();
    }

    QSettings settings(filepath, QSettings::NativeFormat);
    QVariant value = settings.value(key, defaultValue);
    return value.isNull() ? QJSValue() : engine()->toScriptValue(value);
}

QString UtilitiesExtension::kernelVersion()
{
    return QSysInfo::kernelVersion();
}

QStringList UtilitiesExtension::nativeSettingGroups(const QString &name)
{
    QSettings settings(name, QSettings::NativeFormat);
    return settings.childGroups();
}

QJSValue UtilitiesExtension::rfc1034Identifier(const QString &identifier)
{
    return QJSValue(HostOsInfo::rfc1034Identifier(identifier));
}

/**
 * Reads the contents of the S/MIME message located at \p filePath.
 * An equivalent command line would be:
 * \code security cms -D -i <infile> -o <outfile> \endcode
 * or:
 * \code openssl smime -verify -noverify -inform DER -in <infile> -out <outfile> \endcode
 *
 * \note A provisioning profile is an S/MIME message whose contents are an XML property list,
 * so this method can be used to read such files.
 */
QByteArray UtilitiesExtension::smimeMessageContent(const QString &filePath)
{
#if !defined(Q_OS_MACOS)
    Q_UNUSED(filePath);
    engine()->throwError(
                QStringLiteral("smimeMessageContent is not available on this platform"));
    return {};
#else
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QByteArray content = qbs::Internal::smimeMessageContent(file.readAll());
    if (content.isEmpty())
        return {};
    return content;
#endif
}

QVariantMap UtilitiesExtension::certificateInfo(const QByteArray &data)
{
#if !defined(Q_OS_MACOS)
    Q_UNUSED(data);
    engine()->throwError(
                QStringLiteral("certificateInfo is not available on this platform"));
    return {};
#else
    return qbs::Internal::certificateInfo(data);
#endif
}

// Rough command line equivalent: security find-identity -p codesigning -v
QVariantMap UtilitiesExtension::signingIdentities()
{
#if !defined(Q_OS_MACOS)
    engine()->throwError(QStringLiteral("signingIdentities is not available on this platform"));
    return {};
#else
    return qbs::Internal::identitiesProperties();
#endif
}

#ifdef Q_OS_WIN
static std::pair<QVariantMap /*result*/, QString /*error*/> msvcCompilerInfoHelper(
        const QString &compilerFilePath,
        MSVC::CompilerLanguage language,
        const QString &vcvarsallPath,
        const QString &arch)
{
    MSVC msvc(compilerFilePath, arch);
    VsEnvironmentDetector envdetector(vcvarsallPath);
    if (!envdetector.start(&msvc))
        return { {}, QStringLiteral("Detecting the MSVC build environment failed: ")
                    + envdetector.errorString() };

    try {
        QVariantMap envMap;
        for (const QString &key : msvc.environment.keys())
            envMap.insert(key, msvc.environment.value(key));

        return {
            QVariantMap {
                {QStringLiteral("buildEnvironment"), envMap},
                {QStringLiteral("macros"), msvc.compilerDefines(compilerFilePath, language)},
            },
            {}
        };
    } catch (const qbs::ErrorInfo &info) {
        return { {}, info.toString() };
    }
}
#endif


QVariantMap UtilitiesExtension::msvcCompilerInfo(const QString &compilerFilePath,
                                                   const QString &compilerLanguage)
{
#ifndef Q_OS_WIN
    Q_UNUSED(compilerFilePath);
    Q_UNUSED((compilerLanguage));
    engine()->throwError(QStringLiteral("msvcCompilerInfo is not available on this platform"));
    return {};
#else
    MSVC::CompilerLanguage language;
    if (compilerLanguage == QStringLiteral("c"))
        language = MSVC::CLanguage;
    else if (compilerLanguage == StringConstants::cppLang())
        language = MSVC::CPlusPlusLanguage;
    else {
        engine()->throwError(QJSValue::TypeError,
            QStringLiteral("msvcCompilerInfo expects \"c\" or \"cpp\" as its second argument"));
        return {};
    }
    const auto result = msvcCompilerInfoHelper(
            compilerFilePath, language, {}, MSVC::architectureFromClPath(compilerFilePath));
    if (result.first.isEmpty()) {
        engine()->throwError(result.second);
        return {};
    }
    return result.first;
#endif
}

QVariantMap UtilitiesExtension::clangClCompilerInfo(const QString &compilerFilePath,
                                                      QString arch,
                                                      const QString &vcvarsallPath,
                                                      const QString &compilerLanguage)
{
#ifndef Q_OS_WIN
    Q_UNUSED(compilerFilePath);
    Q_UNUSED(arch);
    Q_UNUSED(vcvarsallPath);
    Q_UNUSED(compilerLanguage);
    engine()->throwError(QStringLiteral("clangClCompilerInfo is not available on this platform"));
    return {};
#else
    // architecture cannot be empty as vcvarsall.bat requires at least 1 arg, so fallback
    // to host architecture if none is present
    if (arch.isEmpty())
        arch = QString::fromStdString(HostOsInfo::hostOSArchitecture());

    MSVC::CompilerLanguage language;
    if (compilerLanguage == QStringLiteral("c"))
        language = MSVC::CLanguage;
    else if (compilerLanguage == StringConstants::cppLang())
        language = MSVC::CPlusPlusLanguage;
    else {
        engine()->throwError(QJSValue::TypeError,
            QStringLiteral("clangClCompilerInfo expects \"c\" or \"cpp\" as its fourth argument"));
        return {};
    }
    const auto result = msvcCompilerInfoHelper(
            compilerFilePath, language, vcvarsallPath, arch);
    if (result.first.isEmpty()) {
        engine()->throwError(result.second);
        return {};
    }
    return result.first;
#endif
}

QVariantList UtilitiesExtension::installedMSVCs(const QString &arch)
{
#ifndef Q_OS_WIN
    Q_UNUSED(arch);
    engine()->throwError(QStringLiteral("installedMSVCs is not available on this platform"));
    return {};
#else
    const auto hostArch = QString::fromStdString(HostOsInfo::hostOSArchitecture());
    const auto preferredArch = !arch.isEmpty()
            ? arch
            : hostArch;

    DummyLogSink dummySink;
    Logger dummyLogger(&dummySink);
    auto msvcs = MSVC::installedCompilers(dummyLogger);

    const auto predicate = [&preferredArch, &hostArch](const MSVC &msvc)
    {
        auto archPair = MSVC::getHostTargetArchPair(msvc.architecture);
        return archPair.first != hostArch || preferredArch != archPair.second;
    };
    msvcs.erase(std::remove_if(msvcs.begin(), msvcs.end(), predicate), msvcs.end());
    QVariantList result;
    for (const auto &msvc: msvcs)
        result.append(msvc.toVariantMap());
    return result;
#endif
}

QVariantList UtilitiesExtension::installedClangCls(const QString &path)
{
#ifndef Q_OS_WIN
    Q_UNUSED(path);
    engine()->throwError(QStringLiteral("installedClangCls is not available on this platform"));
    return {};
#else
    DummyLogSink dummySink;
    Logger dummyLogger(&dummySink);
    auto compilers = ClangClInfo::installedCompilers({path}, dummyLogger);
    QVariantList result;
    for (const auto &compiler: compilers)
        result.append(compiler.toVariantMap());
    return result;
#endif
}

QJSValue UtilitiesExtension::versionCompare(const QJSValue &a, const QJSValue &b)
{
    if (a.isString() && b.isString()) {
        const QString value1 = a.toString();
        const QString value2 = b.toString();
        const auto a = Version::fromString(value1);
        const auto b = Version::fromString(value2);
        return QJSValue(compare(a, b));
    }

    qjsEngine(this)->throwError(QJSValue::SyntaxError,
        QStringLiteral("versionCompare expects two arguments of type string"));
    return QJSValue();
}

QString UtilitiesExtension::qmlTypeInfo()
{
    return QString::fromStdString(qbs::LanguageInfo::qmlTypeInfo());
}

QStringList UtilitiesExtension::builtinExtensionNames() const
{
    return JsExtensions::extensionNames();
}

bool UtilitiesExtension::isSharedLibrary(const QString &value) const
{
    return QLibrary::isLibrary(value);
}

QStringList UtilitiesExtension::getArchitecturesFromBinary(const QString &filePath) const
{
    QStringList archs;
#ifndef __APPLE__
    Q_UNUSED(filePath);
#else
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        engine()->throwError(QStringLiteral("Failed to open file '%1': %2")
                             .arg(file.fileName(), file.errorString()));
        return {};
    }
    archs = detectMachOArchs(&file);
#endif // __APPLE__
    return archs;
}

QString UtilitiesExtension::shellQuote(const QString &program, const QStringList &args)
{
    HostOsInfo::HostOs hostOs = HostOsInfo::hostOs();
    return qbs::Internal::shellQuote(program, args, hostOs);
}

QJSValue createUtilitiesExtension(QJSEngine *engine)
{
    return engine->newQObject(new UtilitiesExtension());
}

QBS_REGISTER_JS_EXTENSION("Utilities", createUtilitiesExtension)

}
}


#include "utilitiesextension.moc"
