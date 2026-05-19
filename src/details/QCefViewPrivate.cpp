#include "QCefViewPrivate.h"

#pragma region stl_headers
#include <stdexcept>
#pragma endregion

#pragma region qt_headers
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QDrag>
#include <QFileDialog>
#include <QGridLayout>
#include <QInputMethodQueryEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaMethod>
#include <QPainter>
#include <QRegion>
#include <QStyleOption>
#include <QWindow>
#pragma endregion

#pragma region cef_headers
#include <include/cef_app.h>
#include <include/cef_browser.h>
#include <include/cef_frame.h>
#include <include/cef_parser.h>
#pragma endregion

#include <CefViewCoreProtocol.h>

#include "CCefClientDelegate.h"
#include "QCefContext.h"
#include "QCefSettingPrivate.h"
#include "utils/CommonUtils.h"
#include "utils/DragAndDropUtils.h"
#include "utils/KeyboardUtils.h"
#include "utils/MenuBuilder.h"
#include "utils/ValueConvertor.h"

QSet<QCefViewPrivate*> QCefViewPrivate::sLiveInstances;

#ifdef Q_OS_WIN
#include <Windows.h>

HICON
QPixmapToHICON(const QPixmap &pixmap)
{
  if (pixmap.isNull()) {
    return HICON();
  }

  // 获取像素数据和尺寸
  QImage image = pixmap.toImage();
  int width = image.width();
  int height = image.height();

  // HBITMAP
  // 转换为 Windows 位图格式（通常是 32 位 ARGB）
  // 注意：Windows GDI 使用 BGR 顺序，而 Qt 可能使用 RGB，需进行通道转换
  uint32_t *pixels = (uint32_t*)image.bits();
  // 创建 Windows HBITMAP
  BITMAPINFO bmiBitmap = { 0 };
  bmiBitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmiBitmap.bmiHeader.biWidth = width;
  bmiBitmap.bmiHeader.biHeight = -height; // 负数表示 top-down 位图
  bmiBitmap.bmiHeader.biPlanes = 1;
  bmiBitmap.bmiHeader.biBitCount = 32;
  bmiBitmap.bmiHeader.biCompression = BI_RGB;
  void *bitmapData = nullptr;
  HBITMAP hBitmap = CreateDIBSection(NULL, &bmiBitmap, DIB_RGB_COLORS, &bitmapData, NULL, 0);
  // 复制像素数据到 HBITMAP
  memcpy(bitmapData, pixels, width * height * 4); // 32 位 = 4 字节/像素

  // HBITMAP
  // 创建单色掩码位图（1位/像素）
  BITMAPINFO bmiMask = { 0 };
  bmiMask.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmiMask.bmiHeader.biWidth = width;
  bmiMask.bmiHeader.biHeight = height; // 正数表示 bottom-up 位图
  bmiMask.bmiHeader.biPlanes = 1;
  bmiMask.bmiHeader.biBitCount = 1;
  bmiMask.bmiHeader.biCompression = BI_RGB;
  void *maskData = nullptr;
  HBITMAP hMask = CreateDIBSection(NULL, &bmiMask, DIB_RGB_COLORS, &maskData, NULL, 0);
  // 初始化所有位为0（全透明）
  memset(maskData, 0, ((width + 31) / 32) * 4 /* 计算每行字节数（四字节对齐） */ * height);
  // 根据 Alpha 通道生成掩码
  // 注意：Windows 位图是 bottom-up 存储，而 Qt 图像可能是 top-down
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      QRgb pixel = image.pixel(x, y);
      uchar alpha = qAlpha(pixel);
      // Alpha 值大于等于128的像素设为不透明（白色）
      // 计算字节索引（每行四字节对齐）
      int bytesPerLine = ((x + 31) / 32) * 4; // 向上取整到最近的四字节
      int byteIndex = y * bytesPerLine + x / 8;
      int bitIndex = x % 8;
      // 获取字节指针并修改特定位
      unsigned char* bytePtr = static_cast<unsigned char*>(maskData) + byteIndex;
      if (alpha >= 128) {
        *bytePtr |= (1 << (7 - bitIndex)); // 设置位为1
      } else {
        *bytePtr &= ~(1 << (7 - bitIndex)); // 设置位为0
      }
    }
  }

  // HICON
  ICONINFO iconInfo = { 0 };
  iconInfo.fIcon = TRUE;       // 表示这是一个图标
  iconInfo.hbmColor = hBitmap; // 主位图（颜色）
  iconInfo.hbmMask = hMask;    // 掩码位图（透明度）

  // 创建 HICON
  HICON hIcon = CreateIconIndirect(&iconInfo);

  // 释放临时位图
  if (hBitmap) {
    DeleteObject(hBitmap);
  }
  if (hMask) {
    DeleteObject(hMask);
  }

  return hIcon;
}
#elif defined(Q_OS_MAC)
#else
#endif

void
QCefViewPrivate::destroyAllInstance()
{
  auto s = sLiveInstances;
  for (auto& i : s) {
    i->destroyCefBrowser();
  }
}

QCefViewPrivate::QCefViewPrivate(QCefView* view, QCefContextPrivate* ctx, const QCefSettingPrivate* setting)
  : q_ptr(view)
  , pContextPrivate_(ctx)
{
  sLiveInstances.insert(this);

  // get the global windowless rendering switch
  isOSRModeEnabled_ = pContextPrivate_->cefConfig()->windowlessRenderingEnabled().toBool();

  // if windowless rendering is enabled globally, then
  // get the windowless rendering switch from the per view setting
  if (isOSRModeEnabled_ && setting) {
    isOSRModeEnabled_ = setting->offScreenRendering_;
  }
}

QCefViewPrivate::~QCefViewPrivate()
{
  qDebug() << this << "is being destructed";

  sLiveInstances.remove(this);
}

