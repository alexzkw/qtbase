/***************************************************************************
**
** Copyright (C) 2011 - 2012 Research In Motion
** Contact: http://www.qt-project.org/
**
** This file is part of the plugins of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** GNU Lesser General Public License Usage
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this
** file. Please review the following information to ensure the GNU Lesser
** General Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU General
** Public License version 3.0 as published by the Free Software Foundation
** and appearing in the file LICENSE.GPL included in the packaging of this
** file. Please review the following information to ensure the GNU General
** Public License version 3.0 requirements will be met:
** http://www.gnu.org/copyleft/gpl.html.
**
** Other Usage
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
**
**
**
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qqnxintegration.h"
#include "qqnxeventthread.h"
#include "qqnxnativeinterface.h"
#include "qqnxrasterbackingstore.h"
#include "qqnxscreen.h"
#include "qqnxscreeneventhandler.h"
#include "qqnxwindow.h"
#include "qqnxnavigatoreventhandler.h"
#include "qqnxabstractnavigator.h"
#include "qqnxabstractvirtualkeyboard.h"
#include "qqnxservices.h"

#if defined(Q_OS_BLACKBERRY)
#include "qqnxbpseventfilter.h"
#include "qqnxnavigatorbps.h"
#elif defined(QQNX_PPS)
#include "qqnxnavigatorpps.h"
#endif

#if defined(QQNX_PPS)
#  include "qqnxnavigatoreventnotifier.h"
#  include "qqnxvirtualkeyboard.h"
#  include "qqnxclipboard.h"

#  if defined(QQNX_IMF)
#    include "qqnxinputcontext_imf.h"
#  else
#    include "qqnxinputcontext_noimf.h"
#  endif
#endif

#include "private/qgenericunixfontdatabase_p.h"

#if defined(Q_OS_BLACKBERRY)
#include "qqnxeventdispatcher_blackberry.h"
#else
#include "private/qgenericunixeventdispatcher_p.h"
#endif

#include <QtGui/QPlatformWindow>
#include <QtGui/QWindowSystemInterface>

#if !defined(QT_NO_OPENGL)
#include "qqnxglbackingstore.h"
#include "qqnxglcontext.h"

#include <QtGui/QOpenGLContext>
#endif

#include <QtCore/QDebug>
#include <QtCore/QHash>

#include <errno.h>

QT_BEGIN_NAMESPACE

QQnxWindowMapper QQnxIntegration::ms_windowMapper;
QMutex QQnxIntegration::ms_windowMapperMutex;

QQnxIntegration::QQnxIntegration()
    : QPlatformIntegration()
    , m_eventThread(0)
    , m_navigatorEventHandler(new QQnxNavigatorEventHandler())
    , m_virtualKeyboard(0)
#if defined(QQNX_PPS)
    , m_navigatorEventNotifier(0)
    , m_inputContext(0)
#endif
    , m_services(0)
    , m_fontDatabase(new QGenericUnixFontDatabase())
#if !defined(QT_NO_OPENGL)
    , m_paintUsingOpenGL(false)
#endif
#if defined(Q_OS_BLACKBERRY)
    , m_eventDispatcher(new QQnxEventDispatcherBlackberry())
    , m_bpsEventFilter(0)
#else
    , m_eventDispatcher(createUnixEventDispatcher())
#endif
    , m_nativeInterface(new QQnxNativeInterface())
    , m_screenEventHandler(new QQnxScreenEventHandler())
#if !defined(QT_NO_CLIPBOARD)
    , m_clipboard(0)
#endif
{
#if defined(QQNXINTEGRATION_DEBUG)
    qDebug() << Q_FUNC_INFO;
#endif
    // Open connection to QNX composition manager
    errno = 0;
    int result = screen_create_context(&m_screenContext, SCREEN_APPLICATION_CONTEXT);
    if (result != 0) {
        qFatal("QQnx: failed to connect to composition manager, errno=%d", errno);
    }

#if defined(QQNX_PPS)
    // Create/start navigator event notifier
    m_navigatorEventNotifier = new QQnxNavigatorEventNotifier(m_navigatorEventHandler);

    // delay invocation of start() to the time the event loop is up and running
    // needed to have the QThread internals of the main thread properly initialized
    QMetaObject::invokeMethod(m_navigatorEventNotifier, "start", Qt::QueuedConnection);
#endif

    // Create displays for all possible screens (which may not be attached)
    createDisplays();

#if !defined(QT_NO_OPENGL)
    // Initialize global OpenGL resources
    QQnxGLContext::initialize();
#endif

    // Create/start event thread
    m_eventThread = new QQnxEventThread(m_screenContext, m_screenEventHandler);
    m_eventThread->start();

#if defined(QQNX_PPS)
    // Create/start the keyboard class.
    m_virtualKeyboard = new QQnxVirtualKeyboard();

    // delay invocation of start() to the time the event loop is up and running
    // needed to have the QThread internals of the main thread properly initialized
    QMetaObject::invokeMethod(m_virtualKeyboard, "start", Qt::QueuedConnection);

    // TODO check if we need to do this for all screens or only the primary one
    QObject::connect(m_virtualKeyboard, SIGNAL(heightChanged(int)),
                     primaryDisplay(), SLOT(keyboardHeightChanged(int)));

    // Set up the input context
    m_inputContext = new QQnxInputContext(*m_virtualKeyboard);
#endif

#if defined(Q_OS_BLACKBERRY)
    m_navigator = new QQnxNavigatorBps();
#elif defined(QQNX_PPS)
    m_navigator = new QQnxNavigatorPps();
#endif

    // Create services handling class
    if (m_navigator)
        m_services = new QQnxServices(m_navigator);

#if defined(Q_OS_BLACKBERRY)
    m_bpsEventFilter = new QQnxBpsEventFilter;
    m_bpsEventFilter->installOnEventDispatcher(m_eventDispatcher);
#endif

}

QQnxIntegration::~QQnxIntegration()
{
#if defined(QQNXINTEGRATION_DEBUG)
    qDebug() << "QQnx: platform plugin shutdown begin";
#endif


    delete m_nativeInterface;

#if defined(QQNX_PPS)
    // Destroy input context
    delete m_inputContext;
#endif

    // Destroy the keyboard class.
    delete m_virtualKeyboard;

#if !defined(QT_NO_CLIPBOARD)
    // Delete the clipboard
    delete m_clipboard;
#endif

    // Stop/destroy navigator event notifier
#if defined(QQNX_PPS)
    delete m_navigatorEventNotifier;
#endif
    delete m_navigatorEventHandler;

    // Stop/destroy event thread
    delete m_eventThread;
    delete m_screenEventHandler;

    // Destroy all displays
    destroyDisplays();

    // Close connection to QNX composition manager
    screen_destroy_context(m_screenContext);

#if !defined(QT_NO_OPENGL)
    // Cleanup global OpenGL resources
    QQnxGLContext::shutdown();
#endif

    // Destroy services class
    delete m_services;

    // Destroy navigator interface
    delete m_navigator;

#if defined(Q_OS_BLACKBERRY)
    delete m_bpsEventFilter;
#endif

#if defined(QQNXINTEGRATION_DEBUG)
    qDebug() << "QQnx: platform plugin shutdown end";
#endif
}

bool QQnxIntegration::hasCapability(QPlatformIntegration::Capability cap) const
{
#if defined(QQNXINTEGRATION_DEBUG)
    qDebug() << Q_FUNC_INFO;
#endif
    switch (cap) {
    case ThreadedPixmaps: return true;
#if defined(QT_OPENGL_ES)
    case OpenGL:
        return true;
#endif
    default: return QPlatformIntegration::hasCapability(cap);
    }
}

QPlatformWindow *QQnxIntegration::createPlatformWindow(QWindow *window) const
{
#if defined(QQNXINTEGRATION_DEBUG)
    qDebug() << Q_FUNC_INFO;
#endif
    return new QQnxWindow(window, m_screenContext);
}

QPlatformBackingStore *QQnxIntegration::createPlatformBackingStore(QWindow *window) const
{
#if defined(QQNXINTEGRATION_DEBUG)
    qDebug() << Q_FUNC_INFO;
#endif
#if !defined(QT_NO_OPENGL)
    if (paintUsingOpenGL())
        return new QQnxGLBackingStore(window);
    else
#endif
        return new QQnxRasterBackingStore(window);
}

#if !defined(QT_NO_OPENGL)
QPlatformOpenGLContext *QQnxIntegration::createPlatformOpenGLContext(QOpenGLContext *context) const
{
#if defined(QQNXINTEGRATION_DEBUG)
    qDebug() << Q_FUNC_INFO;
#endif
    return new QQnxGLContext(context);
}
#endif

#if defined(QQNX_PPS)
QPlatformInputContext *QQnxIntegration::inputContext() const
{
#if defined(QQNXINTEGRATION_DEBUG)
    qDebug() << Q_FUNC_INFO;
#endif
    return m_inputContext;
}
#endif

void QQnxIntegration::moveToScreen(QWindow *window, int screen)
{
#if defined(QQNXINTEGRATION_DEBUG)
    qDebug() << "QQnxIntegration::moveToScreen - w=" << window << ", s=" << screen;
#endif

    // get platform window used by widget
    QQnxWindow *platformWindow = static_cast<QQnxWindow *>(window->handle());

    // lookup platform screen by index
    QQnxScreen *platformScreen = m_screens.at(screen);

    // move the platform window to the platform screen
    platformWindow->setScreen(platformScreen);
}

QAbstractEventDispatcher *QQnxIntegration::guiThreadEventDispatcher() const
{
#if defined(QQNXINTEGRATION_DEBUG)
    qDebug() << Q_FUNC_INFO;
#endif
    return m_eventDispatcher;
}

QPlatformNativeInterface *QQnxIntegration::nativeInterface() const
{
    return m_nativeInterface;
}

#if !defined(QT_NO_CLIPBOARD)
QPlatformClipboard *QQnxIntegration::clipboard() const
{
#if defined(QQNXINTEGRATION_DEBUG)
    qDebug() << Q_FUNC_INFO;
#endif

#if defined(QQNX_PPS)
    if (!m_clipboard) {
        m_clipboard = new QQnxClipboard;
    }
#endif
    return m_clipboard;
}
#endif

QVariant QQnxIntegration::styleHint(QPlatformIntegration::StyleHint hint) const
{
#if defined(QQNXINTEGRATION_DEBUG)
    qDebug() << Q_FUNC_INFO;
#endif
    if (hint == ShowIsFullScreen)
        return true;

    return QPlatformIntegration::styleHint(hint);
}

QPlatformServices * QQnxIntegration::services() const
{
    return m_services;
}

QWindow *QQnxIntegration::window(screen_window_t qnxWindow)
{
#if defined(QQNXINTEGRATION_DEBUG)
    qDebug() << Q_FUNC_INFO;
#endif
    QMutexLocker locker(&ms_windowMapperMutex);
    Q_UNUSED(locker);
    return ms_windowMapper.value(qnxWindow, 0);
}

void QQnxIntegration::addWindow(screen_window_t qnxWindow, QWindow *window)
{
#if defined(QQNXINTEGRATION_DEBUG)
    qDebug() << Q_FUNC_INFO;
#endif
    QMutexLocker locker(&ms_windowMapperMutex);
    Q_UNUSED(locker);
    ms_windowMapper.insert(qnxWindow, window);
}

void QQnxIntegration::removeWindow(screen_window_t qnxWindow)
{
#if defined(QQNXINTEGRATION_DEBUG)
    qDebug() << Q_FUNC_INFO;
#endif
    QMutexLocker locker(&ms_windowMapperMutex);
    Q_UNUSED(locker);
    ms_windowMapper.remove(qnxWindow);
}

void QQnxIntegration::createDisplays()
{
#if defined(QQNXINTEGRATION_DEBUG)
    qDebug() << Q_FUNC_INFO;
#endif
    // Query number of displays
    errno = 0;
    int displayCount;
    int result = screen_get_context_property_iv(m_screenContext, SCREEN_PROPERTY_DISPLAY_COUNT, &displayCount);
    if (result != 0) {
        qFatal("QQnxIntegration: failed to query display count, errno=%d", errno);
    }

    // Get all displays
    errno = 0;
    screen_display_t *displays = (screen_display_t *)alloca(sizeof(screen_display_t) * displayCount);
    result = screen_get_context_property_pv(m_screenContext, SCREEN_PROPERTY_DISPLAYS, (void **)displays);
    if (result != 0) {
        qFatal("QQnxIntegration: failed to query displays, errno=%d", errno);
    }

    for (int i=0; i<displayCount; i++) {
#if defined(QQNXINTEGRATION_DEBUG)
        qDebug() << "QQnxIntegration::Creating screen for display " << i;
#endif
        QQnxScreen *screen = new QQnxScreen(m_screenContext, displays[i], i==0);
        m_screens.append(screen);
        screenAdded(screen);

        QObject::connect(m_screenEventHandler, SIGNAL(newWindowCreated(void *)),
                         screen, SLOT(newWindowCreated(void *)));
        QObject::connect(m_screenEventHandler, SIGNAL(windowClosed(void *)),
                         screen, SLOT(windowClosed(void *)));

        QObject::connect(m_navigatorEventHandler, SIGNAL(rotationChanged(int)), screen, SLOT(setRotation(int)));
    }
}

void QQnxIntegration::destroyDisplays()
{
#if defined(QQNXINTEGRATION_DEBUG)
    qDebug() << Q_FUNC_INFO;
#endif
    qDeleteAll(m_screens);
    m_screens.clear();
}

QQnxScreen *QQnxIntegration::primaryDisplay() const
{
    return m_screens.first();
}

QT_END_NAMESPACE
