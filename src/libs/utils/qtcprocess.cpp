/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "qtcprocess.h"

#include "stringutils.h"
#include "executeondestruction.h"
#include "hostosinfo.h"
#include "qtcassert.h"
#include "qtcprocess.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStack>
#include <QTextCodec>
#include <QThread>
#include <QTimer>

#ifdef QT_GUI_LIB
// qmlpuppet does not use that.
#include <QApplication>
#include <QMessageBox>
#endif

#include <algorithm>
#include <limits.h>
#include <memory>

#ifdef Q_OS_WIN
#define CALLBACK WINAPI
#include <qt_windows.h>
#else
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#endif


// The main state of the Unix shell parser
enum MxQuoting { MxBasic, MxSingleQuote, MxDoubleQuote, MxParen, MxSubst, MxGroup, MxMath };

struct MxState
{
    MxQuoting current;
    // Bizarrely enough, double quoting has an impact on the behavior of some
    // complex expressions within the quoted string.
    bool dquote;
};
QT_BEGIN_NAMESPACE
Q_DECLARE_TYPEINFO(MxState, Q_PRIMITIVE_TYPE);
QT_END_NAMESPACE

// Pushed state for the case where a $(()) expansion turns out bogus
struct MxSave
{
    QString str;
    int pos, varPos;
};
QT_BEGIN_NAMESPACE
Q_DECLARE_TYPEINFO(MxSave, Q_MOVABLE_TYPE);
QT_END_NAMESPACE

using namespace Utils::Internal;