void
QCefViewPrivate::createCefBrowser(const QString& url, const QCefSettingPrivate* setting)
{
  Q_Q(QCefView);
  if (!q) {
    return;
  }

  // 1. create browser client handler delegate
  auto pClientDelegate = std::make_shared<CCefClientDelegate>(sharedFromThis());

  // 2. create browser client handler
  auto pClient = new CefViewBrowserClient(pContextPrivate_->getCefApp(), pClientDelegate);

  // 3. create the browser settings
  CefBrowserSettings browserSettings;
  QCefSettingPrivate::CopyToCefBrowserSettings(setting, &browserSettings);

  // 4. set window info
  CefWindowInfo windowInfo;
  if (isOSRModeEnabled_) {
    qDebug() << __FUNCTION__ << "OSR mode";
    auto winSize = q->size();
    // set QWidget background
    QColor background = q->palette().color(q->backgroundRole());
    CefViewRendererPtr renderer;
    // if hardware is enabled
    if (setting && setting->hardwareAcceleration_) {
      qDebug() << __FUNCTION__ << "Use hardware renderer";
#if CEF_VERSION_MAJOR >= 125
      // create hardware renderer if enabled
      if ((renderer = CefViewRendererFactory::createRenderer(true))) {
        // initialize renderer
        if (renderer->initialize(q,                //
                                 winSize.width(),  //
                                 winSize.height(), //
                                 scaleFactor(),    //
                                 background.rgba() //
                                 )) {
          // OSR with hardware renderer
          osr.pRenderer_ = renderer;
          windowInfo.SetAsWindowless(0);
          windowInfo.shared_texture_enabled = true;
        } else {
          qWarning() << "Failed to initialize hardware renderer, will fall back to software renderer";
        }
      } else {
        qWarning() << "Failed to create hardware renderer, will fall back to software renderer";
      }
    } else {
      qInfo() << "Hardware Acceleration is disabled";
#else
      qWarning() << "Hardware Acceleration is supported only on CEF version 125+";
#endif
    }

    // if hardware renderer is not enabled or failed to create it
    // fall back to software renderer
    if (!osr.pRenderer_) {
      qDebug() << __FUNCTION__ << "Use software renderer";
      // create software renderer
      if ((renderer = CefViewRendererFactory::createRenderer(false))) {
        // initialize renderer
        if (renderer->initialize(q,                //
                                 winSize.width(),  //
                                 winSize.height(), //
                                 scaleFactor(),    //
                                 background.rgba() //
                                 )) {
          // OSR with software renderer
          osr.pRenderer_ = renderer;
          windowInfo.SetAsWindowless(0);
          windowInfo.shared_texture_enabled = false;
        } else {
          qWarning() << "Failed to initialize software renderer";
        }
      } else {
        qWarning() << "Failed to create software renderer";
      }
    }

    Q_ASSERT_X(                            //
      osr.pRenderer_,                      //
      "QCefViewPrivate::createCefBrowser", //
      "Failed to create osr renderer"      //
    );
    if (!osr.pRenderer_) {
      // all renderer were failed to create or initialize
      qCritical() << "QCefViewPrivate::createCefBrowser: Failed to create osr renderer.";
    }
  } else {
#if CEF_VERSION_MAJOR >= 125
    // https://github.com/chromiumembedded/cef/issues/3685
    windowInfo.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
#endif
    // create CEF browser parent window
    auto windowInitialSize = q->size();
    if (setting) {
      windowInitialSize = setting->windowInitialSize_;
    }
    qDebug() << "Browser init size:" << windowInitialSize;

    ncw.qBrowserWindow_ = new QCefWindow();
    if (ncw.qBrowserWindow_) {
      ncw.qBrowserWindow_->resize(windowInitialSize);
      ncw.qBrowserWindow_->setFlags(Qt::Window | Qt::FramelessWindowHint);

      auto width = windowInitialSize.width() * scaleFactor();
      auto height = windowInitialSize.height() * scaleFactor();
#if CEF_VERSION_MAJOR > 85
      windowInfo.SetAsChild((CefWindowHandle)ncw.qBrowserWindow_->winId(), { 0, 0, (int)width, (int)height });
#else
      windowInfo.SetAsChild((CefWindowHandle)ncw.qBrowserWindow_->winId(), 0, 0, (int)width, (int)height);
#endif
    }
  }

  // 5. create browser object
  bool success = CefBrowserHost::CreateBrowser(windowInfo,        // window info
                                               pClient,           // handler
                                               url.toStdString(), // url
                                               browserSettings,   // settings
                                               nullptr,
                                               CefRequestContext::GetGlobalContext());
  Q_ASSERT_X(                         //
    success,                          //
    "QCefViewPrivate::createBrowser", //
    "Failed to create cef browser"    //
  );
  if (!success) {
    qCritical() << "QCefViewPrivate::createBrowser: Failed to create cef browser";
    return;
  }

  isMainFrameLoaded_ = true;

  // install global event filter
  qApp->installEventFilter(this);

  pClient_ = pClient;
  pClientDelegate_ = pClientDelegate;
}

void
QCefViewPrivate::destroyCefBrowser()
{
  if (!pClient_)
    return;

  qDebug() << "destroy browser from native";

  clearJSDialogMap();

  if (!isOSRModeEnabled_) {
    // remove from parent, prevent from being destroyed
    if (ncw.qBrowserWidget_) {
      ncw.qBrowserWidget_->setParent(nullptr);
    }
    if (ncw.qBrowserWidget_) {
      ncw.qBrowserWidget_->deleteLater();
    }
    if (ncw.qBrowserWindow_) {
      ncw.qBrowserWindow_->detachCefWindow();
    }
  }

  // clean all browsers
  if (pClient_) {
    pClient_->CloseAllBrowsers();
    pClient_ = nullptr;
  }
  pCefBrowser_ = nullptr;

  qApp->removeEventFilter(this);
}

void
QCefViewPrivate::clearJSDialogMap()
{
  if (pClientDelegate_) {
    pClientDelegate_->clearJSDialogMap();
  }
}

void
QCefViewPrivate::addLocalFolderResource(const QString& path, const QString& url, int priority /*= 0*/)
{
  if (pClient_) {
    pClient_->AddLocalDirectoryResourceProvider(path.toStdString(), url.toStdString(), priority);
  }
}

void
QCefViewPrivate::addArchiveResource(const QString& path,
                                    const QString& url,
                                    const QString& password /*= ""*/,
                                    int priority /*= 0*/)
{
  if (pClient_) {
    pClient_->AddArchiveResourceProvider(path.toStdString(), url.toStdString(), password.toStdString(), priority);
  }
}

void
QCefViewPrivate::setCefWindowFocus(bool focus)
{
  if (pCefBrowser_) {
    CefRefPtr<CefBrowserHost> host = pCefBrowser_->GetHost();
    if (host) {
      host->SetFocus(focus);
    }
  }
}

QCefQuery
QCefViewPrivate::createQuery(const QString& req, const int64_t id)
{
  return QCefQuery(this, req, id);
}

void
QCefViewPrivate::onCefBrowserCreated(CefRefPtr<CefBrowser> browser, QWindow* window)
{
  Q_Q(QCefView);
  if (!q) {
    return;
  }

  // capture the browser
  pCefBrowser_ = browser;

  // if last url is not empty, need to navigate to it
  if (!lastUrl_.empty()) {
    browser->GetMainFrame()->LoadURL(lastUrl_);
  }

  // monitor the screenChanged signal from the top-level window
  if (q->window()->windowHandle()) {
    connect(q->window()->windowHandle(),         //
            SIGNAL(screenChanged(QScreen*)),     //
            this,                                //
            SLOT(onViewScreenChanged(QScreen*)), //
            Qt::UniqueConnection                 //
    );
  }

  if (isOSRModeEnabled_) {
    // OSR mode
    // notify the visibility and size
    pCefBrowser_->GetHost()->WasHidden(!q->isVisible());
    pCefBrowser_->GetHost()->WasResized();
  } else {
    // emit signal
    emit q->nativeBrowserCreated(window);

    qDebug() << "CEF Window Native ID:" << window->winId();

    // create widget for cef window
    ncw.qBrowserWidget_ = ncw.qBrowserWindow_->attachCefWindow(window, q);
    Q_ASSERT_X(                                                //
      ncw.qBrowserWidget_,                                     //
      "QCefViewPrivate::onCefBrowserCreated",                  //
      "Failed to create alien QWidget from cef browser window" //
    );
    if (!ncw.qBrowserWidget_) {
      qCritical() << "QCefViewPrivate::onCefBrowserCreated: Failed to create QWidget from cef browser window";
      browser->GetHost()->CloseBrowser(true);
      return;
    }

#ifdef Q_OS_WIN
    connect(ncw.qBrowserWindow_, &QCefWindow::sigResizeCefWindow, window, [this, window](qint32 width, qint32 height) {
      QPointer<QWindow> windowPointer = window;
      if (windowPointer) {
        windowPointer->setProperty("newWidth", width);
        windowPointer->setProperty("newHeight", height);
        QTimer::singleShot(100, windowPointer, [this, windowPointer]() {
          if (windowPointer && isMainFrameLoaded_) {
            // resize window
            qint32 width = windowPointer->property("newWidth").toInt();
            qint32 height = windowPointer->property("newHeight").toInt();
            ::SetWindowPos((HWND)(windowPointer->winId()),
                           NULL,
                           0,
                           0,
                           width,
                           height,
                           SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSENDCHANGING | SWP_DEFERERASE);
          }
        });
      }
    });
#endif // Q_OS_WIN

    // adjust size/mask and attach to cef window
    ncw.qBrowserWindow_->applyMask(q->mask());

    // resize to eliminate flicker
    qDebug() << "Host QCefView size:" << q->size();
    ncw.qBrowserWidget_->resize(q->size());

    // initialize the layout and add browser widget to the layout
    QGridLayout* layout = new QGridLayout(q);
    layout->setContentsMargins(0, 0, 0, 0);
    if (ncw.qBrowserWidget_) {
      layout->addWidget(ncw.qBrowserWidget_);
    }
  }

  emit q->sigCefBrowserCreated();
}

