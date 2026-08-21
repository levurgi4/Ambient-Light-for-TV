#pragma once

#include <QString>

namespace ambilight::autostart {

[[nodiscard]] bool isEnabled();
[[nodiscard]] bool setEnabled(bool enabled, QString* errorMessage = nullptr);

} // namespace ambilight::autostart