namespace Utils {

enum { debug = 0 };
enum { syncDebug = 0 };

enum { defaultMaxHangTimerCount = 10 };

static Q_LOGGING_CATEGORY(processLog, "qtc.utils.synchronousprocess", QtWarningMsg);

static std::function<void(QtcProcess &)> s_remoteRunProcessHook;

/*!
    \class Utils::QtcProcess

    \brief The QtcProcess class provides functionality for dealing with
    shell-quoted process arguments.
*/

inline static bool isMetaCharWin(ushort c)
{
    static const uchar iqm[] = {
        0x00, 0x00, 0x00, 0x00, 0x40, 0x03, 0x00, 0x50,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
    }; // &()<>|

    return (c < sizeof(iqm) * 8) && (iqm[c / 8] & (1 << (c & 7)));
}

static void envExpandWin(QString &args, const Environment *env, const QString *pwd)
{
    static const QString cdName = QLatin1String("CD");
    int off = 0;
  next:
    for (int prev = -1, that;
         (that = args.indexOf(QLatin1Char('%'), off)) >= 0;
         prev = that, off = that + 1) {
        if (prev >= 0) {
            const QString var = args.mid(prev + 1, that - prev - 1).toUpper();
            const QString val = (var == cdName && pwd && !pwd->isEmpty())
                                    ? QDir::toNativeSeparators(*pwd) : env->expandedValueForKey(var);
            if (!val.isEmpty()) { // Empty values are impossible, so this is an existence check
                args.replace(prev, that - prev + 1, val);
                off = prev + val.length();
                goto next;
            }
        }
    }
}

static ProcessArgs prepareArgsWin(const QString &_args, ProcessArgs::SplitError *err,
                                  const Environment *env, const QString *pwd)
{
    QString args(_args);

    if (env) {
        envExpandWin(args, env, pwd);
    } else {
        if (args.indexOf(QLatin1Char('%')) >= 0) {
            if (err)
                *err = ProcessArgs::FoundMeta;
            return ProcessArgs::createWindowsArgs(QString());
        }
    }

    if (!args.isEmpty() && args.unicode()[0].unicode() == '@')
        args.remove(0, 1);

    for (int p = 0; p < args.length(); p++) {
        ushort c = args.unicode()[p].unicode();
        if (c == '^') {
            args.remove(p, 1);
        } else if (c == '"') {
            do {
                if (++p == args.length())
                    break; // For cmd, this is no error.
            } while (args.unicode()[p].unicode() != '"');
        } else if (isMetaCharWin(c)) {
            if (err)
                *err = ProcessArgs::FoundMeta;
            return ProcessArgs::createWindowsArgs(QString());
        }
    }

    if (err)
        *err = ProcessArgs::SplitOk;
    return ProcessArgs::createWindowsArgs(args);
}

inline static bool isWhiteSpaceWin(ushort c)
{
    return c == ' ' || c == '\t';
}

static QStringList doSplitArgsWin(const QString &args, ProcessArgs::SplitError *err)
{
    QStringList ret;

    if (err)
        *err = ProcessArgs::SplitOk;

    int p = 0;
    const int length = args.length();
    forever {
        forever {
            if (p == length)
                return ret;
            if (!isWhiteSpaceWin(args.unicode()[p].unicode()))
                break;
            ++p;
        }

        QString arg;
        bool inquote = false;
        forever {
            bool copy = true; // copy this char
            int bslashes = 0; // number of preceding backslashes to insert
            while (p < length && args.unicode()[p] == QLatin1Char('\\')) {
                ++p;
                ++bslashes;
            }
            if (p < length && args.unicode()[p] == QLatin1Char('"')) {
                if (!(bslashes & 1)) {
                    // Even number of backslashes, so the quote is not escaped.
                    if (inquote) {
                        if (p + 1 < length && args.unicode()[p + 1] == QLatin1Char('"')) {
                            // This is not documented on MSDN.
                            // Two consecutive quotes make a literal quote. Brain damage:
                            // this still closes the quoting, so a 3rd quote is required,
                            // which makes the runtime's quoting run out of sync with the
                            // shell's one unless the 2nd quote is escaped.
                            ++p;
                        } else {
                            // Closing quote
                            copy = false;
                        }
                        inquote = false;
                    } else {
                        // Opening quote
                        copy = false;
                        inquote = true;
                    }
                }
                bslashes >>= 1;
            }

            while (--bslashes >= 0)
                arg.append(QLatin1Char('\\'));

            if (p == length || (!inquote && isWhiteSpaceWin(args.unicode()[p].unicode()))) {
                ret.append(arg);
                if (inquote) {
                    if (err)
                        *err = ProcessArgs::BadQuoting;
                    return QStringList();
                }
                break;
            }

            if (copy)
                arg.append(args.unicode()[p]);
            ++p;
        }
    }
    //not reached
}

/*!
    Splits \a _args according to system shell word splitting and quoting rules.

    \section1 Unix

    The behavior is based on the POSIX shell and bash:
    \list
    \li Whitespace splits tokens.
    \li The backslash quotes the following character.
    \li A string enclosed in single quotes is not split. No shell meta
        characters are interpreted.
    \li A string enclosed in double quotes is not split. Within the string,
        the backslash quotes shell meta characters - if it is followed
        by a "meaningless" character, the backslash is output verbatim.
    \endlist
    If \a abortOnMeta is \c false, only the splitting and quoting rules apply,
    while other meta characters (substitutions, redirections, etc.) are ignored.
    If \a abortOnMeta is \c true, encounters of unhandled meta characters are
    treated as errors.

    If \a err is not NULL, stores a status code at the pointer target. For more
    information, see \l SplitError.

    If \env is not NULL, performs variable substitution with the
    given environment.

    Returns a list of unquoted words or an empty list if an error occurred.

    \section1 Windows

    The behavior is defined by the Microsoft C runtime:
    \list
    \li Whitespace splits tokens.
    \li A string enclosed in double quotes is not split.
    \list
        \li 3N double quotes within a quoted string yield N literal quotes.
           This is not documented on MSDN.
    \endlist
    \li Backslashes have special semantics if they are followed by a double quote:
    \list
        \li 2N backslashes + double quote => N backslashes and begin/end quoting
        \li 2N+1 backslashes + double quote => N backslashes + literal quote
    \endlist
    \endlist
    Qt and many other implementations comply with this standard, but many do not.

    If \a abortOnMeta is \c true, cmd shell semantics are applied before
    proceeding with word splitting:
    \list
    \li Cmd ignores \e all special chars between double quotes.
        Note that the quotes are \e not removed at this stage - the
        tokenization rules described above still apply.
    \li The \c circumflex is the escape char for everything including itself.
    \endlist
    As the quoting levels are independent from each other and have different
    semantics, you need a command line like \c{"foo "\^"" bar"} to get
    \c{foo " bar}.
 */


static QStringList splitArgsWin(const QString &_args, bool abortOnMeta,
                                ProcessArgs::SplitError *err,
                                const Environment *env, const QString *pwd)
{
    if (abortOnMeta) {
        ProcessArgs::SplitError perr;
        if (!err)
            err = &perr;
        QString args = prepareArgsWin(_args, &perr, env, pwd).toWindowsArgs();
        if (*err != ProcessArgs::SplitOk)
            return QStringList();
        return doSplitArgsWin(args, err);
    } else {
        QString args = _args;
        if (env)
            envExpandWin(args, env, pwd);
        return doSplitArgsWin(args, err);
    }
}


static bool isMetaUnix(QChar cUnicode)
{
    static const uchar iqm[] = {
        0x00, 0x00, 0x00, 0x00, 0xdc, 0x07, 0x00, 0xd8,
        0x00, 0x00, 0x00, 0x38, 0x01, 0x00, 0x00, 0x38
    }; // \'"$`<>|;&(){}*?#[]

    uint c = cUnicode.unicode();

    return (c < sizeof(iqm) * 8) && (iqm[c / 8] & (1 << (c & 7)));
}

static QStringList splitArgsUnix(const QString &args, bool abortOnMeta,
                                 ProcessArgs::SplitError *err,
                                 const Environment *env, const QString *pwd)
{
    static const QString pwdName = QLatin1String("PWD");
    QStringList ret;

    for (int pos = 0; ; ) {
        QChar c;
        do {
            if (pos >= args.length())
                goto okret;
            c = args.unicode()[pos++];
        } while (c.isSpace());
        QString cret;
        bool hadWord = false;
        if (c == QLatin1Char('~')) {
            if (pos >= args.length()
                || args.unicode()[pos].isSpace() || args.unicode()[pos] == QLatin1Char('/')) {
                cret = QDir::homePath();
                hadWord = true;
                goto getc;
            } else if (abortOnMeta) {
                goto metaerr;
            }
        }
        do {
            if (c == QLatin1Char('\'')) {
                int spos = pos;
                do {
                    if (pos >= args.length())
                        goto quoteerr;
                    c = args.unicode()[pos++];
                } while (c != QLatin1Char('\''));
                cret += args.mid(spos, pos - spos - 1);
                hadWord = true;
            } else if (c == QLatin1Char('"')) {
                for (;;) {
                    if (pos >= args.length())
                        goto quoteerr;
                    c = args.unicode()[pos++];
                  nextq:
                    if (c == QLatin1Char('"'))
                        break;
                    if (c == QLatin1Char('\\')) {
                        if (pos >= args.length())
                            goto quoteerr;
                        c = args.unicode()[pos++];
                        if (c != QLatin1Char('"') &&
                            c != QLatin1Char('\\') &&
                            !(abortOnMeta &&
                              (c == QLatin1Char('$') ||
                               c == QLatin1Char('`'))))
                            cret += QLatin1Char('\\');
                    } else if (c == QLatin1Char('$') && env) {
                        if (pos >= args.length())
                            goto quoteerr;
                        c = args.unicode()[pos++];
                        bool braced = false;
                        if (c == QLatin1Char('{')) {
                            if (pos >= args.length())
                                goto quoteerr;
                            c = args.unicode()[pos++];
                            braced = true;
                        }
                        QString var;
                        while (c.isLetterOrNumber() || c == QLatin1Char('_')) {
                            var += c;
                            if (pos >= args.length())
                                goto quoteerr;
                            c = args.unicode()[pos++];
                        }
                        if (var == pwdName && pwd && !pwd->isEmpty()) {
                            cret += *pwd;
                        } else {
                            Environment::const_iterator vit = env->constFind(var);
                            if (vit == env->constEnd()) {
                                if (abortOnMeta)
                                    goto metaerr; // Assume this is a shell builtin
                            } else {
                                cret += env->expandedValueForKey(env->key(vit));
                            }
                        }
                        if (!braced)
                            goto nextq;
                        if (c != QLatin1Char('}')) {
                            if (abortOnMeta)
                                goto metaerr; // Assume this is a complex expansion
                            goto quoteerr; // Otherwise it's just garbage
                        }
                        continue;
                    } else if (abortOnMeta &&
                               (c == QLatin1Char('$') ||
                                c == QLatin1Char('`'))) {
                        goto metaerr;
                    }
                    cret += c;
                }
                hadWord = true;
            } else if (c == QLatin1Char('$') && env) {
                if (pos >= args.length())
                    goto quoteerr; // Bash just takes it verbatim, but whatever
                c = args.unicode()[pos++];
                bool braced = false;
                if (c == QLatin1Char('{')) {
                    if (pos >= args.length())
                        goto quoteerr;
                    c = args.unicode()[pos++];
                    braced = true;
                }
                QString var;
                while (c.isLetterOrNumber() || c == QLatin1Char('_')) {
                    var += c;
                    if (pos >= args.length()) {
                        if (braced)
                            goto quoteerr;
                        c = QLatin1Char(' ');
                        break;
                    }
                    c = args.unicode()[pos++];
                }
                QString val;
                if (var == pwdName && pwd && !pwd->isEmpty()) {
                    val = *pwd;
                } else {
                    Environment::const_iterator vit = env->constFind(var);
                    if (vit == env->constEnd()) {
                        if (abortOnMeta)
                            goto metaerr; // Assume this is a shell builtin
                    } else {
                        val = env->expandedValueForKey(env->key(vit));
                    }
                }
                for (int i = 0; i < val.length(); i++) {
                    const QChar cc = val.unicode()[i];
                    if (cc.unicode() == 9 || cc.unicode() == 10 || cc.unicode() == 32) {
                        if (hadWord) {
                            ret += cret;
                            cret.clear();
                            hadWord = false;
                        }
                    } else {
                        cret += cc;
                        hadWord = true;
                    }
                }
                if (!braced)
                    goto nextc;
                if (c != QLatin1Char('}')) {
                    if (abortOnMeta)
                        goto metaerr; // Assume this is a complex expansion
                    goto quoteerr; // Otherwise it's just garbage
                }
            } else {
                if (c == QLatin1Char('\\')) {
                    if (pos >= args.length())
                        goto quoteerr;
                    c = args.unicode()[pos++];
                } else if (abortOnMeta && isMetaUnix(c)) {
                    goto metaerr;
                }
                cret += c;
                hadWord = true;
            }
          getc:
            if (pos >= args.length())
                break;
            c = args.unicode()[pos++];
          nextc: ;
        } while (!c.isSpace());
        if (hadWord)
            ret += cret;
    }

  okret:
    if (err)
        *err = ProcessArgs::SplitOk;
    return ret;

  quoteerr:
    if (err)
        *err = ProcessArgs::BadQuoting;
    return QStringList();

  metaerr:
    if (err)
        *err = ProcessArgs::FoundMeta;
    return QStringList();
}

inline static bool isSpecialCharUnix(ushort c)
{
    // Chars that should be quoted (TM). This includes:
    static const uchar iqm[] = {
        0xff, 0xff, 0xff, 0xff, 0xdf, 0x07, 0x00, 0xd8,
        0x00, 0x00, 0x00, 0x38, 0x01, 0x00, 0x00, 0x78
    }; // 0-32 \'"$`<>|;&(){}*?#!~[]

    return (c < sizeof(iqm) * 8) && (iqm[c / 8] & (1 << (c & 7)));
}

inline static bool hasSpecialCharsUnix(const QString &arg)
{
    for (int x = arg.length() - 1; x >= 0; --x)
        if (isSpecialCharUnix(arg.unicode()[x].unicode()))
            return true;
    return false;
}

QStringList ProcessArgs::splitArgs(const QString &args, OsType osType,
                                   bool abortOnMeta, ProcessArgs::SplitError *err,
                                   const Environment *env, const QString *pwd)
{
    if (osType == OsTypeWindows)
        return splitArgsWin(args, abortOnMeta, err, env, pwd);
    else
        return splitArgsUnix(args, abortOnMeta, err, env, pwd);
}

QString ProcessArgs::quoteArgUnix(const QString &arg)
{
    if (arg.isEmpty())
        return QString::fromLatin1("''");

    QString ret(arg);
    if (hasSpecialCharsUnix(ret)) {
        ret.replace(QLatin1Char('\''), QLatin1String("'\\''"));
        ret.prepend(QLatin1Char('\''));
        ret.append(QLatin1Char('\''));
    }
    return ret;
}

static bool isSpecialCharWin(ushort c)
{
    // Chars that should be quoted (TM). This includes:
    // - control chars & space
    // - the shell meta chars "&()<>^|
    // - the potential separators ,;=
    static const uchar iqm[] = {
        0xff, 0xff, 0xff, 0xff, 0x45, 0x13, 0x00, 0x78,
        0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x10
    };

    return (c < sizeof(iqm) * 8) && (iqm[c / 8] & (1 << (c & 7)));
}

static bool hasSpecialCharsWin(const QString &arg)
{
    for (int x = arg.length() - 1; x >= 0; --x)
        if (isSpecialCharWin(arg.unicode()[x].unicode()))
            return true;
    return false;
}

static QString quoteArgWin(const QString &arg)
{
    if (arg.isEmpty())
        return QString::fromLatin1("\"\"");

    QString ret(arg);
    if (hasSpecialCharsWin(ret)) {
        // Quotes are escaped and their preceding backslashes are doubled.
        // It's impossible to escape anything inside a quoted string on cmd
        // level, so the outer quoting must be "suspended".
        ret.replace(QRegularExpression(QLatin1String("(\\\\*)\"")), QLatin1String("\"\\1\\1\\^\"\""));
        // The argument must not end with a \ since this would be interpreted
        // as escaping the quote -- rather put the \ behind the quote: e.g.
        // rather use "foo"\ than "foo\"
        int i = ret.length();
        while (i > 0 && ret.at(i - 1) == QLatin1Char('\\'))
            --i;
        ret.insert(i, QLatin1Char('"'));
        ret.prepend(QLatin1Char('"'));
    }
    // FIXME: Without this, quoting is not foolproof. But it needs support in the process setup, etc.
    //ret.replace('%', QLatin1String("%PERCENT_SIGN%"));
    return ret;
}

ProcessArgs ProcessArgs::prepareArgs(const QString &cmd, SplitError *err, OsType osType,
                                   const Environment *env, const QString *pwd, bool abortOnMeta)
{
    if (osType == OsTypeWindows)
        return prepareArgsWin(cmd, err, env, pwd);
    else
        return createUnixArgs(splitArgs(cmd, osType, abortOnMeta, err, env, pwd));
}


QString ProcessArgs::quoteArg(const QString &arg, OsType osType)
{
    if (osType == OsTypeWindows)
        return quoteArgWin(arg);
    else
        return quoteArgUnix(arg);
}

void ProcessArgs::addArg(QString *args, const QString &arg, OsType osType)
{
    if (!args->isEmpty())
        *args += QLatin1Char(' ');
    *args += quoteArg(arg, osType);
}

QString ProcessArgs::joinArgs(const QStringList &args, OsType osType)
{
    QString ret;
    for (const QString &arg : args)
        addArg(&ret, arg, osType);
    return ret;
}

void ProcessArgs::addArgs(QString *args, const QString &inArgs)
{
    if (!inArgs.isEmpty()) {
        if (!args->isEmpty())
            *args += QLatin1Char(' ');
        *args += inArgs;
    }
}

void ProcessArgs::addArgs(QString *args, const QStringList &inArgs)
{
    for (const QString &arg : inArgs)
        addArg(args, arg);
}

bool ProcessArgs::prepareCommand(const QString &command, const QString &arguments,
                                 QString *outCmd, ProcessArgs *outArgs, OsType osType,
                                 const Environment *env, const QString *pwd)
{
    ProcessArgs::SplitError err;
    *outArgs = ProcessArgs::prepareArgs(arguments, &err, osType, env, pwd);
    if (err == ProcessArgs::SplitOk) {
        *outCmd = command;
    } else {
        if (osType == OsTypeWindows) {
            *outCmd = QString::fromLatin1(qgetenv("COMSPEC"));
            *outArgs = ProcessArgs::createWindowsArgs(QLatin1String("/v:off /s /c \"")
                    + quoteArg(QDir::toNativeSeparators(command)) + QLatin1Char(' ') + arguments
                    + QLatin1Char('"'));
        } else {
            if (err != ProcessArgs::FoundMeta)
                return false;
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
            *outCmd = qEnvironmentVariable("SHELL", "/bin/sh");
#else
            // for sdktool
            *outCmd = qEnvironmentVariableIsSet("SHELL") ? QString::fromLocal8Bit(qgetenv("SHELL"))
                                                         : QString("/bin/sh");
#endif
            *outArgs = ProcessArgs::createUnixArgs(
                        QStringList({"-c", (quoteArg(command) + ' ' + arguments)}));
        }
    }
    return true;
}

namespace Internal {

// Data for one channel buffer (stderr/stdout)
class ChannelBuffer : public QObject
{
public:
    void clearForRun();