void
QCefViewPrivate::onBeforeNewBrowserCreate(const QCefFrameId& sourceFrameId,
                                          const QString& targetUrl,
                                          const QString& targetFrameName,
                                          QCefView::CefWindowOpenDisposition targetDisposition,
                                          QRect rect,
                                          QCefSetting settings)
{
  Q_Q(QCefView);
  if (!q) {
    return;
  }

  // this is a fake pop-up browser, we just cancel it and then
  // we create a new QCefView instance to replace the fake pop-up browser
  q->onNewBrowser(sourceFrameId,     //
                  targetUrl,         //
                  targetFrameName,   //
                  targetDisposition, //
                  rect,              //
                  settings);
}

bool
QCefViewPrivate::onBeforeNewPopupCreate(const QCefFrameId& sourceFrameId,
                                        const QString& targetUrl,
                                        QString& targetFrameName,
                                        QCefView::CefWindowOpenDisposition targetDisposition,
                                        QRect& rect,
                                        QCefSetting& settings,
                                        bool& disableJavascriptAccess)
{
  Q_Q(QCefView);
  if (!q) {
    return true;
  }

  return q->onNewPopup(sourceFrameId,     //
                       targetUrl,         //
                       targetFrameName,   //
                       targetDisposition, //
                       rect,              //
                       settings,          //
                       disableJavascriptAccess);
}

