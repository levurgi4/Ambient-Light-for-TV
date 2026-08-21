#include "main_window.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QIcon>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

class SingleInstanceGuard final {
public:
    SingleInstanceGuard()
    {
#ifdef Q_OS_WIN
        SetLastError(ERROR_SUCCESS);
        mutex_ = CreateMutexW(
            nullptr,
            TRUE,
            L"Local\\Levurgi4.AmbilightByLevurgi4.Instance");
        if (mutex_ == nullptr) {
            // Fail closed if an existing higher-integrity instance denies a
            // full mutex handle. The activation broadcast is still attempted,
            // and the message filter on the primary window permits it.
            primary_ = false;
        } else {
            const DWORD creationError = GetLastError();
            if (creationError != ERROR_ALREADY_EXISTS) {
                primary_ = true;
                ownsMutex_ = true;
            } else {
                const DWORD waitResult = WaitForSingleObject(mutex_, 0);
                if (waitResult == WAIT_OBJECT_0
                    || waitResult == WAIT_ABANDONED) {
                    primary_ = true;
                    ownsMutex_ = true;
                } else {
                    primary_ = false;
                }
            }
        }

        readyEvent_ = CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            L"Local\\Levurgi4.AmbilightByLevurgi4.Ready");
        activationAckEvent_ = CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            ambilight::kSingleInstanceActivationAckEventName);
        if (primary_ && readyEvent_ != nullptr) {
            ResetEvent(readyEvent_);
        }
        if (primary_ && activationAckEvent_ != nullptr) {
            ResetEvent(activationAckEvent_);
        }
#endif
    }

    ~SingleInstanceGuard()
    {
#ifdef Q_OS_WIN
        if (readyEvent_ != nullptr) {
            CloseHandle(readyEvent_);
        }
        if (activationAckEvent_ != nullptr) {
            CloseHandle(activationAckEvent_);
        }
        if (ownsMutex_ && mutex_ != nullptr) {
            ReleaseMutex(mutex_);
        }
        if (mutex_ != nullptr) {
            CloseHandle(mutex_);
        }
#endif
    }

    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    [[nodiscard]] bool isPrimary() const noexcept
    {
        return primary_;
    }

    void markReady() const noexcept
    {
#ifdef Q_OS_WIN
        if (primary_ && readyEvent_ != nullptr) {
            SetEvent(readyEvent_);
        }
#endif
    }

    [[nodiscard]] bool activatePrimaryOrTakeOver() noexcept
    {
#ifdef Q_OS_WIN
        if (readyEvent_ != nullptr && mutex_ != nullptr) {
            const HANDLE startupHandles[] {readyEvent_, mutex_};
            const DWORD startupResult =
                WaitForMultipleObjects(2, startupHandles, FALSE, 5'000);
            if (startupResult == WAIT_OBJECT_0 + 1
                || startupResult == WAIT_ABANDONED_0 + 1) {
                takeOwnership();
                return true;
            }
        } else if (readyEvent_ != nullptr) {
            WaitForSingleObject(readyEvent_, 5'000);
        }

        if (activationAckEvent_ != nullptr) {
            ResetEvent(activationAckEvent_);
        }
        AllowSetForegroundWindow(ASFW_ANY);
        const UINT activationMessage =
            RegisterWindowMessageW(
                ambilight::kSingleInstanceActivationMessageName);
        if (activationMessage != 0) {
            PostMessageW(HWND_BROADCAST, activationMessage, 0, 0);
        }

        if (activationAckEvent_ != nullptr && mutex_ != nullptr) {
            const HANDLE activationHandles[] {
                activationAckEvent_,
                mutex_,
            };
            const DWORD activationResult =
                WaitForMultipleObjects(
                    2,
                    activationHandles,
                    FALSE,
                    2'250);
            if (activationResult == WAIT_OBJECT_0) {
                return false;
            }
            if (activationResult == WAIT_OBJECT_0 + 1
                || activationResult == WAIT_ABANDONED_0 + 1) {
                takeOwnership();
                return true;
            }
        } else if (mutex_ != nullptr) {
            const DWORD mutexResult =
                WaitForSingleObject(mutex_, 2'250);
            if (mutexResult == WAIT_OBJECT_0
                || mutexResult == WAIT_ABANDONED) {
                takeOwnership();
                return true;
            }
        }
#endif
        return false;
    }

private:
#ifdef Q_OS_WIN
    void takeOwnership() noexcept
    {
        primary_ = true;
        ownsMutex_ = true;
        if (readyEvent_ != nullptr) {
            ResetEvent(readyEvent_);
        }
        if (activationAckEvent_ != nullptr) {
            ResetEvent(activationAckEvent_);
        }
    }
#endif

    bool primary_ = true;
#ifdef Q_OS_WIN
    HANDLE mutex_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    HANDLE activationAckEvent_ = nullptr;
    bool ownsMutex_ = false;
#endif
};

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);

    QCoreApplication::setOrganizationName(QStringLiteral("Levurgi4"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("levurgi4.local"));
    QCoreApplication::setApplicationName(QStringLiteral("Ambilight by Levurgi4"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.2.8"));
    QApplication::setQuitOnLastWindowClosed(false);
    application.setWindowIcon(
        QIcon(QStringLiteral(":/ambilight/branding/app-icon.png")));

    SingleInstanceGuard singleInstance;
    if (!singleInstance.isPrimary()
        && !singleInstance.activatePrimaryOrTakeOver()) {
        return 0;
    }

    ambilight::MainWindow window;
    if (application.arguments().contains(QStringLiteral("--minimized"))) {
        window.startMinimized();
    } else {
        window.show();
    }
    // A hidden tray-started window still needs a native HWND so that the
    // registered activation broadcast from a later launch can reach it.
    const WId nativeWindowId = window.winId();
#ifdef Q_OS_WIN
    const UINT activationMessage =
        RegisterWindowMessageW(
            ambilight::kSingleInstanceActivationMessageName);
    if (activationMessage != 0) {
        CHANGEFILTERSTRUCT filterResult {
            .cbSize = sizeof(CHANGEFILTERSTRUCT),
        };
        ChangeWindowMessageFilterEx(
            reinterpret_cast<HWND>(nativeWindowId),
            activationMessage,
            MSGFLT_ALLOW,
            &filterResult);
    }
#else
    Q_UNUSED(nativeWindowId)
#endif
    singleInstance.markReady();

    return QApplication::exec();
}