    QString linesRead();
    void append(const QByteArray &text, bool emitSignals);

    QByteArray rawData;
    QString incompleteLineBuffer; // lines not yet signaled
    QTextCodec *codec = nullptr; // Not owner
    std::unique_ptr<QTextCodec::ConverterState> codecState;
    int rawDataPos = 0;
    std::function<void(const QString &lines)> outputCallback;
};

class QtcProcessPrivate : public QObject
{
public:
    explicit QtcProcessPrivate(QtcProcess *parent) : q(parent) {}

    void setupChildProcess_impl();

    CommandLine m_commandLine;
    Environment m_environment;
    bool m_haveEnv = false;
    bool m_useCtrlCStub = false;
    bool m_lowPriority = false;
    bool m_disableUnixTerminal = false;

    bool m_synchronous = false;
    QProcess::OpenMode m_openMode = QProcess::ReadWrite;

    // SynchronousProcess left overs:
    void slotTimeout();
    void slotFinished(int exitCode, QProcess::ExitStatus e);
    void slotError(QProcess::ProcessError);
    void clearForRun();

    QtcProcess *q;
    QTextCodec *m_codec = QTextCodec::codecForLocale();
    QTimer m_timer;
    QEventLoop m_eventLoop;
    SynchronousProcessResponse m_result;
    FilePath m_binary;
    ChannelBuffer m_stdOut;
    ChannelBuffer m_stdErr;
    ExitCodeInterpreter m_exitCodeInterpreter = defaultExitCodeInterpreter;

    int m_hangTimerCount = 0;
    int m_maxHangTimerCount = defaultMaxHangTimerCount;
    bool m_startFailure = false;
    bool m_timeOutMessageBoxEnabled = false;
    bool m_waitingForUser = false;
};

void QtcProcessPrivate::clearForRun()
{
    m_hangTimerCount = 0;
    m_stdOut.clearForRun();
    m_stdOut.codec = m_codec;
    m_stdErr.clearForRun();
    m_stdErr.codec = m_codec;
    m_result.clear();
    m_result.codec = m_codec;
    m_startFailure = false;
    m_binary = {};
}

} // Internal

QtcProcess::QtcProcess(QObject *parent)
    : QProcess(parent), d(new QtcProcessPrivate(this))
{
    static int qProcessExitStatusMeta = qRegisterMetaType<QProcess::ExitStatus>();
    static int qProcessProcessErrorMeta = qRegisterMetaType<QProcess::ProcessError>();
    Q_UNUSED(qProcessExitStatusMeta)
    Q_UNUSED(qProcessProcessErrorMeta)

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_UNIX)
    setChildProcessModifier([this] { setupChildProcess_impl(); });
#endif
}

QtcProcess::~QtcProcess()
{
    delete d;
}

void QtcProcess::setEnvironment(const Environment &env)
{
    d->m_environment = env;
    d->m_haveEnv = true;
}

const Environment &QtcProcess::environment() const
{
    return d->m_environment;
}

void QtcProcess::setCommand(const CommandLine &cmdLine)
{
    d->m_commandLine  = cmdLine;
}

const CommandLine &QtcProcess::commandLine() const
{
    return d->m_commandLine;
}

void QtcProcess::setUseCtrlCStub(bool enabled)
{
    // Do not use the stub in debug mode. Activating the stub will shut down
    // Qt Creator otherwise, because they share the same Windows console.
    // See QTCREATORBUG-11995 for details.
#ifndef QT_DEBUG
    d->m_useCtrlCStub = enabled;
#else
    Q_UNUSED(enabled)
#endif
}

void QtcProcess::start()
{
    if (d->m_commandLine.executable().needsDevice()) {
        QTC_ASSERT(s_remoteRunProcessHook, return);
        s_remoteRunProcessHook(*this);
        return;
    }

    Environment env;
    const OsType osType = HostOsInfo::hostOs();
    if (d->m_haveEnv) {
        if (d->m_environment.size() == 0)
            qWarning("QtcProcess::start: Empty environment set when running '%s'.",
                     qPrintable(d->m_commandLine.executable().toString()));
        env = d->m_environment;

        QProcess::setProcessEnvironment(env.toProcessEnvironment());
    } else {
        env = Environment::systemEnvironment();
    }

    const QString &workDir = workingDirectory();
    QString command;
    ProcessArgs arguments;
    bool success = ProcessArgs::prepareCommand(d->m_commandLine.executable().toString(),
                                               d->m_commandLine.arguments(),
                                               &command, &arguments, osType, &env, &workDir);
    if (osType == OsTypeWindows) {
        QString args;
        if (d->m_useCtrlCStub) {
            if (d->m_lowPriority)
                ProcessArgs::addArg(&args, "-nice");
            ProcessArgs::addArg(&args, QDir::toNativeSeparators(command));
            command = QCoreApplication::applicationDirPath()
                    + QLatin1String("/qtcreator_ctrlc_stub.exe");
        } else if (d->m_lowPriority) {
#ifdef Q_OS_WIN
            setCreateProcessArgumentsModifier([](CreateProcessArguments *args) {
                args->flags |= BELOW_NORMAL_PRIORITY_CLASS;
            });
#endif
        }
        ProcessArgs::addArgs(&args, arguments.toWindowsArgs());
#ifdef Q_OS_WIN
        setNativeArguments(args);
#endif
        // Note: Arguments set with setNativeArgs will be appended to the ones
        // passed with start() below.
        QProcess::start(command, QStringList(), d->m_openMode);
    } else {
        if (!success) {
            setErrorString(tr("Error in command line."));
            // Should be FailedToStart, but we cannot set the process error from the outside,
            // so it would be inconsistent.
            emit errorOccurred(QProcess::UnknownError);
            return;
        }
        QProcess::start(command, arguments.toUnixArgs(), d->m_openMode);
    }

    if (d->m_synchronous)
        QProcess::waitForFinished();
}