void
QCefViewPrivate::onAfterCefPopupCreated(CefRefPtr<CefBrowser> browser)
{
  Q_Q(QCefView);

  if (browser && browser->GetHost()) {
    QIcon restoredIcon = QIcon();
    // 尝试从自定义数据中获取把窗口图标
    QVariant iconProperty = q->property("newPopupIcon");
    if (iconProperty.canConvert<QIcon>()) {
      restoredIcon = iconProperty.value<QIcon>();
    }
    if (restoredIcon.isNull()) {
      // 尝试获取主界面的窗口图标
      iconProperty = q_ptr->windowIcon();
      if (iconProperty.canConvert<QIcon>()) {
        restoredIcon = iconProperty.value<QIcon>();
      }
    }
    if (!restoredIcon.isNull()) {
      QList<QSize> qSizeList = restoredIcon.availableSizes();
      if (qSizeList.size() > 0) {
        QPixmap qPixmap = restoredIcon.pixmap(qSizeList[0]);
        if (!qPixmap.isNull()) {
#ifdef Q_OS_WIN
          HWND browserHWND = static_cast<HWND>(browser->GetHost()->GetWindowHandle());
          if (browserHWND) {
            // 转换成 HICON
            HICON hIcon = QPixmapToHICON(qPixmap);
            if (hIcon) {
              // 设置小图标（窗口标题栏）
              SendMessage(browserHWND, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
              // 手动释放
              DestroyIcon(hIcon);
            }
          }
#elif defined(Q_OS_MAC)
#endif
        }
      }
    }
  }
}

void
QCefViewPrivate::onNewDownloadItem(QSharedPointer<QCefDownloadItem> item, const QString& suggestedName)
{
  Q_Q(QCefView);
  if (!q) {
    return;
  }

  q->onNewDownloadItem(item, suggestedName);
}

void
QCefViewPrivate::onUpdateDownloadItem(QSharedPointer<QCefDownloadItem> item)
{
  Q_Q(QCefView);
  if (!q) {
    return;
  }

  q->onUpdateDownloadItem(item);
}

bool
QCefViewPrivate::handleLoadError(CefRefPtr<CefBrowser>& browser,
                                 CefRefPtr<CefFrame>& frame,
                                 int errorCode,
                                 const std::string& errorMsg,
                                 const std::string& failedUrl)
{
  Q_Q(QCefView);
  if (!q) {
    return false;
  }

  // If the signal was connected then emit the signal and set handled with true to skip the default handler
  if (q->isSignalConnected(QMetaMethod::fromSignal(&QCefView::loadError))) {
    auto msg = QString::fromStdString(errorMsg);
    auto url = QString::fromStdString(failedUrl);
    emit q->loadError(browser->GetIdentifier(),
                      ValueConvertor::FrameIdC2Q(frame->GetIdentifier()),
                      frame->IsMain(),
                      errorCode,
                      msg,
                      url);
    return true;
  }

  return false;
}

bool
QCefViewPrivate::requestCloseFromWeb(CefRefPtr<CefBrowser>& browser)
{
  Q_Q(QCefView);
  if (!q) {
    return true;
  }

  bool allowClose = q->onRequestCloseFromWeb();
  if (allowClose) {
    // need close
    destroyCefBrowser();
  }

  return allowClose;
}

qreal
QCefViewPrivate::scaleFactor()
{
  Q_Q(QCefView);
  if (!q) {
    return 0;
  }

#if (QT_VERSION >= QT_VERSION_CHECK(5, 6, 0))
  qreal scaleFactor = q->devicePixelRatioF();
#else
  qreal scaleFactor = q->devicePixelRatio();
#endif
  return scaleFactor;
}

void
QCefViewPrivate::onViewScreenChanged(QScreen* screen)
{
  Q_Q(QCefView);
  if (!q) {
    return;
  }

  // no matter what reason, we need to update the geometry
  // because some system/Qt version will not notify the new
  // size information correctly when screen change
  q->updateGeometry();

  if (isOSRModeEnabled_) {
    // OSR mode
    if (pCefBrowser_) {
      pCefBrowser_->GetHost()->WasResized();
      pCefBrowser_->GetHost()->NotifyScreenInfoChanged();
    }
  } else {
    if (ncw.qBrowserWindow_)
      ncw.qBrowserWindow_->applyMask(q->mask());
  }
}

void
QCefViewPrivate::onCefWindowLostTabFocus(bool next)
{
  // The focus was returned from CEF window, QCefView needs to handle
  // this event and give the focus to the correct next or previous widget
  Q_Q(QCefView);
  if (!q) {
    return;
  }

  auto reason = next ? Qt::TabFocusReason : Qt::BacktabFocusReason;
  auto widget = next ? q->nextInFocusChain() : q->previousInFocusChain();

  // find correct widget
  while (widget && 0 == (widget->focusPolicy() & Qt::TabFocus)) {
    widget = next ? widget->nextInFocusChain() : widget->previousInFocusChain();
  }

  if (widget) {
    // TO-DO bug: this does not work on Linux(X11), need to find a workaround
    widget->window()->raise();
    widget->setFocus(reason);
    widget->activateWindow();
  }

  // update cef focus status
  if (isOSRModeEnabled_) {
    osr.hasCefGotFocus_ = false;
  }
}

void
QCefViewPrivate::onCefWindowGotFocus()
{
  Q_Q(QCefView);
  if (!q) {
    return;
  }

  //qDebug() << "----- " << this << "::onCefWindowGotFocus()";-----

  if (isOSRModeEnabled_) {
    // update CEF focus status
    osr.hasCefGotFocus_ = true;

    // OSR mode, sync focus status
    if (!q->hasFocus()) {
      q->setFocus();
    }
  } else {
    // NCW mode, clear previous focus
    QWidget* focusWidget = qApp->focusWidget();
    if (focusWidget && focusWidget != q) {
      focusWidget->clearFocus();
    }
  }
}

void
QCefViewPrivate::onCefUpdateCursor(const QCursor& cursor)
{
  Q_Q(QCefView);
  if (!q) {
    return;
  }

  if (isOSRModeEnabled_) {
    // OSR mode
    q->setCursor(cursor);
  }
}

void
QCefViewPrivate::onCefInputStateChanged(bool editable)
{
  Q_Q(QCefView);
  if (!q) {
    return;
  }

  q->setAttribute(Qt::WA_InputMethodEnabled, editable);
}

void
QCefViewPrivate::onOsrImeCursorRectChanged(const QRect& rc)
{
  osr.imeCursorRect_ = rc;
  auto inputMethod = QGuiApplication::inputMethod();
  if (inputMethod) {
    inputMethod->update(Qt::ImCursorRectangle);
  }
}

void
QCefViewPrivate::onContextMenuTriggered(QAction* action)
{
  FLog();

  if (action && osr.contextMenuCallback_) {
    // TO-DO (sheen) retrieve correct event flags
    auto eventFlags = EVENTFLAG_NONE;
    auto commandId = action->data().toInt();
    osr.contextMenuCallback_->Continue(commandId, eventFlags);
    osr.contextMenuCallback_ = nullptr;
  }
}

void
QCefViewPrivate::onContextMenuDestroyed(QObject* obj)
{
  FLog();

  if (osr.contextMenu_) {
    osr.contextMenu_ = nullptr;
  }

  if (osr.contextMenuCallback_) {
    osr.contextMenuCallback_->Cancel();
    osr.contextMenuCallback_ = nullptr;
  }

  osr.isShowingContextMenu_ = false;
}

void
QCefViewPrivate::onStartDragging(CefRefPtr<CefDragData>& dragData, CefRenderHandler::DragOperationsMask allowedOps)
{
  Q_Q(QCefView);
  if (!q) {
    return;
  }

  Qt::DropAction dropAction = Qt::IgnoreAction;

  // create drag
  if (QMimeData* mimeData = CreateQMimeDataFromCefDragData(*dragData)) {
    QDrag drag(q);
    drag.setMimeData(mimeData);

    CefRefPtr<CefImage> image = dragData->GetImage();
    if (image) {
      int w = 0;
      int h = 0;
      if (auto pngData = image->GetAsPNG(1.0, true, w, h)) {
        QPixmap pixmap;
#if CEF_VERSION_MAJOR >= 119
        pixmap.loadFromData((const uchar*)(pngData->GetRawData()), (int)(pngData->GetSize()));
#else
        std::vector<uchar> buffer(pngData->GetSize());
        pngData->GetData(buffer.data(), buffer.size(), 0);
        pixmap.loadFromData(buffer.data(), static_cast<uint>(buffer.size()));
#endif
        pixmap.setDevicePixelRatio(scaleFactor());
        drag.setPixmap(pixmap);
      }

      CefPoint hotspot = dragData->GetImageHotspot();
      drag.setHotSpot(QPoint(hotspot.x, hotspot.y));
    }

    // execute dragging
    osr.allowedDragOperations_ = MapCefDragOperationToQDropAction(allowedOps);
    dropAction = drag.exec(static_cast<Qt::DropActions>(osr.allowedDragOperations_), Qt::CopyAction);
  }

  // notify cef that the drag ends
  if (pCefBrowser_) {
    CefRefPtr<CefBrowserHost> host = pCefBrowser_->GetHost();
    if (host) {
      auto result = MapQDropActionToCefDragOperation(dropAction);
      auto cursor = q->mapFromGlobal(QCursor::pos());
      host->DragSourceEndedAt(cursor.x(), cursor.y(), result);
      host->DragSourceSystemDragEnded();
    }
  }
}

void
QCefViewPrivate::onBeforeCefContextMenu(const MenuBuilder::MenuData& data)
{
  Q_Q(QCefView);
  if (!q) {
    return;
  }

  // clear previous context menu
  if (osr.contextMenu_) {
    osr.contextMenu_->close();
    osr.contextMenu_->deleteLater();
    osr.contextMenu_ = nullptr;
  }

  // create context menu
  osr.contextMenu_ = new QMenu(q);
  osr.contextMenu_->setAttribute(Qt::WA_DeleteOnClose);

  // connect context menu signals
  connect(osr.contextMenu_, SIGNAL(triggered(QAction*)), this, SLOT(onContextMenuTriggered(QAction*)));
  connect(osr.contextMenu_, SIGNAL(destroyed(QObject*)), this, SLOT(onContextMenuDestroyed(QObject*)));

  // build menu from data
  MenuBuilder::BuildQtMenuFromMenuData(osr.contextMenu_, data);
}

void
QCefViewPrivate::onRunCefContextMenu(QPoint pos, CefRefPtr<CefRunContextMenuCallback> callback)
{
  Q_Q(QCefView);
  if (!q) {
    return;
  }

  // keep the context menu callback
  osr.contextMenuCallback_ = callback;

  // show context menu
  osr.isShowingContextMenu_ = true;
#if QT_VERSION < QT_VERSION_CHECK(6, 4, 0)
  QContextMenuEvent* e = new QContextMenuEvent(QContextMenuEvent::Other, pos);
#else
  QPoint globalPos = q->mapToGlobal(pos);
  QContextMenuEvent* e =
    new QContextMenuEvent(QContextMenuEvent::Other, pos, globalPos, QGuiApplication::keyboardModifiers());
#endif
  qApp->sendEvent(q, e);
}

void
QCefViewPrivate::onCefContextMenuDismissed()
{
  osr.contextMenuCallback_ = nullptr;
}

void
QCefViewPrivate::onFileDialog(CefBrowserHost::FileDialogMode mode,
                              const QString& title,
                              const QString& default_file_path,
                              const QStringList& accept_filters,
#if CEF_VERSION_MAJOR < 102
                              int selected_accept_filter,
#endif
                              CefRefPtr<CefFileDialogCallback> callback)
{
  Q_Q(QCefView);
  if (!q) {
    return;
  }

  // create dialog and set mode
  QFileDialog dialog(q);
  if (mode == FILE_DIALOG_OPEN) {
    dialog.setFileMode(QFileDialog::ExistingFile);
  } else if (mode == FILE_DIALOG_OPEN_MULTIPLE) {
    dialog.setFileMode(QFileDialog::ExistingFiles);
  } else if (mode == FILE_DIALOG_OPEN_FOLDER) {
    dialog.setFileMode(QFileDialog::Directory);
  } else if (mode == FILE_DIALOG_SAVE) {
    dialog.setAcceptMode(QFileDialog::AcceptSave);
  } else {
    callback->Cancel();
    return;
  }

  // set title
  if (!title.isEmpty()) {
    dialog.setWindowTitle(title);
  }

  // set initial folder
  if (!default_file_path.isEmpty() && mode == FILE_DIALOG_SAVE) {
    QFileInfo fileInfo(default_file_path);
    dialog.setDirectory(fileInfo.dir());
    dialog.selectFile(fileInfo.fileName());
  }

  //// set accepted file types
  //dialog.setNameFilters(accept_filters);

  // 修复后（只弹1次，合并所有类型）：
  if (!accept_filters.isEmpty()) {
    QString allTypes = accept_filters.join(" ");
    dialog.setNameFilter("(" + allTypes + ")");
  }

  // execute the dialog
  if (dialog.exec()) {
    std::vector<CefString> file_paths;
    auto selected_files = dialog.selectedFiles();
    for (const auto& file : selected_files) {
      file_paths.push_back(QDir::toNativeSeparators(file).toStdString());
    }

    // 如果网页定义了 onFilePickerReturn(result){ console.log(result) } 函数，可以通过此函数获取返回值
    QJsonArray fileArray;
    foreach (auto fileString, selected_files) {
      fileArray.append(fileString);
    }
    QString jsData = QJsonDocument(fileArray).toJson(QJsonDocument::Compact);
    executeJavascript(0, QStringLiteral("onFilePickerReturn('%1')").arg(jsData), QString());

#if CEF_VERSION_MAJOR < 102
    int index = accept_filters.indexOf(dialog.selectedNameFilter());
    callback->Continue(index, file_paths);
#else
    callback->Continue(file_paths);
#endif
  } else {
    QJsonArray fileArray;
    QString jsData = QJsonDocument(fileArray).toJson(QJsonDocument::Compact);
    executeJavascript(0, QStringLiteral("onFilePickerReturn('%1')").arg(jsData), QString());
    callback->Cancel();
  }
}

bool
QCefViewPrivate::hasDevTools()
{
  if (pCefBrowser_) {
    CefRefPtr<CefBrowserHost> host = pCefBrowser_->GetHost();
    if (host) {
      return host->HasDevTools();
    }
  }

  return false;
}

void
QCefViewPrivate::showDevTools()
{
  if (pCefBrowser_) {
    CefRefPtr<CefBrowserHost> host = pCefBrowser_->GetHost();
    if (host) {
      CefWindowInfo info;
      CefBrowserSettings settings;
      CefPoint point;
      host->ShowDevTools(info, nullptr, settings, point);
    }
  }
}

void
QCefViewPrivate::closeDevTools()
{
  if (pCefBrowser_) {
    CefRefPtr<CefBrowserHost> host = pCefBrowser_->GetHost();
    if (host) {
      host->CloseDevTools();
    }
  }
}

bool
QCefViewPrivate::shouldAllowDrop(CefRefPtr<CefDragData>& dragData, CefDragHandler::DragOperationsMask mask)
{
  Q_Q(QCefView);
  if (!q) {
    return false;
  }

  return q->acceptDrops();
}

bool
QCefViewPrivate::shouldAllowDrag(CefRefPtr<CefDragData>& dragData,
                                 CefRenderHandler::DragOperationsMask allowedOps,
                                 int x,
                                 int y)
{
  // This method only works for OSR mode
  if (!allowDrag_) {
    // return false to cancel the drag operation
    return false;
  }

  // start dragging
  QMetaObject::invokeMethod(
    this, //
    [this, dragData, allowedOps]() mutable {
      //
      this->onStartDragging(dragData, allowedOps);
    },                   //
    Qt::QueuedConnection //
  );
  return true;
}

void
QCefViewPrivate::updateDragOperation(CefRenderHandler::DragOperationsMask allowedOps)
{
  if (isOSRModeEnabled_) {
    osr.allowedDragOperations_ = MapCefDragOperationToQDropAction(allowedOps);
  }
}

bool
QCefViewPrivate::eventFilter(QObject* watched, QEvent* event)
{
  Q_Q(QCefView);
  if (!q) {
    return QObject::eventFilter(watched, event);
  }

  switch (event->type()) {
    case QEvent::Move: {
      // if any ancestor was moved we need to notify CEF
      QWidget* w = qobject_cast<QWidget*>(watched);
      if (w && w->isAncestorOf(q)) {
        notifyMoveOrResizeStarted();
      }
    } break;
    case QEvent::ParentChange: {
      // if any ancestor's parent was changed,
      // we need to reconnect the screenChanged signal/slots
      QWidget* w = qobject_cast<QWidget*>(watched);
      if (w && w->isAncestorOf(q)) {
        // disconnect old
        disconnect(SIGNAL(screenChanged(QScreen*)),    //
                   this,                               //
                   SLOT(onViewScreenChanged(QScreen*)) //
        );

        // connect new
        if (q->window()->windowHandle()) {
          connect(q->window()->windowHandle(),        //
                  SIGNAL(screenChanged(QScreen*)),    //
                  this,                               //
                  SLOT(onViewScreenChanged(QScreen*)) //
          );
        }
      }
    } break;
    case QEvent::PlatformSurface: {
      if (!isOSRModeEnabled_) {
        // the surface is about to be destroyed (top-level window is being closed)
        auto t = static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType();
        if (QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed == t) {
          if (watched == ncw.qBrowserWindow_->cefWindow()) {
            // detach the cef window
            ncw.qBrowserWindow_->detachCefWindow();
          }
        }
      }
    } break;
    case QEvent::FocusIn: {
      if (!isOSRModeEnabled_ && watched == ncw.qBrowserWindow_) {
        //qDebug() << "----- event to ncw.qBrowserWindow_:" << event;
        q->focusInEvent(static_cast<QFocusEvent*>(event));
      }
    } break;
    case QEvent::FocusOut: {
      if (!isOSRModeEnabled_ && watched == ncw.qBrowserWindow_) {
        //qDebug() << "----- event to ncw.qBrowserWindow_:" << event;
        q->focusOutEvent(static_cast<QFocusEvent*>(event));
      }
    } break;
    default:
      break;
  }

  return QObject::eventFilter(watched, event);
}

QVariant
QCefViewPrivate::onViewInputMethodQuery(Qt::InputMethodQuery query) const
{
  if (isOSRModeEnabled_) {
    // OSR mode
    switch (query) {
      case Qt::ImCursorRectangle:
        return QVariant(osr.imeCursorRect_);
      case Qt::ImAnchorRectangle:
        break;
      case Qt::ImFont:
        break;
      case Qt::ImCursorPosition:
        break;
      case Qt::ImSurroundingText:
        break;
      case Qt::ImCurrentSelection:
        break;
      case Qt::ImMaximumTextLength:
        break;
      case Qt::ImAnchorPosition:
        break;
      default:
        break;
    }
  }
  return QVariant();
}

void
QCefViewPrivate::onViewInputMethodEvent(QInputMethodEvent* event)
{
  if (isOSRModeEnabled_) {
    // OSR mode
    if (!pCefBrowser_)
      return;

    auto composingText = event->preeditString();
    auto composedText = event->commitString();
    if (composingText.isEmpty() && composedText.isEmpty()) {
      // cancel composition
      pCefBrowser_->GetHost()->ImeCancelComposition();
      return;
    }

    // collect IME selection range from Qt IME event
    CefRange selectionRange;
    for (auto& attr : event->attributes()) {
      switch (attr.type) {
        case QInputMethodEvent::TextFormat:
          break;
        case QInputMethodEvent::Cursor:
          selectionRange.Set(attr.start, attr.start);
          break;
        case QInputMethodEvent::Language:
        case QInputMethodEvent::Ruby:
        case QInputMethodEvent::Selection:
          break;
        default:
          break;
      }
    }

    CefCompositionUnderline underline;
    underline.background_color = 0;

    if (!composedText.isEmpty()) {
      // Because Chrome OSR needs the IME's pre-edit to get the cursor position for the candidate panel,
      // and some IMEs (e.g., Sogou) lack this feature, we simulate it here using the current input.
      underline.range = { 0, static_cast<decltype(CefRange::to)>(composedText.length()) };
      pCefBrowser_->GetHost()->ImeSetComposition( //
        composedText.toStdString(),               //
        { underline },                            //
        CefRange(UINT32_MAX, UINT32_MAX),         //
        selectionRange                            //
      );

      // commit composed text
      pCefBrowser_->GetHost()->ImeCommitText(composedText.toStdString(), CefRange(UINT32_MAX, UINT32_MAX), 0);
    } else {
      // update composition text
      underline.range = { 0, static_cast<decltype(CefRange::to)>(composingText.length()) };
      pCefBrowser_->GetHost()->ImeSetComposition( //
        composingText.toStdString(),              //
        { underline },                            //
        CefRange(UINT32_MAX, UINT32_MAX),         //
        selectionRange                            //
      );
    }
  }
}

void
QCefViewPrivate::onViewVisibilityChanged(bool visible)
{
  Q_Q(QCefView);
  if (!q) {
    return;
  }

  if (q->window()->windowHandle()) {
    connect(q->window()->windowHandle(),         //
            SIGNAL(screenChanged(QScreen*)),     //
            this,                                //
            SLOT(onViewScreenChanged(QScreen*)), //
            Qt::UniqueConnection);
  }

  // tell cef to start/stop rendering
  if (isOSRModeEnabled_) {
    // OSR mode
    if (pCefBrowser_)
      pCefBrowser_->GetHost()->WasHidden(!visible);
  } else {
    // if (!ncw.qBrowserWidget_)
    //   return;

    //// for better user experience (eliminate flicker), we don't change the size here
    //// if the consumer of QCefView need to constraint the resource
    //// then they need to handle the size change themselves
    // if (visible) {
    //   // restore cef window size
    //   ncw.qBrowserWidget_->resize(q->frameSize());
    // } else {
    //   // set cef window size to 0x0 to stop rendering and reduce resource usage
    //   // please refer to:
    //   // https://bitbucket.org/chromiumembedded/cef/issues/2310/allow-cefbrowser-washidden-to-work-for
    //   ncw.qBrowserWidget_->resize(0, 0);
    // }
  }
}

void
QCefViewPrivate::onViewFocusChanged(bool focused)
{
  if (!pCefBrowser_)
    return;

  if (isOSRModeEnabled_) {
    // if context menu is showing we need to skip the focus out event
    if (!focused && osr.isShowingContextMenu_) {
      return;
    }

    // if focused in and CEF does not have focus, we need to set focus to CEF
    if (focused && !osr.hasCefGotFocus_) {
      pCefBrowser_->GetHost()->SetFocus(true);
      return;
    }
  } else {
    // sync focus status to CEF
    pCefBrowser_->GetHost()->SetFocus(focused);
  }
}

void
QCefViewPrivate::onPaintEvent(QPaintEvent* event)
{
  Q_Q(QCefView);
  if (!q) {
    return;
  }

  if (osr.pRenderer_ && pCefBrowser_) {
    osr.pRenderer_->render();
  }
}

void
QCefViewPrivate::onViewMoved()
{
  notifyMoveOrResizeStarted();
}

void
QCefViewPrivate::onViewSizeChanged(const QSize& size, const QSize& oldSize)
{
  Q_Q(QCefView);
  if (!q) {
    return;
  }

  notifyMoveOrResizeStarted();

  if (isOSRModeEnabled_) {
    // OSR mode
    if (pCefBrowser_) {
      // invalidate the CEF OSR buffer
      pCefBrowser_->GetHost()->Invalidate(CefBrowserHost::PaintElementType::PET_VIEW);
    }

    if (osr.pRenderer_) {
      // resize render
      osr.pRenderer_->resize(size.width(), size.height(), scaleFactor());
    }

    // OSR mode
    if (pCefBrowser_) {
      // notify CEF of the size changing
      pCefBrowser_->GetHost()->WasResized();
    }
  } else {
    if (ncw.qBrowserWindow_) {
      QRegion q_ptrRegion = q->mask();
      QRect q_ptrRegionRect = q_ptrRegion.boundingRect();
      QRegion qBrowserWindowRegion = QRegion(q_ptrRegionRect.x(), q_ptrRegionRect.y(), size.width(), size.height());
      ncw.qBrowserWindow_->applyMask(qBrowserWindowRegion);
    }
  }
}

void
QCefViewPrivate::onViewKeyEvent(QKeyEvent* event)
{
  if (isOSRModeEnabled_) {
    // OSR mode
    if (!pCefBrowser_)
      return;

    // #if defined(QT_DEBUG)
    //     qDebug("==== onViewKeyEvent:key=%d, nativeVirtualKey=0x%02x, nativeScanCode=0x%02x, modifiers=0x%08x, "
    //            "nativeModifiers=0x%08x, text=%s",
    //            (Qt::Key)(event->key()),
    //            event->nativeVirtualKey(),
    //            event->nativeScanCode(),
    //            (quint32)(event->modifiers()),
    //            event->nativeModifiers(),
    //            event->text().toStdString().c_str());
    // #endif

    CefKeyEvent e;
    MapQKeyEventToCefKeyEvent(event, e);

    // QEvent::KeyRelease - send key release event
    if (event->type() == QEvent::KeyRelease) {
      e.type = KEYEVENT_KEYUP;
      pCefBrowser_->GetHost()->SendKeyEvent(e);
      return;
    }

    // QEvent::KeyPress - send key down event
    e.type = KEYEVENT_RAWKEYDOWN;
    pCefBrowser_->GetHost()->SendKeyEvent(e);

    // send key char event
    if (ShouldSendKeyCharEvent(event)) {
      AdjustCefKeyCharEvent(event, e);
      e.type = KEYEVENT_CHAR;
      pCefBrowser_->GetHost()->SendKeyEvent(e);
    }
  }
}

void
QCefViewPrivate::onViewMouseEvent(QMouseEvent* event, bool mouseLeave, bool mouseUp)
{
  if (isOSRModeEnabled_) {
    // OSR mode
    if (!pCefBrowser_)
      return;

    auto b = event->buttons();
    auto m = event->modifiers();

    CefMouseEvent e;
    e.x = ((QMouseEvent*)event)->pos().x();
    e.y = ((QMouseEvent*)event)->pos().y();
    e.modifiers |= m & Qt::ControlModifier ? EVENTFLAG_CONTROL_DOWN : 0;
    e.modifiers |= m & Qt::ShiftModifier ? EVENTFLAG_SHIFT_DOWN : 0;
    e.modifiers |= m & Qt::AltModifier ? EVENTFLAG_ALT_DOWN : 0;
    e.modifiers |= b & Qt::LeftButton ? EVENTFLAG_LEFT_MOUSE_BUTTON : 0;
    e.modifiers |= b & Qt::RightButton ? EVENTFLAG_RIGHT_MOUSE_BUTTON : 0;
    e.modifiers |= b & Qt::MiddleButton ? EVENTFLAG_MIDDLE_MOUSE_BUTTON : 0;

    if (QEvent::MouseMove == event->type()) {
      pCefBrowser_->GetHost()->SendMouseMoveEvent(e, mouseLeave);
      return;
    }

    //qDebug() << "====== onViewMouseEvent:" << event;

    CefBrowserHost::MouseButtonType mbt = MBT_LEFT;
    switch (event->button()) {
      case Qt::LeftButton: {
        mbt = MBT_LEFT;
      } break;
      case Qt::RightButton: {
        mbt = MBT_RIGHT;
      } break;
      case Qt::MiddleButton: {
        mbt = MBT_MIDDLE;
      } break;
      default:
        break;
    }

    if (QEvent::MouseButtonPress == event->type()) {
      pCefBrowser_->GetHost()->SendMouseClickEvent(e, mbt, mouseUp, 1);
    } else if (QEvent::MouseButtonDblClick == event->type()) {
      pCefBrowser_->GetHost()->SendMouseClickEvent(e, mbt, mouseUp, 2);
    } else if (QEvent::MouseButtonRelease == event->type()) {
      // if the release was generated right after a popup, we must discard it
      pCefBrowser_->GetHost()->SendMouseClickEvent(e, mbt, mouseUp, 1);
    }
  }
}

void
QCefViewPrivate::onViewWheelEvent(QWheelEvent* event)
{
  if (isOSRModeEnabled_) {
    // OSR mode
#if QT_VERSION < QT_VERSION_CHECK(5, 14, 0)
    auto p = event->pos();
#else
    auto p = event->position();
#endif

    auto d = event->angleDelta();
    auto m = event->modifiers();
    auto b = event->buttons();

    CefMouseEvent e;
    e.modifiers |= m & Qt::ControlModifier ? EVENTFLAG_CONTROL_DOWN : 0;
    e.modifiers |= m & Qt::ShiftModifier ? EVENTFLAG_SHIFT_DOWN : 0;
    e.modifiers |= m & Qt::AltModifier ? EVENTFLAG_ALT_DOWN : 0;
    e.modifiers |= b & Qt::LeftButton ? EVENTFLAG_LEFT_MOUSE_BUTTON : 0;
    e.modifiers |= b & Qt::RightButton ? EVENTFLAG_RIGHT_MOUSE_BUTTON : 0;
    e.modifiers |= b & Qt::MiddleButton ? EVENTFLAG_MIDDLE_MOUSE_BUTTON : 0;

    e.x = p.x();
    e.y = p.y();

    // Prevent diagonal scrolling: only allow scrolling in one direction at a time
    if (qAbs(d.x()) > qAbs(d.y())) {
      d.setY(0);
    } else {
      d.setX(0);
    }

    pCefBrowser_->GetHost()->SendMouseWheelEvent(e, d.x(), d.y());
  }
}

void
QCefViewPrivate::onContextMenuEvent(const QPoint& pos)
{
  if (isOSRModeEnabled_) {
    // OSR mode
    if (osr.isShowingContextMenu_) {
      osr.contextMenu_->popup(pos);
    }
  }
}

void
QCefViewPrivate::onDragEnter(QDragEnterEvent* event)
{
  FLog();

  if (isOSRModeEnabled_ && pCefBrowser_ && pCefBrowser_->GetHost()) {
    osr.allowedDragOperations_ = 0;

    CefRefPtr<CefDragData> dragData = CreateCefDragDataFromQMimeData(*(event->mimeData()));

    CefMouseEvent e;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    auto b = event->mouseButtons();
    auto m = event->keyboardModifiers();
    e.x = event->pos().x();
    e.y = event->pos().y();
#else
    auto b = event->buttons();
    auto m = event->modifiers();
    e.x = event->position().x();
    e.y = event->position().y();
#endif
    e.modifiers |= m & Qt::ControlModifier ? EVENTFLAG_CONTROL_DOWN : 0;
    e.modifiers |= m & Qt::ShiftModifier ? EVENTFLAG_SHIFT_DOWN : 0;
    e.modifiers |= m & Qt::AltModifier ? EVENTFLAG_ALT_DOWN : 0;
    e.modifiers |= b & Qt::LeftButton ? EVENTFLAG_LEFT_MOUSE_BUTTON : 0;
    e.modifiers |= b & Qt::RightButton ? EVENTFLAG_RIGHT_MOUSE_BUTTON : 0;
    e.modifiers |= b & Qt::MiddleButton ? EVENTFLAG_MIDDLE_MOUSE_BUTTON : 0;

    auto proposedAction = event->proposedAction();
    CefBrowserHost::DragOperationsMask allowedOps = MapQDropActionToCefDragOperation(proposedAction);
    pCefBrowser_->GetHost()->DragTargetDragEnter(dragData, e, allowedOps);
    pCefBrowser_->GetHost()->DragTargetDragOver(e, allowedOps);

    event->setDropAction(static_cast<Qt::DropAction>(osr.allowedDragOperations_));
    event->accept();
  }
}

void
QCefViewPrivate::onDragMove(QDragMoveEvent* event)
{
  FLog();

  if (isOSRModeEnabled_ && pCefBrowser_ && pCefBrowser_->GetHost()) {
    CefMouseEvent e;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    auto b = event->mouseButtons();
    auto m = event->keyboardModifiers();
    e.x = event->pos().x();
    e.y = event->pos().y();
#else
    auto b = event->buttons();
    auto m = event->modifiers();
    e.x = event->position().x();
    e.y = event->position().y();
#endif
    e.modifiers |= m & Qt::ControlModifier ? EVENTFLAG_CONTROL_DOWN : 0;
    e.modifiers |= m & Qt::ShiftModifier ? EVENTFLAG_SHIFT_DOWN : 0;
    e.modifiers |= m & Qt::AltModifier ? EVENTFLAG_ALT_DOWN : 0;
    e.modifiers |= b & Qt::LeftButton ? EVENTFLAG_LEFT_MOUSE_BUTTON : 0;
    e.modifiers |= b & Qt::RightButton ? EVENTFLAG_RIGHT_MOUSE_BUTTON : 0;
    e.modifiers |= b & Qt::MiddleButton ? EVENTFLAG_MIDDLE_MOUSE_BUTTON : 0;

    auto proposedAction = event->proposedAction();
    CefBrowserHost::DragOperationsMask allowedOps = MapQDropActionToCefDragOperation(proposedAction);
    pCefBrowser_->GetHost()->DragTargetDragOver(e, allowedOps);

    event->setDropAction(static_cast<Qt::DropAction>(osr.allowedDragOperations_));
    event->accept();
  }
}

void
QCefViewPrivate::onDragLeave(QDragLeaveEvent* event)
{
  FLog();

  if (isOSRModeEnabled_ && pCefBrowser_ && pCefBrowser_->GetHost()) {
    osr.allowedDragOperations_ = 0;

    pCefBrowser_->GetHost()->DragTargetDragLeave();
  }
}

void
QCefViewPrivate::onDrop(QDropEvent* event)
{
  FLog();

  if (isOSRModeEnabled_ && pCefBrowser_ && pCefBrowser_->GetHost()) {

    CefMouseEvent e;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    auto b = event->mouseButtons();
    auto m = event->keyboardModifiers();
    e.x = event->pos().x();
    e.y = event->pos().y();
#else
    auto b = event->buttons();
    auto m = event->modifiers();
    e.x = event->position().x();
    e.y = event->position().y();
#endif
    e.modifiers |= m & Qt::ControlModifier ? EVENTFLAG_CONTROL_DOWN : 0;
    e.modifiers |= m & Qt::ShiftModifier ? EVENTFLAG_SHIFT_DOWN : 0;
    e.modifiers |= m & Qt::AltModifier ? EVENTFLAG_ALT_DOWN : 0;
    e.modifiers |= b & Qt::LeftButton ? EVENTFLAG_LEFT_MOUSE_BUTTON : 0;
    e.modifiers |= b & Qt::RightButton ? EVENTFLAG_RIGHT_MOUSE_BUTTON : 0;
    e.modifiers |= b & Qt::MiddleButton ? EVENTFLAG_MIDDLE_MOUSE_BUTTON : 0;

    pCefBrowser_->GetHost()->DragTargetDrop(e);

    event->acceptProposedAction();
  }
}

int
QCefViewPrivate::browserId()
{
  if (pCefBrowser_)
    return pCefBrowser_->GetIdentifier();

  return -1;
}

void
QCefViewPrivate::navigateToString(const QString& content)
{
  std::string data = content.toStdString();
  data = CefURIEncode(CefBase64Encode(data.c_str(), data.size()), false).ToString();
  data = "data:text/html;base64," + data;
  lastUrl_.FromString(data);

  if (pCefBrowser_) {
    pCefBrowser_->GetMainFrame()->LoadURL(lastUrl_);
  }
}

void
QCefViewPrivate::navigateToUrl(const QString& url)
{
  lastUrl_.FromString(url.toStdString());

  if (pCefBrowser_) {
    pCefBrowser_->GetMainFrame()->LoadURL(lastUrl_);
  }
}

bool
QCefViewPrivate::browserCanGoBack()
{
  if (pCefBrowser_)
    return pCefBrowser_->CanGoBack();

  return false;
}

bool
QCefViewPrivate::browserCanGoForward()
{
  if (pCefBrowser_)
    return pCefBrowser_->CanGoForward();

  return false;
}

void
QCefViewPrivate::browserGoBack()
{
  if (pCefBrowser_)
    pCefBrowser_->GoBack();
}

void
QCefViewPrivate::browserGoForward()
{
  if (pCefBrowser_)
    pCefBrowser_->GoForward();
}

bool
QCefViewPrivate::browserIsLoading()
{
  if (pCefBrowser_)
    return pCefBrowser_->IsLoading();

  return false;
}

void
QCefViewPrivate::browserReload()
{
  if (pCefBrowser_)
    pCefBrowser_->Reload();
}

void
QCefViewPrivate::browserStopLoad()
{
  if (pCefBrowser_)
    pCefBrowser_->StopLoad();
}

bool
QCefViewPrivate::triggerEvent(const QString& name,
                              const QVariantList& args,
                              const QCefFrameId& frameId /*= QCefView::MainFrameID*/)
{
  if (!name.isEmpty()) {
    return sendEventNotifyMessage(frameId, name, args);
  }

  return false;
}

bool
QCefViewPrivate::responseQCefQuery(const QCefQuery& query)
{
  if (pClient_) {
    CefString res;
    res.FromString(query.response().toStdString());
    query.markAsReplied();
    return pClient_->ResponseQuery(query.id(), query.result(), res, query.error());
  }
  return false;
}

bool
QCefViewPrivate::responseQCefQuery(const int64_t query, bool success, const QString& response, int error)
{
  if (pClient_) {
    CefString res;
    res.FromString(response.toStdString());
    return pClient_->ResponseQuery(query, success, res, error);
  }
  return false;
}

bool
QCefViewPrivate::executeJavascript(const QCefFrameId& frameId, const QString& code, const QString& url)
{
  if (code.isEmpty())
    return false;

  if (pCefBrowser_) {
    // convert frame id
    auto fid = ValueConvertor::FrameIdQ2C(frameId);
    // get frame instance
    auto frame = (frameId == QCefView::MainFrameID) ? //
                   pCefBrowser_->GetMainFrame()       // get main frame
                                                    : //
#if CEF_VERSION_MAJOR < 122                           // get frame by id
                   pCefBrowser_->GetFrame(fid);
#else
                   pCefBrowser_->GetFrameByIdentifier(fid);
#endif

    // validate frame instance
    if (!frame)
      return false;

    CefString c;
    c.FromString(code.toStdString());

    CefString u;
    if (url.isEmpty()) {
      u = frame->GetURL();
    } else {
      u.FromString(url.toStdString());
    }

    frame->ExecuteJavaScript(c, u, 0);
    return true;
  }

  return false;
}

bool
QCefViewPrivate::executeJavascriptWithResult(const QCefFrameId& frameId,
                                             const QString& code,
                                             const QString& url,
                                             const QString& context)
{
  if (code.isEmpty())
    return false;

  if (pClient_ && pCefBrowser_) {
    // convert frame id
    auto fid = ValueConvertor::FrameIdQ2C(frameId);
    // get frame instance
    auto frame = (frameId == QCefView::MainFrameID) ? //
                   pCefBrowser_->GetMainFrame()       // get main frame
                                                    : //
#if CEF_VERSION_MAJOR < 122                           // get frame by id
                   pCefBrowser_->GetFrame(fid);
#else
                   pCefBrowser_->GetFrameByIdentifier(fid);
#endif

    // validate frame instance
    if (!frame)
      return false;

    CefString c;
    c.FromString(code.toStdString());

    CefString u;
    if (url.isEmpty()) {
      u = frame->GetURL();
    } else {
      u.FromString(url.toStdString());
    }

    CefString ctx;
    ctx.FromString(context.toStdString());

    return pClient_->AsyncExecuteJSCode(pCefBrowser_, frame, c, u, ctx);
  }

  return false;
}

void
QCefViewPrivate::notifyMoveOrResizeStarted()
{
#if defined(Q_OS_WINDOWS) || defined(Q_OS_LINUX)
  if (pCefBrowser_) {
    CefRefPtr<CefBrowserHost> host = pCefBrowser_->GetHost();
    if (host) {
      host->NotifyMoveOrResizeStarted();
    }
  }
#endif
}

bool
QCefViewPrivate::sendEventNotifyMessage(const QCefFrameId& frameId, const QString& name, const QVariantList& args)
{
  if (!pClient_) {
    return false;
  }

  CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create(kCefViewClientBrowserTriggerEventMessage);
  CefRefPtr<CefListValue> arguments = msg->GetArgumentList();

  //** arguments(CefValueList)
  //** +------------+
  //** | event name |
  //** | event arg1 |
  //** | event arg2 |
  //** | event arg3 |
  //** | event arg4 |
  //** |    ...     |
  //** |    ...     |
  //** |    ...     |
  //** |    ...     |
  //** +------------+
  int idx = 0;
  CefString eventName = name.toStdString();
  arguments->SetString(idx++, eventName);
  for (auto& qV : args) {
    auto cVal = CefValue::Create();
    ValueConvertor::QVariantToCefValue(cVal.get(), &qV);
    arguments->SetValue(idx++, cVal);
  }

  return pClient_->TriggerEvent(pCefBrowser_, ValueConvertor::FrameIdQ2C(frameId), msg);
}

bool
QCefViewPrivate::setPreference(const QString& name, const QVariant& value, QString& error)
{
  if (pCefBrowser_) {
    CefRefPtr<CefBrowserHost> host = pCefBrowser_->GetHost();
    if (host) {
      CefString n;
      n.FromString(name.toStdString());

      auto v = CefValue::Create();
      ValueConvertor::QVariantToCefValue(v.get(), &value);

      CefString e;
      auto r = host->GetRequestContext()->SetPreference(n, v, e);
      error = QString::fromStdString(e.ToString());
      return r;
    }
  }

  return false;
}

void
QCefViewPrivate::setOSRFrameRate(int fps)
{
  if (pCefBrowser_) {
    CefRefPtr<CefBrowserHost> host = pCefBrowser_->GetHost();
    if (host) {
      host->SetWindowlessFrameRate(fps);
    }
  }
}

void
QCefViewPrivate::setZoomLevel(double level)
{
  if (pCefBrowser_) {
    CefRefPtr<CefBrowserHost> host = pCefBrowser_->GetHost();
    if (host) {
      host->SetZoomLevel(level);
    }
  }
}

double
QCefViewPrivate::zoomLevel()
{
  if (pCefBrowser_) {
    CefRefPtr<CefBrowserHost> host = pCefBrowser_->GetHost();
    if (host) {
      return host->GetZoomLevel();
    }
  }

  return 0;
}
