# KNOWN ISSUES / 遗留跟进项

> 记录于 2026-10-28，对应 `fix/cef-init-failure` 分支（至 bfd25a1）。
> 该分支为 CefInitialize 失败路径引入优雅降级；代码审查确认正常初始化路径行为无回归。
> 2026-10-28 按修改范围纪律重写分支：45a438d（cookie 判空）与 c874d2c（单实例闸）
> 出组，各自记录为下方第 1/4 项。
> 以下各项均为审查中确认的遗留问题，非本分支改动引入；留待后续处理。

## 1. 重复 init()（同实例或跨实例）在 Release 下未受保护

- **位置**: `src/QCefContext.cpp:104`（`Q_ASSERT_X` 仅 Debug 生效）、`:105`（`s_self = this`）
- **触发条件**: Release（QT_NO_DEBUG）下断言被编译掉，任何二次 `init()` 都会再次进入
  `CefInitialize`：libcef 走 "already initialized" 路径时**释放调用方传入的 CefApp**，
  `initializeCef` 尾部的 `pApp_` 接管随即变成对已释放对象的操作（c0000005；实测旧库
  崩 initializeCef+0xb79，现场 feeefeee/BAADF00D freed 堆）。d887072 的返回值传播
  管不到这里——返回值还没传出去，传入对象已先被释放。
- **现状**: 生产宿主从不二次 init（重启走 `QProcess::startDetached` 进程重执行），
  故只记录不修（原 c874d2c 单实例闸已按范围纪律出组）。
- **建议**: 若未来出现进程内重建需求，在 `init()` 入口加 `s_self != nullptr` 守卫
  （同实例与跨实例统一拦截），拦截后保持「init 失败 → uninit → 重建」可用，见第 2 项。

## 2. init 失败后 s_self 持续占用，重建前必须先 uninit

- **位置**: `src/QCefContext.cpp:105`（`s_self = this` 先于 `d->initialize`，失败分支不复位）、
  `:122`（仅 `uninit()` 复位）
- **触发条件**: init 失败后不销毁/不 uninit 失败实例，直接新建 `QCefContext` 重试：
  `s_self` 已被占用，重试会再次进入 `CefInitialize`（第 1 项的崩溃面）。
- **注意**: 失败后保留 `s_self` 是**有意的** —— `QCefView` 构造依赖
  `QCefContext::instance()->d_func()`（`src/QCefView.cpp:31`），若失败即置空会导致 view 构造崩溃。
  因此修复方向是**补文档契约**，而不是在失败分支置空 `s_self`。
- **建议**: 在 `QCefContext::init()` / `isInitialized()` 的头文件注释中写明：
  init 失败后须先调用 `uninit()`（或销毁实例）才能重建新的 `QCefContext`。

## 3. Windows 下 init 失败泄漏 job object 句柄

- **位置**: `src/win/details/QCefContextPrivate_win.cpp:69`(创建)、`:98-101`(失败返回未关闭)、
  `:112`(`uninitializeCef` 因 `!pApp_` 提前返回，跳过 `CloseHandle`)
- **触发条件**: `CefInitialize` 失败 → `windowsJobHandle_` 泄漏至进程退出。
- **附带影响**: job 名由 PID 派生，同进程内重建 `QCefContext` 时 `CreateJobObjectA` 会按名
  打开同一个旧 job(KILL_ON_JOB_CLOSE 仍然生效)，状态轻微混乱。
- **建议**: 在失败分支关闭句柄，或把 `CloseHandle` 移到 `uninitializeCef` 的 `pApp_`
  判空之外(句柄清理与 CEF 生命周期解耦)。

## 4. 全局 cookie 管理器空指针（init 失败路径可达）

- **位置**: `src/details/QCefContextPrivate.cpp:103`（addGlobalCookie）、`:109`（deleteAllCookies）——
  `CefCookieManager::GetGlobalManager(nullptr)->` 直接链式解引用
- **触发条件**: `CefInitialize` 未成功时 GetGlobalManager 返回 null，链式调用即崩。
- **现状**: 宿主应用零调用点（grep 实证：仅 vendored 头声明；应用唯一的
  `deleteAllCookies` 调用是 Qt WebEngine 自己的 webEngineCookieStore，与本库无关），
  故只记录不修（原 45a438d 判空已按范围纪律出组）。
- **建议**: 若未来接入 cookie API，先把 manager 取入局部变量判空，qWarning + return false。