#ifdef Q_OS_WIN
static BOOL sendMessage(UINT message, HWND hwnd, LPARAM lParam)
{
    DWORD dwProcessID;
    GetWindowThreadProcessId(hwnd, &dwProcessID);
    if ((DWORD)lParam == dwProcessID) {
        SendNotifyMessage(hwnd, message, 0, 0);
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK sendShutDownMessageToAllWindowsOfProcess_enumWnd(HWND hwnd, LPARAM lParam)
{
    static UINT uiShutDownMessage = RegisterWindowMessage(L"qtcctrlcstub_shutdown");
    return sendMessage(uiShutDownMessage, hwnd, lParam);
}

BOOL CALLBACK sendInterruptMessageToAllWindowsOfProcess_enumWnd(HWND hwnd, LPARAM lParam)
{
    static UINT uiInterruptMessage = RegisterWindowMessage(L"qtcctrlcstub_interrupt");
    return sendMessage(uiInterruptMessage, hwnd, lParam);
}
#endif

void QtcProcess::terminate()
{
#ifdef Q_OS_WIN
    if (d->m_useCtrlCStub)
        EnumWindows(sendShutDownMessageToAllWindowsOfProcess_enumWnd, processId());
    else
#endif
    QProcess::terminate();
}

void QtcProcess::interrupt()
{
#ifdef Q_OS_WIN
    QTC_ASSERT(d->m_useCtrlCStub, return);
    EnumWindows(sendInterruptMessageToAllWindowsOfProcess_enumWnd, processId());
#endif
}

void QtcProcess::setLowPriority()
{
    d->m_lowPriority = true;
}

void QtcProcess::setDisableUnixTerminal()
{
    d->m_disableUnixTerminal = true;
}

// This function assumes that the resulting string will be quoted.
// That's irrelevant if it does not contain quotes itself.
static int quoteArgInternalWin(QString &ret, int bslashes)
{
    // Quotes are escaped and their preceding backslashes are doubled.
    // It's impossible to escape anything inside a quoted string on cmd
    // level, so the outer quoting must be "suspended".
    const QChar bs(QLatin1Char('\\')), dq(QLatin1Char('"'));
    for (int p = 0; p < ret.length(); p++) {
        if (ret.at(p) == bs) {
            bslashes++;
        } else {
            if (ret.at(p) == dq) {
                if (bslashes) {
                    ret.insert(p, QString(bslashes, bs));
                    p += bslashes;
                }
                ret.insert(p, QLatin1String("\"\\^\""));
                p += 4;
            }
            bslashes = 0;
        }
    }
    return bslashes;
}


// TODO: This documentation is relevant for end-users. Where to put it?
/**
 * Perform safe macro expansion (substitution) on a string for use
 * in shell commands.
 *
 * \section Unix notes
 *
 * Explicitly supported shell constructs:
 *   \\ '' "" {} () $(()) ${} $() ``
 *
 * Implicitly supported shell constructs:
 *   (())
 *
 * Unsupported shell constructs that will cause problems:
 * \list
 * \li Shortened \c{case $v in pat)} syntax. Use \c{case $v in (pat)} instead.
 * \li Bash-style \c{$""} and \c{$''} string quoting syntax.
 * \endlist
 *
 * The rest of the shell (incl. bash) syntax is simply ignored,
 * as it is not expected to cause problems.
 *
 * Security considerations:
 * \list
 * \li Backslash-escaping an expando is treated as a quoting error
 * \li Do not put expandos into double quoted substitutions:
 *     \badcode
 *       "${VAR:-%{macro}}"
 *     \endcode
 * \li Do not put expandos into command line arguments which are nested
 *     shell commands:
 *     \badcode
 *       sh -c 'foo \%{file}'
 *     \endcode
 *     \goodcode
 *       file=\%{file} sh -c 'foo "$file"'
 *     \endcode
 * \endlist
 *
 * \section Windows notes
 *
 * All quoting syntax supported by splitArgs() is supported here as well.
 * Additionally, command grouping via parentheses is recognized - note
 * however, that the parser is much stricter about unquoted parentheses
 * than cmd itself.
 * The rest of the cmd syntax is simply ignored, as it is not expected
 * to cause problems.
 *
 * Security considerations:
 * \list
 * \li Circumflex-escaping an expando is treated as a quoting error
 * \li Closing double quotes right before expandos and opening double quotes
 *     right after expandos are treated as quoting errors
 * \li Do not put expandos into nested commands:
 *     \badcode
 *       for /f "usebackq" \%v in (`foo \%{file}`) do \@echo \%v
 *     \endcode
 * \li A macro's value must not contain anything which may be interpreted
 *     as an environment variable expansion. A solution is replacing any
 *     percent signs with a fixed string like \c{\%PERCENT_SIGN\%} and
 *     injecting \c{PERCENT_SIGN=\%} into the shell's environment.
 * \li Enabling delayed environment variable expansion (cmd /v:on) should have
 *     no security implications, but may still wreak havoc due to the
 *     need for doubling circumflexes if any exclamation marks are present,
 *     and the need to circumflex-escape the exclamation marks themselves.
 * \endlist
 *
 * \param cmd pointer to the string in which macros are expanded in-place
 * \param mx pointer to a macro expander instance
 * \return false if the string could not be parsed and therefore no safe
 *   substitution was possible
 */
bool ProcessArgs::expandMacros(QString *cmd, AbstractMacroExpander *mx, OsType osType)
{
    QString str = *cmd;
    if (str.isEmpty())
        return true;

    QString rsts;
    int varLen;
    int varPos = 0;
    if (!(varLen = mx->findMacro(str, &varPos, &rsts)))
        return true;

    int pos = 0;

    if (osType == OsTypeWindows) {
        enum { // cmd.exe parsing state
            ShellBasic, // initial state
            ShellQuoted, // double-quoted state => *no* other meta chars are interpreted
            ShellEscaped // circumflex-escaped state => next char is not interpreted
        } shellState = ShellBasic;
        enum { // CommandLineToArgv() parsing state and some more
            CrtBasic, // initial state
            CrtNeedWord, // after empty expando; insert empty argument if whitespace follows
            CrtInWord, // in non-whitespace
            CrtClosed, // previous char closed the double-quoting
            CrtHadQuote, // closed double-quoting after an expando
            // The remaining two need to be numerically higher
            CrtQuoted, // double-quoted state => spaces don't split tokens
            CrtNeedQuote // expando opened quote; close if no expando follows
        } crtState = CrtBasic;
        int bslashes = 0; // previous chars were manual backslashes
        int rbslashes = 0; // trailing backslashes in replacement

        forever {
            if (pos == varPos) {
                if (shellState == ShellEscaped)
                    return false; // Circumflex'd quoted expando would be Bad (TM).
                if ((shellState == ShellQuoted) != (crtState == CrtQuoted))
                    return false; // CRT quoting out of sync with shell quoting. Ahoy to Redmond.
                rbslashes += bslashes;
                bslashes = 0;
                if (crtState < CrtQuoted) {
                    if (rsts.isEmpty()) {
                        if (crtState == CrtBasic) {
                            // Outside any quoting and the string is empty, so put
                            // a pair of quotes. Delaying that is just pedantry.
                            crtState = CrtNeedWord;
                        }
                    } else {
                        if (hasSpecialCharsWin(rsts)) {
                            if (crtState == CrtClosed) {
                                // Quoted expando right after closing quote. Can't do that.
                                return false;
                            }
                            int tbslashes = quoteArgInternalWin(rsts, 0);
                            rsts.prepend(QLatin1Char('"'));
                            if (rbslashes)
                                rsts.prepend(QString(rbslashes, QLatin1Char('\\')));
                            crtState = CrtNeedQuote;
                            rbslashes = tbslashes;
                        } else {
                            crtState = CrtInWord; // We know that this string contains no spaces.
                            // We know that this string contains no quotes,
                            // so the function won't make a mess.
                            rbslashes = quoteArgInternalWin(rsts, rbslashes);
                        }
                    }
                } else {
                    rbslashes = quoteArgInternalWin(rsts, rbslashes);
                }
                str.replace(pos, varLen, rsts);
                pos += rsts.length();
                varPos = pos;
                if (!(varLen = mx->findMacro(str, &varPos, &rsts))) {
                    // Don't leave immediately, as we may be in CrtNeedWord state which could
                    // be still resolved, or we may have inserted trailing backslashes.
                    varPos = INT_MAX;
                }
                continue;
            }
            if (crtState == CrtNeedQuote) {
                if (rbslashes) {
                    str.insert(pos, QString(rbslashes, QLatin1Char('\\')));
                    pos += rbslashes;
                    varPos += rbslashes;
                    rbslashes = 0;
                }
                str.insert(pos, QLatin1Char('"'));
                pos++;
                varPos++;
                crtState = CrtHadQuote;
            }
            ushort cc = str.unicode()[pos].unicode();
            if (shellState == ShellBasic && cc == '^') {
                shellState = ShellEscaped;
            } else {
                if (!cc || cc == ' ' || cc == '\t') {
                    if (crtState < CrtQuoted) {
                        if (crtState == CrtNeedWord) {
                            str.insert(pos, QLatin1String("\"\""));
                            pos += 2;
                            varPos += 2;
                        }
                        crtState = CrtBasic;
                    }
                    if (!cc)
                        break;
                    bslashes = 0;
                    rbslashes = 0;
                } else {
                    if (cc == '\\') {
                        bslashes++;
                        if (crtState < CrtQuoted)
                            crtState = CrtInWord;
                    } else {
                        if (cc == '"') {
                            if (shellState != ShellEscaped)
                                shellState = (shellState == ShellQuoted) ? ShellBasic : ShellQuoted;
                            if (rbslashes) {
                                // Offset -1: skip possible circumflex. We have at least
                                // one backslash, so a fixed offset is ok.
                                str.insert(pos - 1, QString(rbslashes, QLatin1Char('\\')));
                                pos += rbslashes;
                                varPos += rbslashes;
                            }
                            if (!(bslashes & 1)) {
                                // Even number of backslashes, so the quote is not escaped.
                                switch (crtState) {
                                case CrtQuoted:
                                    // Closing quote
                                    crtState = CrtClosed;
                                    break;
                                case CrtClosed:
                                    // Two consecutive quotes make a literal quote - and
                                    // still close quoting. See quoteArg().
                                    crtState = CrtInWord;
                                    break;
                                case CrtHadQuote:
                                    // Opening quote right after quoted expando. Can't do that.
                                    return false;
                                default:
                                    // Opening quote
                                    crtState = CrtQuoted;
                                    break;
                                }
                            } else if (crtState < CrtQuoted) {
                                crtState = CrtInWord;
                            }
                        } else if (crtState < CrtQuoted) {
                            crtState = CrtInWord;
                        }
                        bslashes = 0;
                        rbslashes = 0;
                    }
                }
                if (varPos == INT_MAX && !rbslashes)
                    break;
                if (shellState == ShellEscaped)
                    shellState = ShellBasic;
            }
            pos++;
        }
    } else {
        // !Windows
        MxState state = {MxBasic, false};
        QStack<MxState> sstack;
        QStack<MxSave> ostack;

        while (pos < str.length()) {
            if (pos == varPos) {
                // Our expansion rules trigger in any context
                if (state.dquote) {
                    // We are within a double-quoted string. Escape relevant meta characters.
                    rsts.replace(QRegularExpression(QLatin1String("([$`\"\\\\])")), QLatin1String("\\\\1"));
                } else if (state.current == MxSingleQuote) {
                    // We are within a single-quoted string. "Suspend" single-quoting and put a
                    // single escaped quote for each single quote inside the string.
                    rsts.replace(QLatin1Char('\''), QLatin1String("'\\''"));
                } else if (rsts.isEmpty() || hasSpecialCharsUnix(rsts)) {
                    // String contains "quote-worthy" characters. Use single quoting - but
                    // that choice is arbitrary.
                    rsts.replace(QLatin1Char('\''), QLatin1String("'\\''"));
                    rsts.prepend(QLatin1Char('\''));
                    rsts.append(QLatin1Char('\''));
                } // Else just use the string verbatim.
                str.replace(pos, varLen, rsts);
                pos += rsts.length();
                varPos = pos;
                if (!(varLen = mx->findMacro(str, &varPos, &rsts)))
                    break;
                continue;
            }
            ushort cc = str.unicode()[pos].unicode();
            if (state.current == MxSingleQuote) {
                // Single quoted context - only the single quote has any special meaning.
                if (cc == '\'')
                    state = sstack.pop();
            } else if (cc == '\\') {
                // In any other context, the backslash starts an escape.
                pos += 2;
                if (varPos < pos)
                    return false; // Backslash'd quoted expando would be Bad (TM).
                continue;
            } else if (cc == '$') {
                cc = str.unicode()[++pos].unicode();
                if (cc == '(') {
                    sstack.push(state);
                    if (str.unicode()[pos + 1].unicode() == '(') {
                        // $(( starts a math expression. This may also be a $( ( in fact,
                        // so we push the current string and offset on a stack so we can retry.
                        MxSave sav = {str, pos + 2, varPos};
                        ostack.push(sav);
                        state.current = MxMath;
                        pos += 2;
                        continue;
                    } else {
                        // $( starts a command substitution. This actually "opens a new context"
                        // which overrides surrounding double quoting.
                        state.current = MxParen;
                        state.dquote = false;
                    }
                } else if (cc == '{') {
                    // ${ starts a "braced" variable substitution.
                    sstack.push(state);
                    state.current = MxSubst;
                } // Else assume that a "bare" variable substitution has started
            } else if (cc == '`') {
                // Backticks are evil, as every shell interprets escapes within them differently,
                // which is a danger for the quoting of our own expansions.
                // So we just apply *our* rules (which match bash) and transform it into a POSIX
                // command substitution which has clear semantics.
                str.replace(pos, 1, QLatin1String("$( " )); // add space -> avoid creating $((
                varPos += 2;
                int pos2 = pos += 3;
                forever {
                    if (pos2 >= str.length())
                        return false; // Syntax error - unterminated backtick expression.
                    cc = str.unicode()[pos2].unicode();
                    if (cc == '`')
                        break;
                    if (cc == '\\') {
                        cc = str.unicode()[++pos2].unicode();
                        if (cc == '$' || cc == '`' || cc == '\\' ||
                            (cc == '"' && state.dquote))
                        {
                            str.remove(pos2 - 1, 1);
                            if (varPos >= pos2)
                                varPos--;
                            continue;
                        }
                    }
                    pos2++;
                }
                str[pos2] = QLatin1Char(')');
                sstack.push(state);
                state.current = MxParen;
                state.dquote = false;
                continue;
            } else if (state.current == MxDoubleQuote) {
                // (Truly) double quoted context - only remaining special char is the closing quote.
                if (cc == '"')
                    state = sstack.pop();
            } else if (cc == '\'') {
                // Start single quote if we are not in "inherited" double quoted context.
                if (!state.dquote) {
                    sstack.push(state);
                    state.current = MxSingleQuote;
                }
            } else if (cc == '"') {
                // Same for double quoting.
                if (!state.dquote) {
                    sstack.push(state);
                    state.current = MxDoubleQuote;
                    state.dquote = true;
                }
            } else if (state.current == MxSubst) {
                // "Braced" substitution context - only remaining special char is the closing brace.
                if (cc == '}')
                    state = sstack.pop();
            } else if (cc == ')') {
                if (state.current == MxMath) {
                    if (str.unicode()[pos + 1].unicode() == ')') {
                        state = sstack.pop();
                        pos += 2;
                    } else {
                        // False hit: the $(( was a $( ( in fact.
                        // ash does not care (and will complain), but bash actually parses it.
                        varPos = ostack.top().varPos;
                        pos = ostack.top().pos;
                        str = ostack.top().str;
                        ostack.pop();
                        state.current = MxParen;
                        state.dquote = false;
                        sstack.push(state);
                    }
                    continue;
                } else if (state.current == MxParen) {
                    state = sstack.pop();
                } else {
                    break; // Syntax error - excess closing parenthesis.
                }
            } else if (cc == '}') {
                if (state.current == MxGroup)
                    state = sstack.pop();
                else
                    break; // Syntax error - excess closing brace.
            } else if (cc == '(') {
                // Context-saving command grouping.
                sstack.push(state);
                state.current = MxParen;
            } else if (cc == '{') {
                // Plain command grouping.
                sstack.push(state);
                state.current = MxGroup;
            }
            pos++;
        }
        // FIXME? May complain if (!sstack.empty()), but we don't really care anyway.
    }

    *cmd = str;
    return true;
}

QString ProcessArgs::expandMacros(const QString &str, AbstractMacroExpander *mx, OsType osType)
{
    QString ret = str;
    expandMacros(&ret, mx, osType);
    return ret;
}

void QtcProcess::setRemoteStartProcessHook(const std::function<void(QtcProcess &)> &hook)
{
    s_remoteRunProcessHook = hook;
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
void QtcProcess::setupChildProcess()
{
    d->setupChildProcess_impl();
}
#endif

void QtcProcessPrivate::setupChildProcess_impl()
{
#if defined Q_OS_UNIX
    // nice value range is -20 to +19 where -20 is highest, 0 default and +19 is lowest
    if (m_lowPriority) {
        errno = 0;
        if (::nice(5) == -1 && errno != 0)
            perror("Failed to set nice value");
    }

    // Disable terminal by becoming a session leader.
    if (m_disableUnixTerminal)
        setsid();
#endif
}

bool QtcProcess::isSynchronous() const
{
    return d->m_synchronous;
}

void QtcProcess::setSynchronous(bool on)
{
    d->m_synchronous = on;
}

void QtcProcess::setOpenMode(OpenMode mode)
{
    d->m_openMode = mode;
}

bool QtcProcess::stopProcess()
{
    if (state() == QProcess::NotRunning)
        return true;
    terminate();
    if (waitForFinished(300))
        return true;
    kill();
    return waitForFinished(300);
}

static bool askToKill(const QString &command)
{
#ifdef QT_GUI_LIB
    if (QThread::currentThread() != QCoreApplication::instance()->thread())
        return true;
    const QString title = QtcProcess::tr("Process not Responding");
    QString msg = command.isEmpty() ?
                QtcProcess::tr("The process is not responding.") :
                QtcProcess::tr("The process \"%1\" is not responding.").arg(command);
    msg += ' ';
    msg += QtcProcess::tr("Would you like to terminate it?");
    // Restore the cursor that is set to wait while running.
    const bool hasOverrideCursor = QApplication::overrideCursor() != nullptr;
    if (hasOverrideCursor)
        QApplication::restoreOverrideCursor();
    QMessageBox::StandardButton answer = QMessageBox::question(nullptr, title, msg, QMessageBox::Yes|QMessageBox::No);
    if (hasOverrideCursor)
        QApplication::setOverrideCursor(Qt::WaitCursor);
    return answer == QMessageBox::Yes;
#else
    Q_UNUSED(command)
    return true;
#endif
}

// Helper for running a process synchronously in the foreground with timeout
// detection similar SynchronousProcess' handling (taking effect after no more output
// occurs on stderr/stdout as opposed to waitForFinished()). Returns false if a timeout
// occurs. Checking of the process' exit state/code still has to be done.

bool QtcProcess::readDataFromProcess(int timeoutS,
                                     QByteArray *stdOut,
                                     QByteArray *stdErr,
                                     bool showTimeOutMessageBox)
{
    enum { syncDebug = 0 };
    if (syncDebug)
        qDebug() << ">readDataFromProcess" << timeoutS;
    if (state() != QProcess::Running) {
        qWarning("readDataFromProcess: Process in non-running state passed in.");
        return false;
    }

    QTC_ASSERT(readChannel() == QProcess::StandardOutput, return false);

    // Keep the process running until it has no longer has data
    bool finished = false;
    bool hasData = false;
    do {
        finished = waitForFinished(timeoutS > 0 ? timeoutS * 1000 : -1)
                || state() == QProcess::NotRunning;
        // First check 'stdout'
        if (bytesAvailable()) { // applies to readChannel() only
            hasData = true;
            const QByteArray newStdOut = readAllStandardOutput();
            if (stdOut)
                stdOut->append(newStdOut);
        }
        // Check 'stderr' separately. This is a special handling
        // for 'git pull' and the like which prints its progress on stderr.
        const QByteArray newStdErr = readAllStandardError();
        if (!newStdErr.isEmpty()) {
            hasData = true;
            if (stdErr)
                stdErr->append(newStdErr);
        }
        // Prompt user, pretend we have data if says 'No'.
        const bool hang = !hasData && !finished;
        hasData = hang && showTimeOutMessageBox && !askToKill(program());
    } while (hasData && !finished);
    if (syncDebug)
        qDebug() << "<readDataFromProcess" << finished;
    return finished;
}

bool ProcessArgs::ArgIterator::next()
{
    // We delay the setting of m_prev so we can still delete the last argument
    // after we find that there are no more arguments. It's a bit of a hack ...
    int prev = m_pos;

    m_simple = true;
    m_value.clear();

    if (m_osType == OsTypeWindows) {
        enum { // cmd.exe parsing state
            ShellBasic, // initial state
            ShellQuoted, // double-quoted state => *no* other meta chars are interpreted
            ShellEscaped // circumflex-escaped state => next char is not interpreted
        } shellState = ShellBasic;
        enum { // CommandLineToArgv() parsing state and some more
            CrtBasic, // initial state
            CrtInWord, // in non-whitespace
            CrtClosed, // previous char closed the double-quoting
            CrtQuoted // double-quoted state => spaces don't split tokens
        } crtState = CrtBasic;
        enum { NoVar, NewVar, FullVar } varState = NoVar; // inside a potential env variable expansion
        int bslashes = 0; // number of preceding backslashes

        for (;; m_pos++) {
            ushort cc = m_pos < m_str->length() ? m_str->unicode()[m_pos].unicode() : 0;
            if (shellState == ShellBasic && cc == '^') {
                varState = NoVar;
                shellState = ShellEscaped;
            } else if ((shellState == ShellBasic && isMetaCharWin(cc)) || !cc) { // A "bit" simplistic ...
                // We ignore crtQuote state here. Whatever ...
              doReturn:
                if (m_simple)
                    while (--bslashes >= 0)
                        m_value += QLatin1Char('\\');
                else
                    m_value.clear();
                if (crtState != CrtBasic) {
                    m_prev = prev;
                    return true;
                }
                return false;
            } else {
                if (crtState != CrtQuoted && (cc == ' ' || cc == '\t')) {
                    if (crtState != CrtBasic) {
                        // We'll lose shellQuote state here. Whatever ...
                        goto doReturn;
                    }
                } else {
                    if (cc == '\\') {
                        bslashes++;
                        if (crtState != CrtQuoted)
                            crtState = CrtInWord;
                        varState = NoVar;
                    } else {
                        if (cc == '"') {
                            varState = NoVar;
                            if (shellState != ShellEscaped)
                                shellState = (shellState == ShellQuoted) ? ShellBasic : ShellQuoted;
                            int obslashes = bslashes;
                            bslashes >>= 1;
                            if (!(obslashes & 1)) {
                                // Even number of backslashes, so the quote is not escaped.
                                switch (crtState) {
                                case CrtQuoted:
                                    // Closing quote
                                    crtState = CrtClosed;
                                    continue;
                                case CrtClosed:
                                    // Two consecutive quotes make a literal quote - and
                                    // still close quoting. See quoteArg().
                                    crtState = CrtInWord;
                                    break;
                                default:
                                    // Opening quote
                                    crtState = CrtQuoted;
                                    continue;
                                }
                            } else if (crtState != CrtQuoted) {
                                crtState = CrtInWord;
                            }
                        } else {
                            if (cc == '%') {
                                if (varState == FullVar) {
                                    m_simple = false;
                                    varState = NoVar;
                                } else {
                                    varState = NewVar;
                                }
                            } else if (varState != NoVar) {
                                // This check doesn't really reflect cmd reality, but it is an
                                // approximation of what would be sane.
                                varState = (cc == '_' || cc == '-' || cc == '.'
                                         || QChar(cc).isLetterOrNumber()) ? FullVar : NoVar;

                            }
                            if (crtState != CrtQuoted)
                                crtState = CrtInWord;
                        }
                        for (; bslashes > 0; bslashes--)
                            m_value += QLatin1Char('\\');
                        m_value += QChar(cc);
                    }
                }
                if (shellState == ShellEscaped)
                    shellState = ShellBasic;
            }
        }
    } else {
        MxState state = {MxBasic, false};
        QStack<MxState> sstack;
        QStack<int> ostack;
        bool hadWord = false;

        for (; m_pos < m_str->length(); m_pos++) {
            ushort cc = m_str->unicode()[m_pos].unicode();
            if (state.current == MxSingleQuote) {
                if (cc == '\'') {
                    state = sstack.pop();
                    continue;
                }
            } else if (cc == '\\') {
                if (++m_pos >= m_str->length())
                    break;
                cc = m_str->unicode()[m_pos].unicode();
                if (state.dquote && cc != '"' && cc != '\\' && cc != '$' && cc != '`')
                    m_value += QLatin1Char('\\');
            } else if (cc == '$') {
                if (++m_pos >= m_str->length())
                    break;
                cc = m_str->unicode()[m_pos].unicode();
                if (cc == '(') {
                    sstack.push(state);
                    if (++m_pos >= m_str->length())
                        break;
                    if (m_str->unicode()[m_pos].unicode() == '(') {
                        ostack.push(m_pos);
                        state.current = MxMath;
                    } else {
                        state.dquote = false;
                        state.current = MxParen;
                        // m_pos too far by one now - whatever.
                    }
                } else if (cc == '{') {
                    sstack.push(state);
                    state.current = MxSubst;
                } else {
                    // m_pos too far by one now - whatever.
                }
                m_simple = false;
                hadWord = true;
                continue;
            } else if (cc == '`') {
                forever {
                    if (++m_pos >= m_str->length()) {
                        m_simple = false;
                        m_prev = prev;
                        return true;
                    }
                    cc = m_str->unicode()[m_pos].unicode();
                    if (cc == '`')
                        break;
                    if (cc == '\\')
                        m_pos++; // m_pos may be too far by one now - whatever.
                }
                m_simple = false;
                hadWord = true;
                continue;
            } else if (state.current == MxDoubleQuote) {
                if (cc == '"') {
                    state = sstack.pop();
                    continue;
                }
            } else if (cc == '\'') {
                if (!state.dquote) {
                    sstack.push(state);
                    state.current = MxSingleQuote;
                    hadWord = true;
                    continue;
                }
            } else if (cc == '"') {
                if (!state.dquote) {
                    sstack.push(state);
                    state.dquote = true;
                    state.current = MxDoubleQuote;
                    hadWord = true;
                    continue;
                }
            } else if (state.current == MxSubst) {
                if (cc == '}')
                    state = sstack.pop();
                continue; // Not simple anyway
            } else if (cc == ')') {
                if (state.current == MxMath) {
                    if (++m_pos >= m_str->length())
                        break;
                    if (m_str->unicode()[m_pos].unicode() == ')') {
                        ostack.pop();
                        state = sstack.pop();
                    } else {
                        // false hit: the $(( was a $( ( in fact.
                        // ash does not care, but bash does.
                        m_pos = ostack.pop();
                        state.current = MxParen;
                        state.dquote = false;
                        sstack.push(state);
                    }
                    continue;
                } else if (state.current == MxParen) {
                    state = sstack.pop();
                    continue;
                } else {
                    break;
                }
#if 0 // MxGroup is impossible, see below.
            } else if (cc == '}') {
                if (state.current == MxGroup) {
                    state = sstack.pop();
                    continue;
                }
#endif
            } else if (cc == '(') {
                sstack.push(state);
                state.current = MxParen;
                m_simple = false;
                hadWord = true;
                continue;
#if 0 // Should match only at the beginning of a command, which we never have currently.
            } else if (cc == '{') {
                sstack.push(state);
                state.current = MxGroup;
                m_simple = false;
                hadWord = true;
                continue;
#endif
            } else if (cc == '<' || cc == '>' || cc == '&' || cc == '|' || cc == ';') {
                if (sstack.isEmpty())
                    break;
            } else if (cc == ' ' || cc == '\t') {
                if (!hadWord)
                    continue;
                if (sstack.isEmpty())
                    break;
            }
            m_value += QChar(cc);
            hadWord = true;
        }
        // TODO: Possibly complain here if (!sstack.empty())
        if (!m_simple)
            m_value.clear();
        if (hadWord) {
            m_prev = prev;
            return true;
        }
        return false;
    }
}

QString QtcProcess::normalizeNewlines(const QString &text)
{
    QString res = text;
    const auto newEnd = std::unique(res.begin(), res.end(), [](const QChar &c1, const QChar &c2) {
        return c1 == '\r' && c2 == '\r'; // QTCREATORBUG-24556
    });
    res.chop(std::distance(newEnd, res.end()));
    res.replace("\r\n", "\n");
    return res;
}

void ProcessArgs::ArgIterator::deleteArg()
{
    if (!m_prev)
        while (m_pos < m_str->length() && m_str->at(m_pos).isSpace())
            m_pos++;
    m_str->remove(m_prev, m_pos - m_prev);
    m_pos = m_prev;
}

void ProcessArgs::ArgIterator::appendArg(const QString &str)
{
    const QString qstr = quoteArg(str);
    if (!m_pos)
        m_str->insert(0, qstr + QLatin1Char(' '));
    else
        m_str->insert(m_pos, QLatin1Char(' ') + qstr);
    m_pos += qstr.length() + 1;
}

ProcessArgs ProcessArgs::createWindowsArgs(const QString &args)
{
    ProcessArgs result;
    result.m_windowsArgs = args;
    result.m_isWindows = true;
    return result;
}

ProcessArgs ProcessArgs::createUnixArgs(const QStringList &args)
{
    ProcessArgs result;
    result.m_unixArgs = args;
    result.m_isWindows = false;
    return result;
}

QString ProcessArgs::toWindowsArgs() const
{
    QTC_CHECK(m_isWindows);
    return m_windowsArgs;
}

QStringList ProcessArgs::toUnixArgs() const
{
    QTC_CHECK(!m_isWindows);
    return m_unixArgs;
}

QString ProcessArgs::toString() const
{
    if (m_isWindows)
        return m_windowsArgs;
    else
        return ProcessArgs::joinArgs(m_unixArgs, OsTypeLinux);
}

// Path utilities

// Locate a binary in a directory, applying all kinds of
// extensions the operating system supports.
static QString checkBinary(const QDir &dir, const QString &binary)
{
    // naive UNIX approach
    const QFileInfo info(dir.filePath(binary));
    if (info.isFile() && info.isExecutable())
        return info.absoluteFilePath();

    // Does the OS have some weird extension concept or does the
    // binary have a 3 letter extension?
    if (HostOsInfo::isAnyUnixHost() && !HostOsInfo::isMacHost())
        return QString();
    const int dotIndex = binary.lastIndexOf(QLatin1Char('.'));
    if (dotIndex != -1 && dotIndex == binary.size() - 4)
        return  QString();

    switch (HostOsInfo::hostOs()) {
    case OsTypeLinux:
    case OsTypeOtherUnix:
    case OsTypeOther:
        break;
    case OsTypeWindows: {
            static const char *windowsExtensions[] = {".cmd", ".bat", ".exe", ".com"};
            // Check the Windows extensions using the order
            const int windowsExtensionCount = sizeof(windowsExtensions)/sizeof(const char*);
            for (int e = 0; e < windowsExtensionCount; e ++) {
                const QFileInfo windowsBinary(dir.filePath(binary + QLatin1String(windowsExtensions[e])));
                if (windowsBinary.isFile() && windowsBinary.isExecutable())
                    return windowsBinary.absoluteFilePath();
            }
        }
        break;
    case OsTypeMac: {
            // Check for Mac app folders
            const QFileInfo appFolder(dir.filePath(binary + QLatin1String(".app")));
            if (appFolder.isDir()) {
                QString macBinaryPath = appFolder.absoluteFilePath();
                macBinaryPath += QLatin1String("/Contents/MacOS/");
                macBinaryPath += binary;
                const QFileInfo macBinary(macBinaryPath);
                if (macBinary.isFile() && macBinary.isExecutable())
                    return macBinary.absoluteFilePath();
            }
        }
        break;
    }
    return QString();
}

QString QtcProcess::locateBinary(const QString &path, const QString &binary)
{
    // Absolute file?
    const QFileInfo absInfo(binary);
    if (absInfo.isAbsolute())
        return checkBinary(absInfo.dir(), absInfo.fileName());

    // Windows finds binaries  in the current directory
    if (HostOsInfo::isWindowsHost()) {
        const QString currentDirBinary = checkBinary(QDir::current(), binary);
        if (!currentDirBinary.isEmpty())
            return currentDirBinary;
    }

    const QStringList paths = path.split(HostOsInfo::pathListSeparator());
    if (paths.empty())
        return QString();
    const QStringList::const_iterator cend = paths.constEnd();
    for (QStringList::const_iterator it = paths.constBegin(); it != cend; ++it) {
        const QDir dir(*it);
        const QString rc = checkBinary(dir, binary);
        if (!rc.isEmpty())
            return rc;
    }
    return QString();
}

QString QtcProcess::locateBinary(const QString &binary)
{
    const QByteArray path = qgetenv("PATH");
    return locateBinary(QString::fromLocal8Bit(path), binary);
}


/*!
    \class Utils::SynchronousProcess

    \brief The SynchronousProcess class runs a synchronous process in its own
    event loop that blocks only user input events. Thus, it allows for the GUI to
    repaint and append output to log windows.

    The callbacks set with setStdOutCallBack(), setStdErrCallback() are called
    with complete lines based on the '\\n' marker.
    They would typically be used for log windows.

    There is a timeout handling that takes effect after the last data have been
    read from stdout/stdin (as opposed to waitForFinished(), which measures time
    since it was invoked). It is thus also suitable for slow processes that
    continuously output data (like version system operations).

    The property timeOutMessageBoxEnabled influences whether a message box is
    shown asking the user if they want to kill the process on timeout (default: false).

    There are also static utility functions for dealing with fully synchronous
    processes, like reading the output with correct timeout handling.

    Caution: This class should NOT be used if there is a chance that the process
    triggers opening dialog boxes (for example, by file watchers triggering),
    as this will cause event loop problems.
*/

// ----------- SynchronousProcessResponse
void SynchronousProcessResponse::clear()
{
    result = StartFailed;
    exitCode = -1;
    rawStdOut.clear();
    rawStdErr.clear();
}

QString SynchronousProcessResponse::exitMessage(const QString &binary, int timeoutS) const
{
    switch (result) {
    case Finished:
        return QtcProcess::tr("The command \"%1\" finished successfully.").arg(QDir::toNativeSeparators(binary));
    case FinishedError:
        return QtcProcess::tr("The command \"%1\" terminated with exit code %2.").arg(QDir::toNativeSeparators(binary)).arg(exitCode);
    case TerminatedAbnormally:
        return QtcProcess::tr("The command \"%1\" terminated abnormally.").arg(QDir::toNativeSeparators(binary));
    case StartFailed:
        return QtcProcess::tr("The command \"%1\" could not be started.").arg(QDir::toNativeSeparators(binary));
    case Hang:
        return QtcProcess::tr("The command \"%1\" did not respond within the timeout limit (%2 s).")
                .arg(QDir::toNativeSeparators(binary)).arg(timeoutS);
    }
    return QString();
}

QByteArray SynchronousProcessResponse::allRawOutput() const
{
    if (!rawStdOut.isEmpty() && !rawStdErr.isEmpty()) {
        QByteArray result = rawStdOut;
        if (!result.endsWith('\n'))
            result += '\n';
        result += rawStdErr;
        return result;
    }
    return !rawStdOut.isEmpty() ? rawStdOut : rawStdErr;
}

QString SynchronousProcessResponse::allOutput() const
{
    const QString out = stdOut();
    const QString err = stdErr();

    if (!out.isEmpty() && !err.isEmpty()) {
        QString result = out;
        if (!result.endsWith('\n'))
            result += '\n';
        result += err;
        return result;
    }
    return !out.isEmpty() ? out : err;
}

QString SynchronousProcessResponse::stdOut() const
{
    return QtcProcess::normalizeNewlines(codec->toUnicode(rawStdOut));
}

QString SynchronousProcessResponse::stdErr() const
{
    return QtcProcess::normalizeNewlines(codec->toUnicode(rawStdErr));
}

QTCREATOR_UTILS_EXPORT QDebug operator<<(QDebug str, const SynchronousProcessResponse& r)
{
    QDebug nsp = str.nospace();
    nsp << "SynchronousProcessResponse: result=" << r.result << " ex=" << r.exitCode << '\n'
        << r.rawStdOut.size() << " bytes stdout, stderr=" << r.rawStdErr << '\n';
    return str;
}

SynchronousProcessResponse::Result defaultExitCodeInterpreter(int code)
{
    return code ? SynchronousProcessResponse::FinishedError
                : SynchronousProcessResponse::Finished;
}

void ChannelBuffer::clearForRun()
{
    rawDataPos = 0;
    rawData.clear();
    codecState.reset(new QTextCodec::ConverterState);
    incompleteLineBuffer.clear();
}

/* Check for complete lines read from the device and return them, moving the
 * buffer position. */
QString ChannelBuffer::linesRead()
{
    // Convert and append the new input to the buffer of incomplete lines
    const char *start = rawData.constData() + rawDataPos;
    const int len = rawData.size() - rawDataPos;
    incompleteLineBuffer.append(codec->toUnicode(start, len, codecState.get()));
    rawDataPos = rawData.size();

    // Any completed lines in the incompleteLineBuffer?
    const int lastLineIndex = qMax(incompleteLineBuffer.lastIndexOf('\n'),
                                   incompleteLineBuffer.lastIndexOf('\r'));
    if (lastLineIndex == -1)
        return QString();

    // Get completed lines and remove them from the incompleteLinesBuffer:
    const QString lines = QtcProcess::normalizeNewlines(incompleteLineBuffer.left(lastLineIndex + 1));
    incompleteLineBuffer = incompleteLineBuffer.mid(lastLineIndex + 1);

    return lines;
}

void ChannelBuffer::append(const QByteArray &text, bool emitSignals)
{
    if (text.isEmpty())
        return;
    rawData += text;
    if (!emitSignals)
        return;

    // Buffered. Emit complete lines?
    if (outputCallback) {
        const QString lines = linesRead();
        if (!lines.isEmpty())
            outputCallback(lines);
    }
}


// ----------- SynchronousProcess
SynchronousProcess::SynchronousProcess()
{
    d->m_timer.setInterval(1000);
    connect(&d->m_timer, &QTimer::timeout, d, &QtcProcessPrivate::slotTimeout);
    connect(this, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            d, &QtcProcessPrivate::slotFinished);
    connect(this, &QProcess::errorOccurred, d, &QtcProcessPrivate::slotError);
    connect(this, &QProcess::readyReadStandardOutput, d, [this] {
        d->m_hangTimerCount = 0;
        d->m_stdOut.append(readAllStandardOutput(), true);
    });
    connect(this, &QProcess::readyReadStandardError, d, [this] {
        d->m_hangTimerCount = 0;
        d->m_stdErr.append(readAllStandardError(), true);
    });
}

SynchronousProcess::~SynchronousProcess()
{
    disconnect(&d->m_timer, nullptr, this, nullptr);
    disconnect(this, nullptr, this, nullptr);
}

void QtcProcess::setTimeoutS(int timeoutS)
{
    if (timeoutS > 0)
        d->m_maxHangTimerCount = qMax(2, timeoutS);
    else
        d->m_maxHangTimerCount = INT_MAX / 1000;
}

void QtcProcess::setCodec(QTextCodec *c)
{
    QTC_ASSERT(c, return);
    d->m_codec = c;
}

void QtcProcess::setTimeOutMessageBoxEnabled(bool v)
{
    d->m_timeOutMessageBoxEnabled = v;
}

void QtcProcess::setExitCodeInterpreter(const ExitCodeInterpreter &interpreter)
{
    QTC_ASSERT(interpreter, return);
    d->m_exitCodeInterpreter = interpreter;
}

#ifdef QT_GUI_LIB
static bool isGuiThread()
{
    return QThread::currentThread() == QCoreApplication::instance()->thread();
}
#endif

SynchronousProcessResponse QtcProcess::run(const CommandLine &cmd, const QByteArray &writeData)
{
    // FIXME: Implement properly
    if (cmd.executable().needsDevice()) {
        QtcProcess proc;
        proc.setEnvironment(Environment(environment()));
        proc.setWorkingDirectory(workingDirectory());
        proc.setCommand(cmd);

        // writeData ?
        proc.start();

        proc.waitForFinished();

        SynchronousProcessResponse res;
        res.result = SynchronousProcessResponse::Finished;
        res.exitCode = proc.exitCode();
        res.rawStdOut = proc.readAllStandardOutput();
        res.rawStdErr = proc.readAllStandardError();
        return res;
    };

    qCDebug(processLog).noquote() << "Starting:" << cmd.toUserOutput();
    ExecuteOnDestruction logResult([this] {
        qCDebug(processLog) << d->m_result;
    });

    d->clearForRun();

    d->m_binary = cmd.executable();
    // using QProcess::start() and passing program, args and OpenMode results in a different
    // quoting of arguments than using QProcess::setArguments() beforehand and calling start()
    // only with the OpenMode
    setCommand(cmd);
    if (!writeData.isEmpty()) {
        connect(this, &QProcess::started, this, [this, writeData] {
            write(writeData);
            closeWriteChannel();
        });
    }
    setOpenMode(writeData.isEmpty() ? QIODevice::ReadOnly : QIODevice::ReadWrite);
    start();

    // On Windows, start failure is triggered immediately if the
    // executable cannot be found in the path. Do not start the
    // event loop in that case.
    if (!d->m_startFailure) {
        d->m_timer.start();
#ifdef QT_GUI_LIB
        if (isGuiThread())
            QApplication::setOverrideCursor(Qt::WaitCursor);
#endif
        d->m_eventLoop.exec(QEventLoop::ExcludeUserInputEvents);
        d->m_stdOut.append(readAllStandardOutput(), false);
        d->m_stdErr.append(readAllStandardError(), false);

        d->m_result.rawStdOut = d->m_stdOut.rawData;
        d->m_result.rawStdErr = d->m_stdErr.rawData;

        d->m_timer.stop();
#ifdef QT_GUI_LIB
        if (isGuiThread())
            QApplication::restoreOverrideCursor();
#endif
    }

    return  d->m_result;
}

SynchronousProcessResponse QtcProcess::runBlocking(const CommandLine &cmd)
{
    // FIXME: Implement properly
    if (cmd.executable().needsDevice()) {
        QtcProcess proc;
        proc.setEnvironment(Environment(environment()));
        proc.setWorkingDirectory(workingDirectory());
        proc.setCommand(cmd);

        // writeData ?
        proc.start();

        proc.waitForFinished();

        SynchronousProcessResponse res;
        res.result = SynchronousProcessResponse::Finished;
        res.exitCode = proc.exitCode();
        res.rawStdOut = proc.readAllStandardOutput();
        res.rawStdErr = proc.readAllStandardError();
        return res;
    };

    qCDebug(processLog).noquote() << "Starting blocking:" << cmd.toUserOutput();
    ExecuteOnDestruction logResult([this] {
        qCDebug(processLog) << d->m_result;
    });

    d->clearForRun();

    d->m_binary = cmd.executable();
    setOpenMode(QIODevice::ReadOnly);
    setCommand(cmd);
    start();
    if (!waitForStarted(d->m_maxHangTimerCount * 1000)) {
        d->m_result.result = SynchronousProcessResponse::StartFailed;
        return d->m_result;
    }
    closeWriteChannel();
    if (!waitForFinished(d->m_maxHangTimerCount * 1000)) {
        d->m_result.result = SynchronousProcessResponse::Hang;
        terminate();
        if (!waitForFinished(1000)) {
            kill();
            waitForFinished(1000);
        }
    }

    if (state() != QProcess::NotRunning)
        return d->m_result;

    d->m_result.exitCode = exitCode();
    if (d->m_result.result == SynchronousProcessResponse::StartFailed) {
        if (exitStatus() != QProcess::NormalExit)
            d->m_result.result = SynchronousProcessResponse::TerminatedAbnormally;
        else
            d->m_result.result = d->m_exitCodeInterpreter(d->m_result.exitCode);
    }
    d->m_stdOut.append(readAllStandardOutput(), false);
    d->m_stdErr.append(readAllStandardError(), false);

    d->m_result.rawStdOut = d->m_stdOut.rawData;
    d->m_result.rawStdErr = d->m_stdErr.rawData;

    return d->m_result;
}

void QtcProcess::setStdOutCallback(const std::function<void (const QString &)> &callback)
{
    d->m_stdOut.outputCallback = callback;
}

void QtcProcess::setStdErrCallback(const std::function<void (const QString &)> &callback)
{
    d->m_stdErr.outputCallback = callback;
}

void QtcProcessPrivate::slotTimeout()
{
    if (!m_waitingForUser && (++m_hangTimerCount > m_maxHangTimerCount)) {
        if (debug)
            qDebug() << Q_FUNC_INFO << "HANG detected, killing";
        m_waitingForUser = true;
        const bool terminate = !m_timeOutMessageBoxEnabled || askToKill(m_binary.toString());
        m_waitingForUser = false;
        if (terminate) {
            q->stopProcess();
            m_result.result = SynchronousProcessResponse::Hang;
        } else {
            m_hangTimerCount = 0;
        }
    } else {
        if (debug)
            qDebug() << Q_FUNC_INFO << m_hangTimerCount;
    }
}

void QtcProcessPrivate::slotFinished(int exitCode, QProcess::ExitStatus e)
{
    if (debug)
        qDebug() << Q_FUNC_INFO << exitCode << e;
    m_hangTimerCount = 0;

    switch (e) {
    case QProcess::NormalExit:
        m_result.result = m_exitCodeInterpreter(exitCode);
        m_result.exitCode = exitCode;
        break;
    case QProcess::CrashExit:
        // Was hang detected before and killed?
        if (m_result.result != SynchronousProcessResponse::Hang)
            m_result.result = SynchronousProcessResponse::TerminatedAbnormally;
        m_result.exitCode = -1;
        break;
    }
    m_eventLoop.quit();
}

void QtcProcessPrivate::slotError(QProcess::ProcessError e)
{
    m_hangTimerCount = 0;
    if (debug)
        qDebug() << Q_FUNC_INFO << e;
    // Was hang detected before and killed?
    if (m_result.result != SynchronousProcessResponse::Hang)
        m_result.result = SynchronousProcessResponse::StartFailed;
    m_startFailure = true;
    m_eventLoop.quit();
}

} // namespace Utils
