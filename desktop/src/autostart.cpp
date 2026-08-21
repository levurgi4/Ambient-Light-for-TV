#include "autostart.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

namespace ambilight::autostart {

namespace {

constexpr auto kRunKey =
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr auto kValueName = "Ambilight by Levurgi4";

QString launchCommand()
{
    const auto executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return QStringLiteral("\"%1\" --minimized").arg(executable);
}

} // namespace

bool isEnabled()
{
#ifdef Q_OS_WIN
    QSettings registry(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    return !registry.value(QString::fromLatin1(kValueName)).toString().isEmpty();
#else
    return false;
#endif
}

bool setEnabled(const bool enabled, QString* errorMessage)
{
#ifdef Q_OS_WIN
    QSettings registry(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    if (enabled) {
        registry.setValue(QString::fromLatin1(kValueName), launchCommand());
    } else {
        registry.remove(QString::fromLatin1(kValueName));
    }
    registry.sync();

    if (registry.status() != QSettings::NoError) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Unable to update the current user's Run registry key.");
        }
        return false;
    }
    return true;
#else
    if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("Autostart is supported only on Windows.");
    }
    return !enabled;
#endif
}

} // namespace ambilight::autostart
