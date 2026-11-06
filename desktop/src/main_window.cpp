#include "main_window.hpp"
#include "mirror_window.hpp"
#include "capture/mirror_geometry.hpp"

#include "autostart.hpp"
#include "audio/audio_capture_worker.hpp"
#include "capture/capture_output_selection.hpp"
#include "capture/capture_types.hpp"
#include "capture/screen_capture_worker.hpp"
#include "led_preview_widget.hpp"
#include "perimeter_mapper.hpp"
#include "transport/serial_transport_worker.hpp"

#include <ambilight/protocol/protocol.hpp>

#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyCombination>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLayout>
#include <QLibraryInfo>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QStringList>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QTranslator>
#include <QVBoxLayout>
#include <QVariant>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace ambilight {

namespace {

constexpr int kHotkeyId = 0x4C56;
constexpr int kFirmwareBaudRate = 2'000'000;
constexpr int kCaptureOutputSizeRole = Qt::UserRole + 1;
constexpr int kCaptureOutputLegacyIdRole = Qt::UserRole + 2;

protocol::ColorOrder protocolColorOrder(const QString& value)
{
    if (value == QStringLiteral("RGB")) {
        return protocol::ColorOrder::Rgb;
    }
    if (value == QStringLiteral("RBG")) {
        return protocol::ColorOrder::Rbg;
    }
    if (value == QStringLiteral("GBR")) {
        return protocol::ColorOrder::Gbr;
    }
    if (value == QStringLiteral("BRG")) {
        return protocol::ColorOrder::Brg;
    }
    if (value == QStringLiteral("BGR")) {
        return protocol::ColorOrder::Bgr;
    }
    return protocol::ColorOrder::Grb;
}

protocol::EffectType protocolEffect(const QString& value)
{
    if (value == QStringLiteral("solid")) {
        return protocol::EffectType::Solid;
    }
    if (value == QStringLiteral("wave")) {
        return protocol::EffectType::Wave;
    }
    if (value == QStringLiteral("rainbow")) {
        return protocol::EffectType::Rainbow;
    }
    return protocol::EffectType::Breathing;
}

protocol::OperatingMode protocolMode(const QString& value)
{
    if (value == QStringLiteral("ambilight")
        || value == QStringLiteral("equalizer")) {
        return protocol::OperatingMode::Ambilight;
    }
    if (value == QStringLiteral("solid")) {
        return protocol::OperatingMode::Solid;
    }
    if (value == QStringLiteral("breathing")
        || value == QStringLiteral("rainbow")
        || value == QStringLiteral("wave")) {
        return protocol::OperatingMode::Effect;
    }
    return protocol::OperatingMode::Off;
}

capture::RegionColorMode captureRegionColorMode(const QString& value)
{
    if (value == QStringLiteral("rms")) {
        return capture::RegionColorMode::RootMeanSquare;
    }
    if (value == QStringLiteral("dominant")) {
        return capture::RegionColorMode::Dominant;
    }
    return capture::RegionColorMode::Mean;
}

capture::LedTransferMode captureLedTransferMode(const QString& value)
{
    if (value == QStringLiteral("raw")) {
        return capture::LedTransferMode::Raw;
    }
    if (value == QStringLiteral("soft-gamma")) {
        return capture::LedTransferMode::SoftGamma;
    }
    if (value == QStringLiteral("strong-gamma")) {
        return capture::LedTransferMode::StrongGamma;
    }
    return capture::LedTransferMode::Srgb;
}

capture::BlackBarDetectionMode captureBlackBarDetectionMode(
    const QString& value)
{
    if (value == QStringLiteral("off")) {
        return capture::BlackBarDetectionMode::Off;
    }
    if (value == QStringLiteral("limit-21-9")) {
        return capture::BlackBarDetectionMode::Limited21x9;
    }
    return capture::BlackBarDetectionMode::FirstContent;
}

protocol::EffectConfig effectConfiguration(
    const QString& value,
    const QColor& requestedSolidColor = QColor(255, 96, 24))
{
    const QColor solidColor = requestedSolidColor.isValid()
        ? requestedSolidColor
        : QColor(255, 96, 24);
    const bool solid = value == QStringLiteral("solid");
    return {
        .effect = protocolEffect(value),
        .intensity = 255,
        .red = static_cast<protocol::Byte>(solid ? solidColor.red() : 255),
        .green = static_cast<protocol::Byte>(solid ? solidColor.green() : 96),
        .blue = static_cast<protocol::Byte>(solid ? solidColor.blue() : 24),
        .reserved = 0,
        .speedPermille = 1000,
    };
}

QLabel* makeFieldLabel(QWidget* parent)
{
    auto* label = new QLabel(parent);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return label;
}

QSpinBox* makeSpinBox(
    QWidget* parent,
    const int minimum,
    const int maximum,
    const QString& suffix = {})
{
    auto* spin = new QSpinBox(parent);
    spin->setRange(minimum, maximum);
    spin->setSuffix(suffix);
    spin->setKeyboardTracking(false);
    return spin;
}

void addComboItem(QComboBox* combo, const QString& value)
{
    combo->addItem(QString(), value);
}

QPushButton* addHelpButton(
    QVBoxLayout* layout,
    QWidget* parent,
    QWidget* leadingWidget = nullptr)
{
    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    if (leadingWidget != nullptr) {
        row->addWidget(leadingWidget);
    }
    auto* button = new QPushButton(parent);
    button->setAutoDefault(false);
    button->setIcon(
        parent->style()->standardIcon(QStyle::SP_DialogHelpButton));
    button->setMaximumWidth(130);
    row->addStretch();
    row->addWidget(button);
    layout->addLayout(row);
    return button;
}

#ifdef Q_OS_WIN
bool localizedKeyToWindowsVirtualKey(Qt::Key key, UINT& virtualKey)
{
    const int keyValue = static_cast<int>(key);
    if (keyValue <= 0 || keyValue > 0xFFFF) {
        return false;
    }

    const wchar_t character = static_cast<wchar_t>(keyValue);
    const auto tryLayout = [character, &virtualKey](HKL layout) {
        if (layout == nullptr) {
            return false;
        }
        const SHORT mapping = VkKeyScanExW(character, layout);
        if (mapping == -1) {
            return false;
        }
        virtualKey = static_cast<UINT>(LOBYTE(mapping));
        return virtualKey != 0 && virtualKey != 0xFF;
    };

    const HKL activeLayout = GetKeyboardLayout(0);
    if (tryLayout(activeLayout)) {
        return true;
    }

    const int layoutCount = GetKeyboardLayoutList(0, nullptr);
    if (layoutCount <= 0) {
        return false;
    }
    std::vector<HKL> layouts(static_cast<std::size_t>(layoutCount));
    const int copiedCount = GetKeyboardLayoutList(layoutCount, layouts.data());
    for (int index = 0; index < copiedCount; ++index) {
        if (layouts[static_cast<std::size_t>(index)] != activeLayout
            && tryLayout(layouts[static_cast<std::size_t>(index)])) {
            return true;
        }
    }
    return false;
}

bool toWindowsHotkey(
    const QKeySequence& sequence,
    UINT& nativeModifiers,
    UINT& virtualKey)
{
    if (sequence.count() != 1 || sequence.isEmpty()) {
        return false;
    }

    const QKeyCombination combination = sequence[0];
    const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers();
    const Qt::Key key = combination.key();
    constexpr Qt::KeyboardModifiers allowedModifiers =
        Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier;
    if ((modifiers & allowedModifiers) == Qt::NoModifier
        || (modifiers & ~allowedModifiers) != Qt::NoModifier) {
        return false;
    }

    nativeModifiers = MOD_NOREPEAT;
    if (modifiers.testFlag(Qt::ControlModifier)) {
        nativeModifiers |= MOD_CONTROL;
    }
    if (modifiers.testFlag(Qt::AltModifier)) {
        nativeModifiers |= MOD_ALT;
    }
    if (modifiers.testFlag(Qt::ShiftModifier)) {
        nativeModifiers |= MOD_SHIFT;
    }
    if (modifiers.testFlag(Qt::MetaModifier)) {
        nativeModifiers |= MOD_WIN;
    }

    const int keyValue = static_cast<int>(key);
    if ((key >= Qt::Key_A && key <= Qt::Key_Z)
        || (key >= Qt::Key_0 && key <= Qt::Key_9)) {
        virtualKey = static_cast<UINT>(keyValue);
        return true;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        virtualKey = VK_F1 + static_cast<UINT>(keyValue - static_cast<int>(Qt::Key_F1));
        return true;
    }

    switch (key) {
    case Qt::Key_Space:
        virtualKey = VK_SPACE;
        return true;
    case Qt::Key_Tab:
        virtualKey = VK_TAB;
        return true;
    case Qt::Key_Backspace:
        virtualKey = VK_BACK;
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        virtualKey = VK_RETURN;
        return true;
    case Qt::Key_Insert:
        virtualKey = VK_INSERT;
        return true;
    case Qt::Key_Delete:
        virtualKey = VK_DELETE;
        return true;
    case Qt::Key_Home:
        virtualKey = VK_HOME;
        return true;
    case Qt::Key_End:
        virtualKey = VK_END;
        return true;
    case Qt::Key_PageUp:
        virtualKey = VK_PRIOR;
        return true;
    case Qt::Key_PageDown:
        virtualKey = VK_NEXT;
        return true;
    case Qt::Key_Left:
        virtualKey = VK_LEFT;
        return true;
    case Qt::Key_Right:
        virtualKey = VK_RIGHT;
        return true;
    case Qt::Key_Up:
        virtualKey = VK_UP;
        return true;
    case Qt::Key_Down:
        virtualKey = VK_DOWN;
        return true;
    default:
        // QKeySequenceEdit stores the character produced by the active
        // keyboard layout. For example, the physical A key becomes Ф on a
        // Russian layout. Resolve it back to a Windows virtual key so global
        // shortcuts work independently of the display language/layout.
        return localizedKeyToWindowsVirtualKey(key, virtualKey);
    }
}
#endif

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , settings_()
{
    qtTranslator_ = new QTranslator(this);
    setWindowIcon(qApp->windowIcon());
    setMinimumSize(680, 500);
    resize(760, 600);

    buildUi();
    buildTray();
    captureWorker_ = new capture::ScreenCaptureWorker(this);
    audioWorker_ = new audio::AudioCaptureWorker(this);
    transportWorker_ = new transport::SerialTransportWorker(this);
    refreshCaptureOutputs();
    refreshAudioDevices();
    loadSettings();
    connectSignals();
    refreshSerialPorts();

    qApp->installNativeEventFilter(this);
    applyHotkey(hotkeyEdit_->keySequence());
    updateLedSummary();
    updateEnabledState();
    updateDiagnostics();
    updateCaptureConfiguration();
    updateAudioConfiguration();
    synchronizeDeviceConfiguration();
    updateTransportConfiguration();
    updateWindowSizeForCurrentTab();
}

MainWindow::~MainWindow()
{
    stopCapture();
    delete mirrorWindow_;
    stopAudio();
    stopTransport();
    qApp->removeNativeEventFilter(this);
    unregisterGlobalHotkey();
}

void MainWindow::buildUi()
{
    centralWidget_ = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(centralWidget_);

    tabs_ = new QTabWidget(centralWidget_);
    rootLayout->addWidget(tabs_, 1);

    buildGeneralTab();
    buildScreenTab();
    buildMirrorTab();
    buildAudioTab();
    buildLedTab();
    buildConnectionTab();
    buildDiagnosticsTab();

    auto* buttons = new QHBoxLayout();
    defaultsButton_ = new QPushButton(centralWidget_);
    saveButton_ = new QPushButton(centralWidget_);
    quitButton_ = new QPushButton(centralWidget_);
    buttons->addWidget(defaultsButton_);
    buttons->addStretch();
    buttons->addWidget(saveButton_);
    buttons->addWidget(quitButton_);
    rootLayout->addLayout(buttons);

    setCentralWidget(centralWidget_);

    connectionShortcutButton_ = new QPushButton(this);
    connectionShortcutButton_->setFlat(true);
    connectionShortcutButton_->setCursor(Qt::PointingHandCursor);
    statusBar()->addPermanentWidget(connectionShortcutButton_);

    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(300);

    captureUpdateTimer_ = new QTimer(this);
    captureUpdateTimer_->setSingleShot(true);
    captureUpdateTimer_->setInterval(120);

    audioUpdateTimer_ = new QTimer(this);
    audioUpdateTimer_->setSingleShot(true);
    audioUpdateTimer_->setInterval(120);

    transportUpdateTimer_ = new QTimer(this);
    transportUpdateTimer_->setSingleShot(true);
    transportUpdateTimer_->setInterval(120);

    layoutTestRefreshTimer_ = new QTimer(this);
    layoutTestRefreshTimer_->setTimerType(Qt::PreciseTimer);
    layoutTestRefreshTimer_->setInterval(50);

    previewRefreshTimer_ = new QTimer(this);
    previewRefreshTimer_->setSingleShot(true);
    previewRefreshTimer_->setTimerType(Qt::PreciseTimer);
    previewRefreshTimer_->setInterval(50);
}

void MainWindow::buildGeneralTab()
{
    generalPage_ = new QWidget(tabs_);
    auto* layout = new QVBoxLayout(generalPage_);
    generalHelpButton_ = addHelpButton(layout, generalPage_);

    generalSettingsGroup_ = new QGroupBox(generalPage_);
    auto* form = new QFormLayout(generalSettingsGroup_);
    languageLabel_ = makeFieldLabel(generalSettingsGroup_);
    languageCombo_ = new QComboBox(generalSettingsGroup_);
    languageCombo_->addItem(QStringLiteral("Русский"), QStringLiteral("ru"));
    languageCombo_->addItem(QStringLiteral("English"), QStringLiteral("en"));
    languageCombo_->setMaximumWidth(180);
    form->addRow(languageLabel_, languageCombo_);

    lightingEnabledCheck_ = new QCheckBox(generalSettingsGroup_);
    form->addRow(QString(), lightingEnabledCheck_);
    autostartCheck_ = new QCheckBox(generalSettingsGroup_);
    form->addRow(QString(), autostartCheck_);
    closeToTrayCheck_ = new QCheckBox(generalSettingsGroup_);
    form->addRow(QString(), closeToTrayCheck_);

    hotkeyLabel_ = makeFieldLabel(generalSettingsGroup_);
    hotkeyEdit_ = new QKeySequenceEdit(generalSettingsGroup_);
    hotkeyEdit_->setMaximumSequenceLength(1);
    hotkeyEdit_->setMaximumWidth(260);
    form->addRow(hotkeyLabel_, hotkeyEdit_);

    hotkeyHintLabel_ = new QLabel(generalSettingsGroup_);
    hotkeyHintLabel_->setWordWrap(true);
    form->addRow(QString(), hotkeyHintLabel_);

    hotkeyStatusLabel_ = new QLabel(generalSettingsGroup_);
    hotkeyStatusLabel_->setWordWrap(true);
    form->addRow(QString(), hotkeyStatusLabel_);

    layout->addWidget(generalSettingsGroup_);

    lightingModeGroup_ = new QGroupBox(generalPage_);
    auto* modeForm = new QFormLayout(lightingModeGroup_);

    activeModeLabel_ = makeFieldLabel(lightingModeGroup_);
    activeModeCombo_ = new QComboBox(lightingModeGroup_);
    for (const auto& value :
        {"ambilight", "equalizer", "solid", "breathing", "rainbow", "wave", "off"}) {
        addComboItem(activeModeCombo_, QString::fromLatin1(value));
    }
    activeModeCombo_->setMaximumWidth(260);
    modeForm->addRow(activeModeLabel_, activeModeCombo_);

    brightnessLabel_ = makeFieldLabel(lightingModeGroup_);
    brightnessSpin_ =
        makeSpinBox(lightingModeGroup_, 0, 100, QStringLiteral(" %"));
    brightnessSpin_->setMaximumWidth(140);
    modeForm->addRow(brightnessLabel_, brightnessSpin_);

    standaloneEffectLabel_ = makeFieldLabel(lightingModeGroup_);
    standaloneEffectCombo_ = new QComboBox(lightingModeGroup_);
    for (const auto& value :
        {"solid", "breathing", "rainbow", "wave", "off"}) {
        addComboItem(standaloneEffectCombo_, QString::fromLatin1(value));
    }
    standaloneEffectCombo_->setMaximumWidth(260);
    modeForm->addRow(standaloneEffectLabel_, standaloneEffectCombo_);

    solidColorLabel_ = makeFieldLabel(lightingModeGroup_);
    solidColorButton_ = new QPushButton(lightingModeGroup_);
    solidColorButton_->setMaximumWidth(140);
    modeForm->addRow(solidColorLabel_, solidColorButton_);

    manualAnimationButton_ = new QPushButton(lightingModeGroup_);
    manualAnimationButton_->setCheckable(true);
    modeForm->addRow(QString(), manualAnimationButton_);

    standaloneHintLabel_ = new QLabel(lightingModeGroup_);
    standaloneHintLabel_->setWordWrap(true);
    modeForm->addRow(QString(), standaloneHintLabel_);

    layout->addWidget(lightingModeGroup_);
    layout->addStretch();
    tabs_->addTab(generalPage_, QString());
}

void MainWindow::buildScreenTab()
{
    screenPage_ = new QWidget(tabs_);
    auto* layout = new QVBoxLayout(screenPage_);
    screenHelpButton_ = addHelpButton(layout, screenPage_);

    auto* contentLayout = new QHBoxLayout();
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(12);

    auto* settingsPanel = new QWidget(screenPage_);
    settingsPanel->setMinimumWidth(400);
    settingsPanel->setMaximumWidth(500);
    settingsPanel->setSizePolicy(
        QSizePolicy::Preferred,
        QSizePolicy::Expanding);
    auto* settingsLayout = new QVBoxLayout(settingsPanel);
    settingsLayout->setContentsMargins(0, 0, 0, 0);

    screenSourceGroup_ = new QGroupBox(settingsPanel);
    auto* sourceForm = new QFormLayout(screenSourceGroup_);
    sourceForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    captureOutputLabel_ = makeFieldLabel(screenSourceGroup_);
    auto* outputLayout = new QHBoxLayout();
    captureOutputCombo_ = new QComboBox(screenSourceGroup_);
    captureOutputCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    captureOutputCombo_->setMinimumContentsLength(28);
    refreshOutputsButton_ = new QPushButton(screenSourceGroup_);
    outputLayout->addWidget(captureOutputCombo_, 1);
    outputLayout->addWidget(refreshOutputsButton_);
    sourceForm->addRow(captureOutputLabel_, outputLayout);

    captureMethodLabel_ = makeFieldLabel(screenSourceGroup_);
    captureMethodCombo_ = new QComboBox(screenSourceGroup_);
    addComboItem(captureMethodCombo_, QStringLiteral("dxgi"));
    addComboItem(captureMethodCombo_, QStringLiteral("wgc"));
    sourceForm->addRow(captureMethodLabel_, captureMethodCombo_);
    captureMethodHintLabel_ = new QLabel(screenSourceGroup_);
    captureMethodHintLabel_->setWordWrap(true);
    sourceForm->addRow(QString(), captureMethodHintLabel_);

    resolutionLabel_ = makeFieldLabel(screenSourceGroup_);
    auto* resolutionLayout = new QHBoxLayout();
    widthLabel_ = new QLabel(screenSourceGroup_);
    screenWidthSpin_ = makeSpinBox(screenSourceGroup_, 320, 16'384);
    screenWidthSpin_->setReadOnly(true);
    screenWidthSpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    screenWidthSpin_->setMaximumWidth(100);
    heightLabel_ = new QLabel(screenSourceGroup_);
    screenHeightSpin_ = makeSpinBox(screenSourceGroup_, 240, 16'384);
    screenHeightSpin_->setReadOnly(true);
    screenHeightSpin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    screenHeightSpin_->setMaximumWidth(100);
    resolutionLayout->addWidget(widthLabel_);
    resolutionLayout->addWidget(screenWidthSpin_);
    resolutionLayout->addSpacing(12);
    resolutionLayout->addWidget(heightLabel_);
    resolutionLayout->addWidget(screenHeightSpin_);
    resolutionLayout->addStretch();
    sourceForm->addRow(resolutionLabel_, resolutionLayout);

    fpsLabel_ = makeFieldLabel(screenSourceGroup_);
    fpsSpin_ =
        makeSpinBox(screenSourceGroup_, 1, 240, QStringLiteral(" FPS"));
    fpsSpin_->setMaximumWidth(110);
    sourceForm->addRow(fpsLabel_, fpsSpin_);

    gamingModeCheck_ = new QCheckBox(screenSourceGroup_);
    sourceForm->addRow(QString(), gamingModeCheck_);
    gamingModeHintLabel_ = new QLabel(screenSourceGroup_);
    gamingModeHintLabel_->setWordWrap(true);
    sourceForm->addRow(QString(), gamingModeHintLabel_);

    blackBarModeLabel_ = makeFieldLabel(screenSourceGroup_);
    blackBarModeCombo_ = new QComboBox(screenSourceGroup_);
    for (const auto& mode : {QStringLiteral("off"),
             QStringLiteral("first-content"),
             QStringLiteral("limit-21-9")}) {
        addComboItem(blackBarModeCombo_, mode);
    }
    blackBarModeCombo_->setMaximumWidth(390);
    sourceForm->addRow(blackBarModeLabel_, blackBarModeCombo_);

    settingsLayout->addWidget(screenSourceGroup_);

    screenColorGroup_ = new QGroupBox(settingsPanel);
    auto* colorForm = new QFormLayout(screenColorGroup_);
    colorForm->setRowWrapPolicy(QFormLayout::WrapLongRows);

    zonesLabel_ = makeFieldLabel(screenColorGroup_);
    zonesSpin_ = makeSpinBox(screenColorGroup_, 4, 300);
    zonesSpin_->setMaximumWidth(100);
    colorForm->addRow(zonesLabel_, zonesSpin_);

    regionColorModeLabel_ = makeFieldLabel(screenColorGroup_);
    regionColorModeCombo_ = new QComboBox(screenColorGroup_);
    for (const auto& mode : {QStringLiteral("mean"), QStringLiteral("rms"),
             QStringLiteral("dominant")}) {
        addComboItem(regionColorModeCombo_, mode);
    }
    regionColorModeCombo_->setMaximumWidth(390);
    colorForm->addRow(regionColorModeLabel_, regionColorModeCombo_);

    regionColorModeHintLabel_ = new QLabel(screenColorGroup_);
    regionColorModeHintLabel_->setWordWrap(true);
    colorForm->addRow(QString(), regionColorModeHintLabel_);

    ledTransferModeLabel_ = makeFieldLabel(screenColorGroup_);
    ledTransferModeCombo_ = new QComboBox(screenColorGroup_);
    for (const auto& mode : {QStringLiteral("raw"),
             QStringLiteral("soft-gamma"), QStringLiteral("srgb"),
             QStringLiteral("strong-gamma")}) {
        addComboItem(ledTransferModeCombo_, mode);
    }
    ledTransferModeCombo_->setMaximumWidth(390);
    colorForm->addRow(ledTransferModeLabel_, ledTransferModeCombo_);

    samplingDepthLabel_ = makeFieldLabel(screenColorGroup_);
    auto* samplingDepthLayout = new QHBoxLayout();
    samplingDepthAutoCheck_ = new QCheckBox(screenColorGroup_);
    samplingDepthSpin_ =
        makeSpinBox(screenColorGroup_, 8, 512, QStringLiteral(" px"));
    samplingDepthSpin_->setMaximumWidth(110);
    samplingDepthLayout->addWidget(samplingDepthAutoCheck_);
    samplingDepthLayout->addWidget(samplingDepthSpin_);
    samplingDepthLayout->addStretch();
    colorForm->addRow(samplingDepthLabel_, samplingDepthLayout);

    samplingDepthHintLabel_ = new QLabel(screenColorGroup_);
    samplingDepthHintLabel_->setWordWrap(true);
    colorForm->addRow(QString(), samplingDepthHintLabel_);

    smoothingLabel_ = makeFieldLabel(screenColorGroup_);
    smoothingCombo_ = new QComboBox(screenColorGroup_);
    for (const int milliseconds : {0, 25, 60, 120}) {
        smoothingCombo_->addItem(QString(), milliseconds);
    }
    smoothingCombo_->setMaximumWidth(310);
    colorForm->addRow(smoothingLabel_, smoothingCombo_);

    flashProtectionCheck_ = new QCheckBox(screenColorGroup_);
    colorForm->addRow(QString(), flashProtectionCheck_);

    temporalFilterHintLabel_ = new QLabel(screenColorGroup_);
    temporalFilterHintLabel_->setWordWrap(true);
    colorForm->addRow(QString(), temporalFilterHintLabel_);

    settingsLayout->addWidget(screenColorGroup_);
    settingsLayout->addStretch();

    ledPreview_ = new LedPreviewWidget(screenPage_);
    contentLayout->addWidget(settingsPanel);
    contentLayout->addWidget(ledPreview_, 1);
    contentLayout->setStretch(0, 0);
    contentLayout->setStretch(1, 1);
    layout->addLayout(contentLayout, 1);
    tabs_->addTab(screenPage_, QString());
}

void MainWindow::buildMirrorTab()
{
    mirrorPage_ = new QWidget(tabs_);
    auto* layout = new QVBoxLayout(mirrorPage_);
    mirrorHintLabel_ = new QLabel(mirrorPage_);
    mirrorHintLabel_->setWordWrap(true);
    layout->addWidget(mirrorHintLabel_);
    auto* form = new QFormLayout();
    mirrorSourceLabel_ = new QLabel(mirrorPage_);
    mirrorTargetLabel_ = new QLabel(mirrorPage_);
    mirrorMethodLabel_ = new QLabel(mirrorPage_);
    mirrorFpsLabel_ = new QLabel(mirrorPage_);
    mirrorSourceCombo_ = new QComboBox(mirrorPage_);
    mirrorTargetCombo_ = new QComboBox(mirrorPage_);
    mirrorMethodCombo_ = new QComboBox(mirrorPage_);
    mirrorMethodCombo_->addItem(QStringLiteral("DXGI Desktop Duplication"), QStringLiteral("dxgi"));
    mirrorMethodCombo_->addItem(QStringLiteral("Windows Graphics Capture"), QStringLiteral("wgc"));
    mirrorFpsSpin_ = makeSpinBox(mirrorPage_, 15, 60, QStringLiteral(" FPS"));
    mirrorFpsSpin_->setValue(60);
    form->addRow(mirrorSourceLabel_, mirrorSourceCombo_);
    form->addRow(mirrorTargetLabel_, mirrorTargetCombo_);
    form->addRow(mirrorMethodLabel_, mirrorMethodCombo_);
    form->addRow(mirrorFpsLabel_, mirrorFpsSpin_);
    layout->addLayout(form);
    auto* buttons = new QHBoxLayout();
    mirrorStartButton_ = new QPushButton(mirrorPage_);
    mirrorStartButton_->setCheckable(true);
    mirrorRefreshButton_ = new QPushButton(mirrorPage_);
    mirrorHelpButton_ = new QPushButton(mirrorPage_);
    buttons->addWidget(mirrorStartButton_);
    buttons->addWidget(mirrorRefreshButton_);
    buttons->addStretch();
    buttons->addWidget(mirrorHelpButton_);
    layout->addLayout(buttons);
    mirrorStatusLabel_ = new QLabel(mirrorPage_);
    mirrorStatusLabel_->setWordWrap(true);
    layout->addWidget(mirrorStatusLabel_);
    layout->addStretch();
    tabs_->addTab(mirrorPage_, QString());
    // An unowned top-level window stays visible when the settings window is
    // minimized/hidden to the tray. Explicitly destroyed after worker stop.
    mirrorWindow_ = new MirrorWindow(nullptr);
    mirrorWindow_->stopRequested = [this] { stopMirror(); };
}

void MainWindow::updateMirrorUi()
{
    tabs_->setTabText(tabs_->indexOf(mirrorPage_), text("Mirroring", "Зеркалирование"));
    mirrorHintLabel_->setText(text(
        "Experimental mirror without blur. Works without lighting or ESP32. Select Extend with Win+P first. "
        "Both displays must use the same GPU. Source and capture method are shared with Ambilight. "
        "Full-frame copies are needed even in Game mode. Extra GPU load and TV latency are possible. "
        "No automatic start or display-mode changes.",
        "Экспериментальное зеркало без размытия. Работает без подсветки и ESP32. Сначала выберите «Расширить» через Win+P. "
        "Оба экрана должны быть подключены к одной видеокарте. Источник и способ захвата общие с Ambilight. "
        "Копируется полный кадр даже в игровом режиме. Возможны дополнительная нагрузка и задержка на ТВ. "
        "Автозапуска и изменения раскладки Windows нет."));
    mirrorSourceLabel_->setText(text("Source:", "Источник:"));
    mirrorTargetLabel_->setText(text("Destination (TV):", "Назначение (ТВ):"));
    mirrorMethodLabel_->setText(text("Capture method:", "Способ захвата:"));
    mirrorFpsLabel_->setText(text("Mirror frame rate:", "Частота зеркала:"));
    mirrorStartButton_->setText(mirrorRunning_ ? text("Stop", "Остановить") : text("Start mirror", "Запустить зеркало"));
    mirrorRefreshButton_->setText(text("Refresh displays", "Обновить экраны"));
    mirrorHelpButton_->setText(text("Help", "Справка"));
    mirrorSourceCombo_->setEnabled(!mirrorRunning_);
    mirrorTargetCombo_->setEnabled(!mirrorRunning_);
    captureOutputCombo_->setEnabled(!mirrorRunning_);
    mirrorRefreshButton_->setEnabled(!mirrorRunning_);
    if (stopMirrorAction_) {
        stopMirrorAction_->setText(text("Stop TV mirror", "Остановить зеркало ТВ"));
        stopMirrorAction_->setEnabled(mirrorRunning_);
    }
    const QSignalBlocker sourceBlocker(mirrorSourceCombo_), methodBlocker(mirrorMethodCombo_);
    mirrorSourceCombo_->setCurrentIndex(captureOutputCombo_->currentIndex());
    selectComboValue(mirrorMethodCombo_, comboValue(captureMethodCombo_));
}

void MainWindow::toggleMirror(const bool enabled)
{
    if (!enabled) { stopMirror(); return; }
    const auto* source = capture::resolveCaptureOutput(captureOutputs_, comboValue(captureOutputCombo_));
    const auto* target = capture::resolveCaptureOutput(captureOutputs_, comboValue(mirrorTargetCombo_));
    if (!source || !target || !capture::separateMirrorOutputs(*source, *target)) {
        const QSignalBlocker blocker(mirrorStartButton_);
        mirrorStartButton_->setChecked(false);
        mirrorStatusLabel_->setText(text(
            "Select two different extended displays. Mirroring to the source is not allowed.",
            "Выберите два разных экрана в режиме «Расширить». Нельзя выводить зеркало на источник."));
        return;
    }
    // Freeze automatic selection to the current source while the mirror runs.
    const QString sourceId = source->id;
    const QRect targetRect = target->geometry;
    selectComboValue(captureOutputCombo_, sourceId);
    const HWND window = reinterpret_cast<HWND>(mirrorWindow_->winId());
    if (!SetWindowPos(window, HWND_TOPMOST, targetRect.x(), targetRect.y(),
            targetRect.width(), targetRect.height(), SWP_NOACTIVATE)) {
        mirrorWindow_->hide();
        const QSignalBlocker blocker(mirrorStartButton_);
        mirrorStartButton_->setChecked(false);
        mirrorStatusLabel_->setText(text("Cannot position the TV window.", "Не удалось разместить окно на ТВ."));
        return;
    }
    mirrorWindow_->show();
    SetWindowPos(window, HWND_TOPMOST, targetRect.x(), targetRect.y(),
        targetRect.width(), targetRect.height(), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    mirrorRunning_ = true;
    mirrorStatusLabel_->setText(text("Starting mirror…", "Запуск зеркала…"));
    updateMirrorUi();
    updateCaptureConfiguration();
}

void MainWindow::stopMirror()
{
    if (!mirrorRunning_) return;
    // Release the HWND's swap chain on its owning worker before hiding it.
    stopCapture();
    mirrorRunning_ = false;
    mirrorWindow_->hide();
    const QSignalBlocker blocker(mirrorStartButton_);
    mirrorStartButton_->setChecked(false);
    mirrorStatusLabel_->setText(text("Mirror stopped.", "Зеркало остановлено."));
    updateMirrorUi();
    if (!exiting_) updateCaptureConfiguration();
}

void MainWindow::buildAudioTab()
{
    audioPage_ = new QWidget(tabs_);
    auto* layout = new QVBoxLayout(audioPage_);
    auto* audioActions = new QWidget(audioPage_);
    auto* audioActionsLayout = new QHBoxLayout(audioActions);
    audioActionsLayout->setContentsMargins(0, 0, 0, 0);
    audioDefaultsButton_ = new QPushButton(audioActions);
    audioStartButton_ = new QPushButton(audioActions);
    audioActionsLayout->addWidget(audioDefaultsButton_);
    audioActionsLayout->addWidget(audioStartButton_);
    audioHelpButton_ = addHelpButton(
        layout, audioPage_, audioActions);

    auto* contentLayout = new QHBoxLayout();
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(12);

    auto* settingsPanel = new QWidget(audioPage_);
    settingsPanel->setMinimumWidth(430);
    settingsPanel->setMaximumWidth(560);
    auto* settingsLayout = new QVBoxLayout(settingsPanel);
    settingsLayout->setContentsMargins(0, 0, 0, 0);

    audioSourceGroup_ = new QGroupBox(settingsPanel);
    auto* sourceForm = new QFormLayout(audioSourceGroup_);
    sourceForm->setRowWrapPolicy(QFormLayout::WrapLongRows);

    audioSourceLabel_ = makeFieldLabel(audioSourceGroup_);
    audioSourceCombo_ = new QComboBox(audioSourceGroup_);
    for (const auto& source : {QStringLiteral("windows"),
             QStringLiteral("microphone"), QStringLiteral("mixed")}) {
        addComboItem(audioSourceCombo_, source);
    }
    audioSourceCombo_->setMaximumWidth(310);
    sourceForm->addRow(audioSourceLabel_, audioSourceCombo_);

    audioOutputDeviceLabel_ = makeFieldLabel(audioSourceGroup_);
    audioOutputDeviceCombo_ = new QComboBox(audioSourceGroup_);
    audioOutputDeviceCombo_->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    audioOutputDeviceCombo_->setMinimumContentsLength(28);
    sourceForm->addRow(audioOutputDeviceLabel_, audioOutputDeviceCombo_);

    audioInputDeviceLabel_ = makeFieldLabel(audioSourceGroup_);
    audioInputDeviceCombo_ = new QComboBox(audioSourceGroup_);
    audioInputDeviceCombo_->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    audioInputDeviceCombo_->setMinimumContentsLength(28);
    sourceForm->addRow(audioInputDeviceLabel_, audioInputDeviceCombo_);

    refreshAudioDevicesButton_ = new QPushButton(audioSourceGroup_);
    sourceForm->addRow(QString(), refreshAudioDevicesButton_);

    audioOutputGainLabel_ = makeFieldLabel(audioSourceGroup_);
    audioOutputGainSpin_ = makeSpinBox(
        audioSourceGroup_, 0, 200, QStringLiteral(" %"));
    audioOutputGainSpin_->setMaximumWidth(110);
    sourceForm->addRow(audioOutputGainLabel_, audioOutputGainSpin_);

    audioMicrophoneGainLabel_ = makeFieldLabel(audioSourceGroup_);
    audioMicrophoneGainSpin_ = makeSpinBox(
        audioSourceGroup_, 0, 200, QStringLiteral(" %"));
    audioMicrophoneGainSpin_->setMaximumWidth(110);
    sourceForm->addRow(audioMicrophoneGainLabel_, audioMicrophoneGainSpin_);
    settingsLayout->addWidget(audioSourceGroup_);

    audioVisualizationGroup_ = new QGroupBox(settingsPanel);
    auto* visualForm = new QFormLayout(audioVisualizationGroup_);
    visualForm->setRowWrapPolicy(QFormLayout::WrapLongRows);

    audioBandsLabel_ = makeFieldLabel(audioVisualizationGroup_);
    audioBandsSpin_ = makeSpinBox(audioVisualizationGroup_, 4, 64);
    audioBandsSpin_->setMaximumWidth(100);
    visualForm->addRow(audioBandsLabel_, audioBandsSpin_);

    audioFrequencyRangeLabel_ = makeFieldLabel(audioVisualizationGroup_);
    auto* frequencyRow = new QHBoxLayout();
    audioMinimumFrequencySpin_ = makeSpinBox(
        audioVisualizationGroup_, 20, 2'000, QStringLiteral(" Hz"));
    audioMaximumFrequencySpin_ = makeSpinBox(
        audioVisualizationGroup_, 1'000, 24'000, QStringLiteral(" Hz"));
    audioMinimumFrequencySpin_->setMaximumWidth(120);
    audioMaximumFrequencySpin_->setMaximumWidth(130);
    frequencyRow->addWidget(audioMinimumFrequencySpin_);
    frequencyRow->addWidget(new QLabel(QStringLiteral("–"), audioVisualizationGroup_));
    frequencyRow->addWidget(audioMaximumFrequencySpin_);
    frequencyRow->addStretch();
    visualForm->addRow(audioFrequencyRangeLabel_, frequencyRow);

    audioSensitivityLabel_ = makeFieldLabel(audioVisualizationGroup_);
    audioSensitivitySpin_ = makeSpinBox(
        audioVisualizationGroup_, 10, 400, QStringLiteral(" %"));
    audioSensitivitySpin_->setMaximumWidth(110);
    visualForm->addRow(audioSensitivityLabel_, audioSensitivitySpin_);

    audioNoiseGateLabel_ = makeFieldLabel(audioVisualizationGroup_);
    audioNoiseGateSpin_ = makeSpinBox(
        audioVisualizationGroup_, -100, -10, QStringLiteral(" dB"));
    audioNoiseGateSpin_->setMaximumWidth(110);
    visualForm->addRow(audioNoiseGateLabel_, audioNoiseGateSpin_);

    audioAttackLabel_ = makeFieldLabel(audioVisualizationGroup_);
    audioAttackSpin_ = makeSpinBox(
        audioVisualizationGroup_, 0, 500, QStringLiteral(" ms"));
    audioAttackSpin_->setMaximumWidth(110);
    visualForm->addRow(audioAttackLabel_, audioAttackSpin_);

    audioReleaseLabel_ = makeFieldLabel(audioVisualizationGroup_);
    audioReleaseSpin_ = makeSpinBox(
        audioVisualizationGroup_, 10, 2'000, QStringLiteral(" ms"));
    audioReleaseSpin_->setMaximumWidth(110);
    visualForm->addRow(audioReleaseLabel_, audioReleaseSpin_);

    audioDynamicsLabel_ = makeFieldLabel(audioVisualizationGroup_);
    auto* dynamicsRow = new QHBoxLayout();
    audioDynamicsCombo_ = new QComboBox(audioVisualizationGroup_);
    addComboItem(audioDynamicsCombo_, QStringLiteral("smooth"));
    addComboItem(audioDynamicsCombo_, QStringLiteral("spring"));
    audioDynamicsCombo_->setMaximumWidth(230);
    dynamicsRow->addWidget(audioDynamicsCombo_);

    audioSpringinessLabel_ = makeFieldLabel(audioVisualizationGroup_);
    audioSpringinessSpin_ = makeSpinBox(
        audioVisualizationGroup_, 0, 100, QStringLiteral(" %"));
    audioSpringinessSpin_->setMaximumWidth(110);
    dynamicsRow->addWidget(audioSpringinessLabel_);
    dynamicsRow->addWidget(audioSpringinessSpin_);
    dynamicsRow->addStretch();
    visualForm->addRow(audioDynamicsLabel_, dynamicsRow);

    audioLayoutLabel_ = makeFieldLabel(audioVisualizationGroup_);
    audioLayoutCombo_ = new QComboBox(audioVisualizationGroup_);
    addComboItem(audioLayoutCombo_, QStringLiteral("mirrored"));
    addComboItem(audioLayoutCombo_, QStringLiteral("sequential"));
    audioLayoutCombo_->setMaximumWidth(310);
    visualForm->addRow(audioLayoutLabel_, audioLayoutCombo_);

    audioColorModeLabel_ = makeFieldLabel(audioVisualizationGroup_);
    auto* colorRow = new QHBoxLayout();
    audioColorModeCombo_ = new QComboBox(audioVisualizationGroup_);
    addComboItem(audioColorModeCombo_, QStringLiteral("spectrum"));
    addComboItem(audioColorModeCombo_, QStringLiteral("solid"));
    audioSolidColorButton_ = new QPushButton(audioVisualizationGroup_);
    audioSolidColorButton_->setMaximumWidth(90);
    colorRow->addWidget(audioColorModeCombo_, 1);
    colorRow->addWidget(audioSolidColorButton_);
    visualForm->addRow(audioColorModeLabel_, colorRow);

    audioHintLabel_ = new QLabel(audioVisualizationGroup_);
    audioHintLabel_->setWordWrap(true);
    visualForm->addRow(QString(), audioHintLabel_);
    settingsLayout->addWidget(audioVisualizationGroup_);
    settingsLayout->addStretch();

    auto* previewPanel = new QWidget(audioPage_);
    auto* previewLayout = new QVBoxLayout(previewPanel);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    audioStatusLabel_ = new QLabel(previewPanel);
    audioStatusLabel_->setWordWrap(true);
    audioLevelMeter_ = new QProgressBar(previewPanel);
    audioLevelMeter_->setRange(0, 100);
    audioLevelMeter_->setValue(0);
    audioLevelMeter_->setMaximumHeight(24);
    audioPreview_ = new LedPreviewWidget(previewPanel);
    previewLayout->addWidget(audioStatusLabel_);
    previewLayout->addWidget(audioLevelMeter_);
    previewLayout->addWidget(audioPreview_, 1);

    contentLayout->addWidget(settingsPanel);
    contentLayout->addWidget(previewPanel, 1);
    layout->addLayout(contentLayout, 1);
    tabs_->addTab(audioPage_, QString());
}

void MainWindow::buildLedTab()
{
    ledPage_ = new QWidget(tabs_);
    auto* layout = new QVBoxLayout(ledPage_);
    ledHelpButton_ = addHelpButton(layout, ledPage_);
    auto* form = new QFormLayout();

    ledTypeLabel_ = makeFieldLabel(ledPage_);
    ledTypeCombo_ = new QComboBox(ledPage_);
    ledTypeCombo_->addItem(QStringLiteral("WS2815"), QStringLiteral("WS2815"));
    ledTypeCombo_->setMaximumWidth(180);
    form->addRow(ledTypeLabel_, ledTypeCombo_);

    voltageHintLabel_ = new QLabel(ledPage_);
    voltageHintLabel_->setWordWrap(true);
    form->addRow(QString(), voltageHintLabel_);

    colorOrderLabel_ = makeFieldLabel(ledPage_);
    colorOrderCombo_ = new QComboBox(ledPage_);
    for (const auto& order : {QStringLiteral("RGB"), QStringLiteral("RBG"),
             QStringLiteral("GRB"), QStringLiteral("GBR"),
             QStringLiteral("BRG"), QStringLiteral("BGR")}) {
        colorOrderCombo_->addItem(order, order);
    }
    colorOrderCombo_->setMaximumWidth(180);
    form->addRow(colorOrderLabel_, colorOrderCombo_);

    topLedsLabel_ = makeFieldLabel(ledPage_);
    topLedsSpin_ = makeSpinBox(ledPage_, 0, 300);
    topLedsSpin_->setMaximumWidth(110);
    form->addRow(topLedsLabel_, topLedsSpin_);
    rightLedsLabel_ = makeFieldLabel(ledPage_);
    rightLedsSpin_ = makeSpinBox(ledPage_, 0, 300);
    rightLedsSpin_->setMaximumWidth(110);
    form->addRow(rightLedsLabel_, rightLedsSpin_);
    bottomLedsLabel_ = makeFieldLabel(ledPage_);
    bottomLedsSpin_ = makeSpinBox(ledPage_, 0, 300);
    bottomLedsSpin_->setMaximumWidth(110);
    form->addRow(bottomLedsLabel_, bottomLedsSpin_);
    leftLedsLabel_ = makeFieldLabel(ledPage_);
    leftLedsSpin_ = makeSpinBox(ledPage_, 0, 300);
    leftLedsSpin_->setMaximumWidth(110);
    form->addRow(leftLedsLabel_, leftLedsSpin_);

    totalLedsLabel_ = new QLabel(ledPage_);
    totalLedsLabel_->setStyleSheet(QStringLiteral("font-weight: 600;"));
    form->addRow(QString(), totalLedsLabel_);

    offsetLabel_ = makeFieldLabel(ledPage_);
    ledOffsetSpin_ = makeSpinBox(ledPage_, 0, 299);
    ledOffsetSpin_->setMaximumWidth(110);
    form->addRow(offsetLabel_, ledOffsetSpin_);

    directionLabel_ = makeFieldLabel(ledPage_);
    directionCombo_ = new QComboBox(ledPage_);
    addComboItem(directionCombo_, QStringLiteral("clockwise"));
    addComboItem(directionCombo_, QStringLiteral("counterclockwise"));
    directionCombo_->setMaximumWidth(240);
    form->addRow(directionLabel_, directionCombo_);

    currentLimitLabel_ = makeFieldLabel(ledPage_);
    currentLimitSpin_ = makeSpinBox(ledPage_, 100, 10'000, QStringLiteral(" mA"));
    currentLimitSpin_->setSingleStep(100);
    currentLimitSpin_->setMaximumWidth(140);
    form->addRow(currentLimitLabel_, currentLimitSpin_);

    outputLimitsHintLabel_ = new QLabel(ledPage_);
    outputLimitsHintLabel_->setWordWrap(true);
    form->addRow(QString(), outputLimitsHintLabel_);

    layout->addLayout(form);

    auto* layoutTestRow = new QHBoxLayout();
    layoutTestButton_ = new QPushButton(ledPage_);
    layoutTestButton_->setCheckable(true);
    ledTestHintLabel_ = new QLabel(ledPage_);
    ledTestHintLabel_->setWordWrap(true);
    layoutTestRow->addWidget(layoutTestButton_);
    layoutTestRow->addWidget(ledTestHintLabel_, 1);
    layout->addLayout(layoutTestRow);

    whiteBalanceGroup_ = new QGroupBox(ledPage_);
    auto* whiteBalanceForm = new QFormLayout(whiteBalanceGroup_);
    const auto addCalibrationRow = [this, whiteBalanceForm](
                                       QLabel*& label,
                                       QSlider*& slider,
                                       QSpinBox*& spin) {
        label = makeFieldLabel(whiteBalanceGroup_);
        auto* row = new QHBoxLayout();
        slider = new QSlider(Qt::Horizontal, whiteBalanceGroup_);
        slider->setRange(0, 100);
        slider->setSingleStep(1);
        slider->setPageStep(5);
        slider->setMinimumWidth(180);
        spin = makeSpinBox(
            whiteBalanceGroup_, 0, 100, QStringLiteral(" %"));
        spin->setMaximumWidth(100);
        row->addWidget(slider, 1);
        row->addWidget(spin);
        whiteBalanceForm->addRow(label, row);
        connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
        connect(spin, &QSpinBox::valueChanged, slider, &QSlider::setValue);
    };
    addCalibrationRow(
        redCalibrationLabel_, redCalibrationSlider_, redCalibrationSpin_);
    addCalibrationRow(
        greenCalibrationLabel_, greenCalibrationSlider_, greenCalibrationSpin_);
    addCalibrationRow(
        blueCalibrationLabel_, blueCalibrationSlider_, blueCalibrationSpin_);
    auto* whiteBalanceButtons = new QHBoxLayout();
    testWhiteBalanceButton_ = new QPushButton(whiteBalanceGroup_);
    testWhiteBalanceButton_->setCheckable(true);
    resetWhiteBalanceButton_ = new QPushButton(whiteBalanceGroup_);
    whiteBalanceButtons->addWidget(testWhiteBalanceButton_);
    whiteBalanceButtons->addWidget(resetWhiteBalanceButton_);
    whiteBalanceButtons->addStretch();
    whiteBalanceForm->addRow(QString(), whiteBalanceButtons);
    layout->addWidget(whiteBalanceGroup_);
    layout->addStretch();
    tabs_->addTab(ledPage_, QString());
}

void MainWindow::buildConnectionTab()
{
    connectionPage_ = new QWidget(tabs_);
    auto* layout = new QVBoxLayout(connectionPage_);
    connectionHelpButton_ = addHelpButton(layout, connectionPage_);
    auto* form = new QFormLayout();

    portLabel_ = makeFieldLabel(connectionPage_);
    auto* portLayout = new QHBoxLayout();
    portCombo_ = new QComboBox(connectionPage_);
    portCombo_->setEditable(true);
    portCombo_->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    portCombo_->setMinimumContentsLength(22);
    portCombo_->addItem(QString(), QString {});
    refreshPortsButton_ = new QPushButton(connectionPage_);
    portLayout->addWidget(portCombo_, 1);
    portLayout->addWidget(refreshPortsButton_);
    form->addRow(portLabel_, portLayout);

    baudLabel_ = makeFieldLabel(connectionPage_);
    baudCombo_ = new QComboBox(connectionPage_);
    baudCombo_->addItem(
        QStringLiteral("2 000 000"),
        kFirmwareBaudRate);
    baudCombo_->setMaximumWidth(180);
    form->addRow(baudLabel_, baudCombo_);

    ambientTimeoutLabel_ = makeFieldLabel(connectionPage_);
    ambientTimeoutSpin_ =
        makeSpinBox(connectionPage_, 100, 60'000, QStringLiteral(" ms"));
    ambientTimeoutSpin_->setSingleStep(100);
    ambientTimeoutSpin_->setMaximumWidth(180);
    form->addRow(ambientTimeoutLabel_, ambientTimeoutSpin_);

    connectionStatusLabel_ = new QLabel(connectionPage_);
    connectionStatusLabel_->setWordWrap(true);
    form->addRow(QString(), connectionStatusLabel_);

    layout->addLayout(form);
    layout->addStretch();
    tabs_->addTab(connectionPage_, QString());
}

void MainWindow::buildDiagnosticsTab()
{
    diagnosticsPage_ = new QWidget(tabs_);
    auto* layout = new QVBoxLayout(diagnosticsPage_);
    diagnosticsHelpButton_ = addHelpButton(layout, diagnosticsPage_);
    diagnosticsText_ = new QPlainTextEdit(diagnosticsPage_);
    diagnosticsText_->setReadOnly(true);
    diagnosticsText_->setLineWrapMode(QPlainTextEdit::NoWrap);
    copyDiagnosticsButton_ = new QPushButton(diagnosticsPage_);
    layout->addWidget(diagnosticsText_, 1);
    layout->addWidget(copyDiagnosticsButton_, 0, Qt::AlignRight);
    tabs_->addTab(diagnosticsPage_, QString());
}

void MainWindow::buildTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        closeToTrayCheck_->setChecked(false);
        closeToTrayCheck_->setEnabled(false);
        return;
    }

    trayMenu_ = new QMenu(this);
    showAction_ = trayMenu_->addAction(QString());
    toggleLightingAction_ = trayMenu_->addAction(QString());
    stopMirrorAction_ = trayMenu_->addAction(QString());
    trayMenu_->addSeparator();
    quitAction_ = trayMenu_->addAction(QString());

    trayIcon_ = new QSystemTrayIcon(windowIcon(), this);
    trayIcon_->setContextMenu(trayMenu_);
    trayIcon_->show();
}

void MainWindow::connectSignals()
{
    connect(mirrorStartButton_, &QPushButton::toggled, this, &MainWindow::toggleMirror);
    connect(mirrorRefreshButton_, &QPushButton::clicked, this, &MainWindow::refreshCaptureOutputs);
    connect(mirrorSourceCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!mirrorRunning_) captureOutputCombo_->setCurrentIndex(index);
    });
    connect(captureOutputCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::updateMirrorUi);
    connect(mirrorMethodCombo_, &QComboBox::currentIndexChanged, this, [this] {
        selectComboValue(captureMethodCombo_, comboValue(mirrorMethodCombo_));
    });
    connect(captureMethodCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::updateMirrorUi);
    connect(mirrorFpsSpin_, &QSpinBox::valueChanged, this, [this] {
        scheduleCaptureUpdate(); saveTimer_->start();
    });
    connect(mirrorTargetCombo_, &QComboBox::currentIndexChanged, saveTimer_, qOverload<>(&QTimer::start));
    connect(mirrorHelpButton_, &QPushButton::clicked, this, [this] {
        QMessageBox::information(this, text("TV mirror", "Зеркало ТВ"), text(
            "1. Win+P > Extend.\n2. Select the source monitor and destination TV.\n3. Start the mirror; lighting may stay off.\n"
            "If a game is not captured, try WGC. Keep the game on the source display.\n"
            "This first version preserves aspect ratio with black bars (no blur), SDR only; the separate system cursor is not drawn. "
            "Stop here or from the tray. Closing the application stops the mirror. "
            "Display topology changes stop it for safety. Compare game FPS and TV delay against Windows Duplicate.",
            "1. Win+P → Расширить.\n2. Выберите монитор-источник и ТВ-назначение.\n3. Запустите зеркало; подсветку можно выключить.\n"
            "Если игра не захватывается, попробуйте WGC. Игра должна оставаться на экране-источнике.\n"
            "Первая версия сохраняет пропорции с чёрными полосами (без размытия), только SDR; отдельный системный курсор не выводится. "
            "Остановка — здесь или через трей. Выход из приложения завершает зеркало. "
            "Изменение конфигурации экранов останавливает его для безопасности. Сравните FPS игры и задержку на ТВ с режимом «Дублировать»."));
    });
    if (stopMirrorAction_) connect(stopMirrorAction_, &QAction::triggered, this, &MainWindow::stopMirror);
    connect(captureWorker_, &capture::ScreenCaptureWorker::mirrorStatistics, this,
        [this](quint64 presented, quint64 skipped, double milliseconds) {
            if (mirrorRunning_) mirrorStatusLabel_->setText(text(
                "Presented: %1 · skipped (output busy): %2 · last capture/processing: %3 ms (not TV latency)",
                "Выведено: %1 · пропущено (вывод занят): %2 · последний захват/обработка: %3 мс (не задержка ТВ)")
                .arg(presented).arg(skipped).arg(milliseconds, 0, 'f', 2));
        });
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        updateWindowSizeForCurrentTab();
        updatePreviewFromLastCapture();
        updateDiagnostics();
    });
    connect(previewRefreshTimer_, &QTimer::timeout,
        this, &MainWindow::updatePreviewFromLastCapture);

    const auto connectHelp =
        [this](QPushButton* button, QWidget* page) {
            connect(button, &QPushButton::clicked, this, [this, page] {
                showTabHelp(page);
            });
        };
    connectHelp(generalHelpButton_, generalPage_);
    connectHelp(screenHelpButton_, screenPage_);
    connectHelp(audioHelpButton_, audioPage_);
    connectHelp(ledHelpButton_, ledPage_);
    connectHelp(connectionHelpButton_, connectionPage_);
    connectHelp(diagnosticsHelpButton_, diagnosticsPage_);

    connect(saveTimer_, &QTimer::timeout, this, [this] {
        saveSettings(false);
    });
    connect(captureUpdateTimer_, &QTimer::timeout,
        this, &MainWindow::updateCaptureConfiguration);
    connect(audioUpdateTimer_, &QTimer::timeout,
        this, &MainWindow::updateAudioConfiguration);
    connect(transportUpdateTimer_, &QTimer::timeout, this, [this] {
        synchronizeDeviceConfiguration();
        updateTransportConfiguration();
    });
    connect(layoutTestRefreshTimer_, &QTimer::timeout,
        this, &MainWindow::submitLayoutTestFrame);
    connect(saveButton_, &QPushButton::clicked, this, [this] {
        saveSettings(true);
    });
    connect(defaultsButton_, &QPushButton::clicked, this, &MainWindow::restoreDefaults);
    connect(quitButton_, &QPushButton::clicked, this, &MainWindow::exitApplication);
    connect(connectionShortcutButton_, &QPushButton::clicked, this, [this] {
        tabs_->setCurrentWidget(connectionPage_);
        showWindow();
    });
    connect(copyDiagnosticsButton_, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(diagnosticsText_->toPlainText());
        statusBar()->showMessage(text("Diagnostics copied.", "Диагностика скопирована."), 2500);
    });

    connect(languageCombo_, &QComboBox::currentIndexChanged, this, [this] {
        setLanguage(comboValue(languageCombo_) == QStringLiteral("en")
                ? Language::English
                : Language::Russian);
        scheduleSave();
    });

    connect(lightingEnabledCheck_, &QCheckBox::toggled, this, [this] {
        if (!lightingEnabledCheck_->isChecked()
            && manualAnimationButton_->isChecked()) {
            manualAnimationButton_->setChecked(false);
        }
        updateEnabledState();
        updateManualAnimationUi();
        updateDiagnostics();
        scheduleCaptureUpdate();
        scheduleAudioUpdate();
        scheduleTransportUpdate();
        scheduleSave();
    });

    connect(refreshOutputsButton_, &QPushButton::clicked,
        this, &MainWindow::refreshCaptureOutputs);
    connect(captureOutputCombo_, &QComboBox::currentIndexChanged, this, [this] {
        clearCapturedFrame(!audioShouldBeActive());
        const QSize outputSize =
            captureOutputCombo_->currentData(kCaptureOutputSizeRole).toSize();
        if (outputSize.isValid()) {
            const QSignalBlocker widthBlocker(screenWidthSpin_);
            const QSignalBlocker heightBlocker(screenHeightSpin_);
            screenWidthSpin_->setValue(outputSize.width());
            screenHeightSpin_->setValue(outputSize.height());
        }
        updateSamplingDepthUi();
        updateDiagnostics();
        scheduleCaptureUpdate();
        scheduleSave();
    });

    connect(autostartCheck_, &QCheckBox::toggled, this, [this](const bool enabled) {
        QString error;
        if (!autostart::setEnabled(enabled, &error)) {
            const QSignalBlocker blocker(autostartCheck_);
            autostartCheck_->setChecked(!enabled);
            QMessageBox::warning(
                this,
                windowTitle(),
                text("Could not update Windows autostart.",
                    "Не удалось изменить автозапуск Windows.")
                    + QStringLiteral("\n")
                    + error);
            return;
        }
        scheduleSave();
        updateDiagnostics();
    });

    connect(closeToTrayCheck_, &QCheckBox::toggled, this, &MainWindow::scheduleSave);
    // keySequenceChanged is also emitted for intermediate states while the
    // user is still holding or replacing keys. Registering such a partial
    // sequence makes validation fail and resets the editor. Wait until Qt
    // declares the single shortcut complete after key release instead.
    connect(hotkeyEdit_, &QKeySequenceEdit::editingFinished, this, [this] {
        applyHotkey(hotkeyEdit_->keySequence());
    });

    const auto connectSpin = [this](QSpinBox* spin) {
        connect(spin, &QSpinBox::valueChanged, this, [this] {
            updateDiagnostics();
            scheduleSave();
        });
    };
    for (auto* spin : {screenWidthSpin_, screenHeightSpin_, fpsSpin_, zonesSpin_,
             samplingDepthSpin_, ledOffsetSpin_, brightnessSpin_,
             currentLimitSpin_, redCalibrationSpin_, greenCalibrationSpin_,
             blueCalibrationSpin_, ambientTimeoutSpin_, audioOutputGainSpin_,
             audioMicrophoneGainSpin_, audioBandsSpin_,
             audioMinimumFrequencySpin_, audioMaximumFrequencySpin_,
             audioSensitivitySpin_, audioNoiseGateSpin_, audioAttackSpin_,
             audioReleaseSpin_, audioSpringinessSpin_}) {
        connectSpin(spin);
    }
    for (auto* spin : {topLedsSpin_, rightLedsSpin_, bottomLedsSpin_, leftLedsSpin_}) {
        connect(spin, &QSpinBox::valueChanged, this, [this, spin] {
            if (applyingSettings_) {
                return;
            }
            const int total = topLedsSpin_->value() + rightLedsSpin_->value()
                + bottomLedsSpin_->value() + leftLedsSpin_->value();
            if (total > 300) {
                const QSignalBlocker blocker(spin);
                spin->setValue(std::max(0, spin->value() - (total - 300)));
            } else if (total == 0) {
                const QSignalBlocker blocker(spin);
                spin->setValue(1);
            }
            updateLedSummary();
            updateDiagnostics();
            synchronizeDeviceConfiguration();
            scheduleCaptureUpdate();
            scheduleAudioUpdate();
            scheduleSave();
        });
    }

    connect(gamingModeCheck_, &QCheckBox::toggled, this, [this] {
        clearCapturedFrame();
        updateSamplingDepthUi();
        updateDiagnostics();
        scheduleCaptureUpdate();
        scheduleSave();
    });

    connect(captureMethodCombo_, &QComboBox::currentIndexChanged, this, [this] {
        clearCapturedFrame();
        updateDiagnostics();
        scheduleCaptureUpdate();
        scheduleSave();
    });

    for (auto* spin : {fpsSpin_, zonesSpin_, samplingDepthSpin_}) {
        connect(spin, &QSpinBox::valueChanged,
            this, &MainWindow::scheduleCaptureUpdate);
        connect(spin, &QSpinBox::valueChanged,
            this, &MainWindow::updateSamplingDepthUi);
    }
    connect(zonesSpin_, &QSpinBox::valueChanged,
        this, &MainWindow::updateSamplingOverlay);
    connect(
        samplingDepthSpin_,
        &QSpinBox::valueChanged,
        this,
        &MainWindow::updateSamplingDepthUi);
    connect(
        samplingDepthAutoCheck_,
        &QCheckBox::toggled,
        this,
        [this] {
            updateSamplingDepthUi();
            updateDiagnostics();
            scheduleCaptureUpdate();
            scheduleSave();
        });

    const auto connectCombo = [this](QComboBox* combo) {
        connect(combo, &QComboBox::currentIndexChanged, this, [this] {
            updateDiagnostics();
            scheduleSave();
        });
    };
    for (auto* combo : {regionColorModeCombo_, ledTransferModeCombo_,
             ledTypeCombo_, colorOrderCombo_,
             directionCombo_,
             activeModeCombo_, standaloneEffectCombo_, smoothingCombo_,
             baudCombo_, audioSourceCombo_, audioLayoutCombo_,
             audioColorModeCombo_, audioDynamicsCombo_}) {
        connectCombo(combo);
    }
    for (auto* combo : {ledTypeCombo_, colorOrderCombo_, directionCombo_,
             activeModeCombo_, standaloneEffectCombo_, baudCombo_}) {
        connect(combo, &QComboBox::currentIndexChanged,
            this, &MainWindow::scheduleTransportUpdate);
    }
    connect(activeModeCombo_, &QComboBox::currentIndexChanged,
        this, &MainWindow::scheduleCaptureUpdate);
    connect(activeModeCombo_, &QComboBox::currentIndexChanged,
        this, [this] {
            clearCapturedFrame();
            clearAudioFrame();
            updateAudioUi();
            scheduleAudioUpdate();
        });
    connect(
        flashProtectionCheck_,
        &QCheckBox::toggled,
        this,
        [this] {
            updateDiagnostics();
            scheduleSave();
        });
    connect(
        manualAnimationButton_,
        &QPushButton::toggled,
        this,
        [this](const bool checked) {
            if (checked
                && (!lightingEnabledCheck_->isChecked()
                    || comboValue(standaloneEffectCombo_)
                        == QStringLiteral("off"))) {
                const QSignalBlocker blocker(manualAnimationButton_);
                manualAnimationButton_->setChecked(false);
                statusBar()->showMessage(
                    text(
                        "Turn lighting on and select an animation first.",
                        "Сначала включите подсветку и выберите анимацию."),
                    5000);
            }
            updateManualAnimationUi();
            updateDiagnostics();
            scheduleCaptureUpdate();
            scheduleAudioUpdate();
            scheduleTransportUpdate();
        });
    connect(
        standaloneEffectCombo_,
        &QComboBox::currentIndexChanged,
        this,
        [this] {
            if (comboValue(standaloneEffectCombo_) == QStringLiteral("off")
                && manualAnimationButton_->isChecked()) {
                manualAnimationButton_->setChecked(false);
            }
            updateManualAnimationUi();
        });
    connect(
        regionColorModeCombo_,
        &QComboBox::currentIndexChanged,
        this,
        [this] {
            updateRegionColorModeUi();
            scheduleCaptureUpdate();
        });
    connect(
        ledTransferModeCombo_,
        &QComboBox::currentIndexChanged,
        this,
        &MainWindow::scheduleCaptureUpdate);
    connect(ledOffsetSpin_, &QSpinBox::valueChanged,
        this, &MainWindow::updateLedSummary);
    connect(directionCombo_, &QComboBox::currentIndexChanged,
        this, &MainWindow::updateLedSummary);
    for (auto* spin : {ledOffsetSpin_, brightnessSpin_, currentLimitSpin_,
             redCalibrationSpin_, greenCalibrationSpin_, blueCalibrationSpin_,
             ambientTimeoutSpin_}) {
        connect(spin, &QSpinBox::valueChanged,
            this, &MainWindow::scheduleTransportUpdate);
    }
    connect(resetWhiteBalanceButton_, &QPushButton::clicked, this, [this] {
        redCalibrationSpin_->setValue(100);
        greenCalibrationSpin_->setValue(100);
        blueCalibrationSpin_->setValue(100);
    });
    const auto handleLedTestToggle = [this](
                                      QPushButton* other,
                                      const bool checked) {
        if (checked && other->isChecked()) {
            const QSignalBlocker blocker(other);
            other->setChecked(false);
        }
        if (checked && !lightingEnabledCheck_->isChecked()) {
            lightingEnabledCheck_->setChecked(true);
        }
        updateLedTestUi();
        if (checked) {
            // Do not leave a debounce-sized gap in which the old fallback can
            // briefly reappear before the calibration frame takes ownership.
            transportUpdateTimer_->stop();
            synchronizeDeviceConfiguration();
        } else {
            scheduleTransportUpdate();
        }
        if (layoutTestButton_->isChecked()) {
            submitLayoutTestFrame();
            layoutTestRefreshTimer_->start();
        } else {
            layoutTestRefreshTimer_->stop();
        }
        if (checked
            && transportStatus_.state
                != transport::ConnectionState::Connected) {
            statusBar()->showMessage(
                text(
                    "LED test is waiting for the ESP32. Click the connection indicator to select its COM port.",
                    "Проверка ожидает ESP32. Нажмите индикатор подключения и выберите COM-порт платы."),
                7000);
        }
        scheduleCaptureUpdate();
        scheduleAudioUpdate();
    };
    connect(
        testWhiteBalanceButton_,
        &QPushButton::toggled,
        this,
        [this, handleLedTestToggle](const bool checked) {
            handleLedTestToggle(
                layoutTestButton_, checked);
        });
    connect(
        layoutTestButton_,
        &QPushButton::toggled,
        this,
        [this, handleLedTestToggle](const bool checked) {
            handleLedTestToggle(
                testWhiteBalanceButton_, checked);
            if (checked) {
                showLedLayoutDialog();
            }
        });
    connect(brightnessSpin_, &QSpinBox::valueChanged,
        this, &MainWindow::updatePreviewFromLastCapture);
    connect(refreshAudioDevicesButton_, &QPushButton::clicked,
        this, &MainWindow::refreshAudioDevices);
    connect(audioDefaultsButton_, &QPushButton::clicked, this, [this] {
        const AppSettings defaults;
        audioBandsSpin_->setValue(defaults.audioBandCount);
        audioMinimumFrequencySpin_->setValue(
            defaults.audioMinimumFrequencyHz);
        audioMaximumFrequencySpin_->setValue(
            defaults.audioMaximumFrequencyHz);
        audioSensitivitySpin_->setValue(defaults.audioSensitivityPercent);
        audioNoiseGateSpin_->setValue(defaults.audioNoiseGateDecibels);
        audioAttackSpin_->setValue(defaults.audioAttackMilliseconds);
        audioReleaseSpin_->setValue(defaults.audioReleaseMilliseconds);
        selectComboValue(audioDynamicsCombo_, defaults.audioDynamics);
        audioSpringinessSpin_->setValue(
            defaults.audioSpringinessPercent);
        selectComboValue(audioLayoutCombo_, defaults.audioLayout);
        selectComboValue(audioColorModeCombo_, defaults.audioColorMode);
        audioSolidColorButton_->setProperty(
            "audioColor", defaults.audioSolidColor);
        updateAudioColorButton();
        updateAudioUi();
        scheduleAudioUpdate();
        scheduleSave();
        statusBar()->showMessage(text(
            "Recommended equalizer settings restored.",
            "Рекомендуемые настройки эквалайзера восстановлены."), 3000);
    });
    connect(audioStartButton_, &QPushButton::clicked, this, [this] {
        if (audioShouldBeActive()) {
            selectComboValue(activeModeCombo_, QStringLiteral("ambilight"));
            statusBar()->showMessage(text(
                "Returned to Ambilight.",
                "Выполнен возврат к Ambilight."), 2500);
            return;
        }
        if (manualAnimationButton_->isChecked()) {
            manualAnimationButton_->setChecked(false);
        }
        if (testWhiteBalanceButton_->isChecked()) {
            testWhiteBalanceButton_->setChecked(false);
        }
        if (layoutTestButton_->isChecked()) {
            layoutTestButton_->setChecked(false);
        }
        lightingEnabledCheck_->setChecked(true);
        selectComboValue(activeModeCombo_, QStringLiteral("equalizer"));
        statusBar()->showMessage(text(
            "Audio equalizer started.",
            "Эквалайзер запущен."), 2500);
    });
    for (auto* combo : {audioSourceCombo_, audioOutputDeviceCombo_,
             audioInputDeviceCombo_, audioLayoutCombo_, audioColorModeCombo_,
             audioDynamicsCombo_}) {
        connect(combo, &QComboBox::currentIndexChanged, this, [this] {
            updateAudioUi();
            scheduleAudioUpdate();
            scheduleSave();
        });
    }
    for (auto* spin : {audioOutputGainSpin_, audioMicrophoneGainSpin_,
             audioBandsSpin_, audioMinimumFrequencySpin_,
             audioMaximumFrequencySpin_, audioSensitivitySpin_,
             audioNoiseGateSpin_, audioAttackSpin_, audioReleaseSpin_,
             audioSpringinessSpin_}) {
        connect(spin, &QSpinBox::valueChanged, this, [this] {
            scheduleAudioUpdate();
        });
    }
    connect(audioMinimumFrequencySpin_, &QSpinBox::valueChanged,
        this, &MainWindow::updateAudioUi);
    connect(audioMaximumFrequencySpin_, &QSpinBox::valueChanged,
        this, &MainWindow::updateAudioUi);
    connect(audioSolidColorButton_, &QPushButton::clicked, this, [this] {
        const QColor initial(audioSolidColorButton_->property("audioColor").toString());
        const QColor selected = QColorDialog::getColor(
            initial.isValid() ? initial : QColor(0, 170, 255),
            this,
            text("Equalizer color", "Цвет эквалайзера"));
        if (!selected.isValid()) {
            return;
        }
        audioSolidColorButton_->setProperty(
            "audioColor", selected.name(QColor::HexRgb));
        updateAudioColorButton();
        scheduleAudioUpdate();
        scheduleSave();
    });
    connect(solidColorButton_, &QPushButton::clicked, this, [this] {
        const QColor initial(
            solidColorButton_->property("solidColor").toString());
        const QColor selected = QColorDialog::getColor(
            initial.isValid() ? initial : QColor(255, 96, 24),
            this,
            text("Solid light color", "Цвет сплошного света"));
        if (!selected.isValid()) {
            return;
        }
        solidColorButton_->setProperty(
            "solidColor", selected.name(QColor::HexRgb));
        updateSolidColorButton();
        scheduleTransportUpdate();
        scheduleSave();
    });

    connect(blackBarModeCombo_, &QComboBox::currentIndexChanged, this, [this] {
        scheduleCaptureUpdate();
        updateDiagnostics();
        scheduleSave();
    });
    connect(refreshPortsButton_, &QPushButton::clicked,
        this, &MainWindow::refreshSerialPorts);
    connect(portCombo_, &QComboBox::editTextChanged, this, [this] {
        updateDiagnostics();
        scheduleTransportUpdate();
        scheduleSave();
    });

    if (trayIcon_ != nullptr) {
        connect(showAction_, &QAction::triggered, this, &MainWindow::showWindow);
        connect(toggleLightingAction_, &QAction::triggered, lightingEnabledCheck_,
            &QCheckBox::toggle);
        connect(quitAction_, &QAction::triggered, this, &MainWindow::exitApplication);
        connect(trayIcon_, &QSystemTrayIcon::activated, this,
            [this](const QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::DoubleClick
                    || reason == QSystemTrayIcon::Trigger) {
                    showWindow();
                }
        });
    }

    connect(captureWorker_, &capture::ScreenCaptureWorker::sampleReady,
        this, &MainWindow::handleCaptureSample);
    connect(captureWorker_, &capture::ScreenCaptureWorker::statusChanged,
        this, &MainWindow::handleCaptureStatus);
    connect(captureWorker_, &capture::ScreenCaptureWorker::outputsChanged,
        this, [this](const QVector<capture::CaptureOutput>& outputs) {
            applyCaptureOutputs(outputs, {});
        });

    connect(audioWorker_, &audio::AudioCaptureWorker::frameReady,
        this, &MainWindow::handleAudioFrame);
    connect(audioWorker_, &audio::AudioCaptureWorker::statusChanged,
        this, &MainWindow::handleAudioStatus);

    connect(transportWorker_, &transport::SerialTransportWorker::portsChanged,
        this, [this](
                  const QVector<transport::SerialPortDescriptor>& ports) {
            applySerialPorts(ports);
        });
    connect(
        transportWorker_,
        &transport::SerialTransportWorker::portRefreshFinished,
        this,
        [this](
            const QVector<transport::SerialPortDescriptor>& ports,
            const QString& errorMessage) {
            if (exiting_) {
                return;
            }
            serialPortRefreshInProgress_ = false;
            refreshPortsButton_->setEnabled(true);
            applySerialPorts(ports, errorMessage);
            if (!ports.isEmpty()) {
                statusBar()->showMessage(
                    text(
                        "COM port list refreshed.",
                        "Список COM-портов обновлён."),
                    2000);
            }
        });
    connect(transportWorker_, &transport::SerialTransportWorker::statusChanged,
        this, &MainWindow::handleTransportStatus);
    connect(
        transportWorker_,
        &transport::SerialTransportWorker::deviceInformationChanged,
        this,
        [this](const transport::DeviceInformation& information) {
            deviceInformation_ = information;
            updateDiagnostics();
        });
    connect(
        transportWorker_,
        &transport::SerialTransportWorker::deviceStateChanged,
        this,
        [this](const transport::DeviceState& state) {
            deviceState_ = state;
            updateDiagnostics();
        });
    connect(transportWorker_, &transport::SerialTransportWorker::commandFinished,
        this, &MainWindow::handleTransportCommand);
    connect(
        transportWorker_,
        &transport::SerialTransportWorker::statisticsChanged,
        this,
        [this](const transport::TransportStatistics& statistics) {
            transportStatistics_ = statistics;
            updateDiagnostics();
        });
    connect(
        transportWorker_,
        &transport::SerialTransportWorker::drainFinished,
        this,
        [this](const bool) {
            if (exiting_) {
                finishExit();
            }
        });
}

void MainWindow::loadSettings()
{
    auto loaded = AppSettings::load(settings_);
    loaded.startWithWindows = autostart::isEnabled();
    if (loaded.startWithWindows) {
        QString ignoredError;
        if (!autostart::setEnabled(true, &ignoredError)) {
            loaded.startWithWindows = false;
        }
    }
    applySettings(loaded);
}

void MainWindow::applySettings(const AppSettings& value)
{
    const bool wasApplyingSettings = applyingSettings_;
    applyingSettings_ = true;
    selectComboValue(languageCombo_, value.language);
    lightingEnabledCheck_->setChecked(value.lightingEnabled);
    autostartCheck_->setChecked(value.startWithWindows);
    closeToTrayCheck_->setChecked(value.closeToTray && trayIcon_ != nullptr);
    configuredHotkey_ =
        QKeySequence::fromString(value.hotkey, QKeySequence::PortableText);
    hotkeyEdit_->setKeySequence(configuredHotkey_);

    screenWidthSpin_->setValue(value.screenWidth);
    screenHeightSpin_->setValue(value.screenHeight);
    int captureOutputIndex =
        captureOutputCombo_->findData(value.captureOutputId);
    if (captureOutputIndex < 0 && !value.captureOutputId.isEmpty()) {
        for (int index = 0; index < captureOutputCombo_->count(); ++index) {
            if (capture::captureOutputIdEquals(
                    captureOutputCombo_
                        ->itemData(index, kCaptureOutputLegacyIdRole)
                        .toString(),
                    value.captureOutputId)) {
                captureOutputIndex = index;
                break;
            }
        }
    }
    if (!value.captureOutputId.isEmpty() && captureOutputIndex < 0) {
        captureOutputCombo_->addItem(
            text("Unavailable display (%1)", "Недоступный экран (%1)")
                .arg(value.captureOutputId),
            value.captureOutputId);
        captureOutputIndex = captureOutputCombo_->count() - 1;
    }
    captureOutputCombo_->setCurrentIndex(std::max(0, captureOutputIndex));
    const QSize selectedOutputSize =
        captureOutputCombo_->currentData(kCaptureOutputSizeRole).toSize();
    if (selectedOutputSize.isValid()) {
        screenWidthSpin_->setValue(selectedOutputSize.width());
        screenHeightSpin_->setValue(selectedOutputSize.height());
    }
    fpsSpin_->setValue(value.targetFps);
    selectComboValue(captureMethodCombo_, value.captureMethod);
    mirrorFpsSpin_->setValue(value.mirrorFps);
    if (!value.mirrorOutputId.isEmpty()) {
        if (mirrorTargetCombo_->findData(value.mirrorOutputId) < 0)
            mirrorTargetCombo_->addItem(text("Unavailable display (%1)", "Недоступный экран (%1)")
                .arg(value.mirrorOutputId), value.mirrorOutputId);
        selectComboValue(mirrorTargetCombo_, value.mirrorOutputId);
    }
    gamingModeCheck_->setChecked(value.gamingMode);
    zonesSpin_->setValue(value.samplingZones);
    selectComboValue(regionColorModeCombo_, value.regionColorMode);
    selectComboValue(ledTransferModeCombo_, value.ledTransferMode);
    samplingDepthAutoCheck_->setChecked(value.automaticSamplingDepth);
    samplingDepthSpin_->setValue(value.samplingDepthPixels);
    updateSamplingDepthUi();
    selectComboValue(blackBarModeCombo_, value.blackBarDetectionMode);
    const int smoothingIndex = smoothingCombo_->findData(
        value.smoothingMilliseconds);
    smoothingCombo_->setCurrentIndex(std::max(0, smoothingIndex));
    flashProtectionCheck_->setChecked(value.flashProtectionEnabled);

    selectComboValue(ledTypeCombo_, value.ledType);
    selectComboValue(colorOrderCombo_, value.colorOrder);
    topLedsSpin_->setValue(value.topLeds);
    rightLedsSpin_->setValue(value.rightLeds);
    bottomLedsSpin_->setValue(value.bottomLeds);
    leftLedsSpin_->setValue(value.leftLeds);
    updateLedSummary();
    ledOffsetSpin_->setValue(value.ledOffset);
    selectComboValue(
        directionCombo_,
        value.clockwise ? QStringLiteral("clockwise") : QStringLiteral("counterclockwise"));
    updateLedSummary();
    brightnessSpin_->setValue(value.brightnessPercent);
    currentLimitSpin_->setValue(value.currentLimitMilliAmps);
    redCalibrationSpin_->setValue(value.redCalibrationPercent);
    greenCalibrationSpin_->setValue(value.greenCalibrationPercent);
    blueCalibrationSpin_->setValue(value.blueCalibrationPercent);

    selectComboValue(activeModeCombo_, value.activeMode);
    selectComboValue(standaloneEffectCombo_, value.standaloneEffect);
    solidColorButton_->setProperty("solidColor", value.solidColor);
    updateSolidColorButton();
    manualAnimationButton_->setChecked(false);
    updateManualAnimationUi();

    selectComboValue(audioSourceCombo_, value.audioSource);
    const auto selectAudioDevice = [this](
                                       QComboBox* combo,
                                       const QString& id,
                                       const char* english,
                                       const char* russian) {
        if (!id.isEmpty() && combo->findData(id) < 0) {
            combo->addItem(text(english, russian).arg(id), id);
        }
        selectComboValue(combo, id);
    };
    selectAudioDevice(
        audioOutputDeviceCombo_, value.audioOutputDeviceId,
        "Unavailable output (%1)", "Недоступный выход (%1)");
    selectAudioDevice(
        audioInputDeviceCombo_, value.audioInputDeviceId,
        "Unavailable microphone (%1)", "Недоступный микрофон (%1)");
    audioBandsSpin_->setValue(value.audioBandCount);
    audioMinimumFrequencySpin_->setRange(20, 2'000);
    audioMaximumFrequencySpin_->setRange(1'000, 24'000);
    audioMaximumFrequencySpin_->setValue(value.audioMaximumFrequencyHz);
    audioMinimumFrequencySpin_->setValue(value.audioMinimumFrequencyHz);
    audioSensitivitySpin_->setValue(value.audioSensitivityPercent);
    audioNoiseGateSpin_->setValue(value.audioNoiseGateDecibels);
    audioOutputGainSpin_->setValue(value.audioOutputGainPercent);
    audioMicrophoneGainSpin_->setValue(value.audioMicrophoneGainPercent);
    audioAttackSpin_->setValue(value.audioAttackMilliseconds);
    audioReleaseSpin_->setValue(value.audioReleaseMilliseconds);
    selectComboValue(audioDynamicsCombo_, value.audioDynamics);
    audioSpringinessSpin_->setValue(value.audioSpringinessPercent);
    selectComboValue(audioLayoutCombo_, value.audioLayout);
    selectComboValue(audioColorModeCombo_, value.audioColorMode);
    audioSolidColorButton_->setProperty("audioColor", value.audioSolidColor);
    updateAudioColorButton();
    updateAudioUi();

    const QString selectedPort =
        value.automaticPortSelection ? QString {} : value.portName;
    if (!selectedPort.isEmpty()
        && portCombo_->findData(selectedPort) < 0) {
        portCombo_->addItem(
            text("Unavailable port (%1)", "Недоступный порт (%1)")
                .arg(selectedPort),
            selectedPort);
    }
    selectComboValue(portCombo_, selectedPort);
    const int baudIndex = baudCombo_->findData(value.baudRate);
    baudCombo_->setCurrentIndex(std::max(0, baudIndex));
    ambientTimeoutSpin_->setValue(value.ambientTimeoutMs);

    language_ = value.language == QStringLiteral("en")
        ? Language::English
        : Language::Russian;
    updateQtTranslator();
    retranslateUi();
    applyingSettings_ = wasApplyingSettings;
}

AppSettings MainWindow::settingsFromUi() const
{
    AppSettings value;
    value.language = language_ == Language::English
        ? QStringLiteral("en")
        : QStringLiteral("ru");
    value.lightingEnabled = lightingEnabledCheck_->isChecked();
    value.startWithWindows = autostartCheck_->isChecked();
    value.closeToTray = closeToTrayCheck_->isChecked();
    value.hotkey = configuredHotkey_.toString(QKeySequence::PortableText);

    value.screenWidth = screenWidthSpin_->value();
    value.screenHeight = screenHeightSpin_->value();
    value.captureOutputId = comboValue(captureOutputCombo_);
    value.targetFps = fpsSpin_->value();
    value.captureMethod = comboValue(captureMethodCombo_);
    value.mirrorOutputId = mirrorTargetCombo_->currentData().toString();
    value.mirrorFps = mirrorFpsSpin_->value();
    value.gamingMode = gamingModeCheck_->isChecked();
    value.samplingZones = zonesSpin_->value();
    value.regionColorMode = comboValue(regionColorModeCombo_);
    value.ledTransferMode = comboValue(ledTransferModeCombo_);
    value.automaticSamplingDepth = samplingDepthAutoCheck_->isChecked();
    value.samplingDepthPixels = samplingDepthSpin_->value();
    value.blackBarDetectionMode = comboValue(blackBarModeCombo_);
    value.smoothingMilliseconds = smoothingCombo_->currentData().toInt();
    value.flashProtectionEnabled = flashProtectionCheck_->isChecked();

    value.ledType = comboValue(ledTypeCombo_);
    value.colorOrder = comboValue(colorOrderCombo_);
    value.topLeds = topLedsSpin_->value();
    value.rightLeds = rightLedsSpin_->value();
    value.bottomLeds = bottomLedsSpin_->value();
    value.leftLeds = leftLedsSpin_->value();
    value.ledOffset = ledOffsetSpin_->value();
    value.clockwise = comboValue(directionCombo_) == QStringLiteral("clockwise");
    value.brightnessPercent = brightnessSpin_->value();
    value.currentLimitMilliAmps = currentLimitSpin_->value();
    value.redCalibrationPercent = redCalibrationSpin_->value();
    value.greenCalibrationPercent = greenCalibrationSpin_->value();
    value.blueCalibrationPercent = blueCalibrationSpin_->value();

    value.activeMode = comboValue(activeModeCombo_);
    value.standaloneEffect = comboValue(standaloneEffectCombo_);
    value.solidColor =
        solidColorButton_->property("solidColor").toString();
    value.audioSource = comboValue(audioSourceCombo_);
    value.audioOutputDeviceId = comboValue(audioOutputDeviceCombo_);
    value.audioInputDeviceId = comboValue(audioInputDeviceCombo_);
    value.audioBandCount = audioBandsSpin_->value();
    value.audioMinimumFrequencyHz = audioMinimumFrequencySpin_->value();
    value.audioMaximumFrequencyHz = audioMaximumFrequencySpin_->value();
    value.audioSensitivityPercent = audioSensitivitySpin_->value();
    value.audioNoiseGateDecibels = audioNoiseGateSpin_->value();
    value.audioOutputGainPercent = audioOutputGainSpin_->value();
    value.audioMicrophoneGainPercent = audioMicrophoneGainSpin_->value();
    value.audioAttackMilliseconds = audioAttackSpin_->value();
    value.audioReleaseMilliseconds = audioReleaseSpin_->value();
    value.audioDynamics = comboValue(audioDynamicsCombo_);
    value.audioSpringinessPercent = audioSpringinessSpin_->value();
    value.audioLayout = comboValue(audioLayoutCombo_);
    value.audioColorMode = comboValue(audioColorModeCombo_);
    value.audioSolidColor =
        audioSolidColorButton_->property("audioColor").toString();
    value.portName = serialPortValue().trimmed().toUpper();
    value.automaticPortSelection = value.portName.isEmpty();
    value.baudRate = baudCombo_->currentData().toInt();
    value.ambientTimeoutMs = ambientTimeoutSpin_->value();
    value.normalize();
    return value;
}

void MainWindow::saveSettings(const bool showConfirmation)
{
    saveTimer_->stop();
    const auto value = settingsFromUi();
    if (!value.save(settings_)) {
        statusBar()->showMessage(
            text("Could not save settings.", "Не удалось сохранить настройки."),
            5000);
        return;
    }
    updateDiagnostics();
    if (showConfirmation) {
        statusBar()->showMessage(
            text("Settings saved.", "Настройки сохранены."), 2000);
    }
}

void MainWindow::scheduleSave()
{
    if (!applyingSettings_) {
        saveTimer_->start();
    }
}

void MainWindow::restoreDefaults()
{
    const auto answer = QMessageBox::question(
        this,
        windowTitle(),
        text("Restore all application settings to their defaults?",
            "Вернуть все настройки приложения к значениям по умолчанию?"));
    if (answer != QMessageBox::Yes) {
        return;
    }

    QString error;
    if (!autostart::setEnabled(false, &error)) {
        QMessageBox::warning(this, windowTitle(), error);
    }

    stopMirror();
    unregisterGlobalHotkey();
    applySettings(AppSettings {});
    applyHotkey(hotkeyEdit_->keySequence());
    saveSettings(false);
    updateEnabledState();
    updateCaptureConfiguration();
    updateAudioConfiguration();
    synchronizeDeviceConfiguration();
    updateTransportConfiguration();
}

void MainWindow::setLanguage(const Language language)
{
    language_ = language;
    updateQtTranslator();
    retranslateUi();
    updateLedSummary();
    updateDiagnostics();
}

void MainWindow::updateQtTranslator()
{
    qApp->removeTranslator(qtTranslator_);
    if (language_ == Language::English) {
        return;
    }

    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    const QStringList searchDirectories{
        QDir(applicationDirectory).filePath(QStringLiteral("../translations")),
        QDir(applicationDirectory).filePath(QStringLiteral("translations")),
        QLibraryInfo::path(QLibraryInfo::TranslationsPath),
    };
    for (const QString& directory : searchDirectories) {
        for (const QString& fileName :
             {QStringLiteral("qt_ru.qm"), QStringLiteral("qtbase_ru.qm")}) {
            if (qtTranslator_->load(QDir(directory).filePath(fileName))) {
                qApp->installTranslator(qtTranslator_);
                return;
            }
        }
    }
}

QString MainWindow::text(const char* english, const char* russian) const
{
    return language_ == Language::English
        ? QString::fromUtf8(english)
        : QString::fromUtf8(russian);
}

void MainWindow::retranslateUi()
{
    updateMirrorUi();
    setWindowTitle(QStringLiteral("Ambilight by Levurgi4 — v%1")
        .arg(QCoreApplication::applicationVersion()));
    tabs_->setTabText(tabs_->indexOf(generalPage_), text("General", "Основное"));
    tabs_->setTabText(tabs_->indexOf(screenPage_), text("Screen", "Экран"));
    tabs_->setTabText(tabs_->indexOf(audioPage_), text("Audio", "Звук"));
    tabs_->setTabText(tabs_->indexOf(ledPage_), text("LED strip", "Светодиодная лента"));
    tabs_->setTabText(tabs_->indexOf(connectionPage_), text("Connection", "Подключение"));
    tabs_->setTabText(tabs_->indexOf(diagnosticsPage_), text("Diagnostics", "Диагностика"));

    for (auto* button : {generalHelpButton_, screenHelpButton_, audioHelpButton_, ledHelpButton_,
             connectionHelpButton_, diagnosticsHelpButton_}) {
        button->setText(text("Help", "Справка"));
    }
    generalSettingsGroup_->setTitle(
        text("Application", "Приложение"));
    lightingModeGroup_->setTitle(
        text("Lighting mode", "Режим подсветки"));
    languageLabel_->setText(text("Language:", "Язык:"));
    lightingEnabledCheck_->setText(
        text(
            "Enable lighting (master switch)",
            "Включить подсветку (главный переключатель)"));
    autostartCheck_->setText(text("Start with Windows", "Запускать вместе с Windows"));
    closeToTrayCheck_->setText(text("Close window to tray", "Сворачивать в трей при закрытии"));
    hotkeyLabel_->setText(
        text(
            "Toggle lighting hotkey:",
            "Горячая клавиша подсветки:"));
    hotkeyHintLabel_->setText(
        text(
            "Turns the master lighting switch on or off from any application, "
            "even while this window is hidden. Release the keys; the new "
            "shortcut is applied after a short pause.",
            "Включает или выключает главный переключатель подсветки из любой "
            "программы, даже когда это окно скрыто. Отпустите клавиши — новое "
            "сочетание применится после короткой паузы."));
    updateHotkeyStatusUi();

    screenSourceGroup_->setTitle(
        text("Image source", "Источник изображения"));
    screenColorGroup_->setTitle(
        text("Zone color calculation", "Расчёт цвета по зонам"));
    captureOutputLabel_->setText(text("Capture display:", "Экран захвата:"));
    refreshOutputsButton_->setText(text("Refresh", "Обновить"));
    if (captureOutputCombo_->count() > 0
        && captureOutputCombo_->itemData(0).toString().isEmpty()) {
        captureOutputCombo_->setItemText(
            0,
            text("Automatic (primary display)", "Автоматически (основной экран)"));
    }
    resolutionLabel_->setText(
        text("Capture resolution:", "Разрешение захвата:"));
    widthLabel_->setText(text("Width", "Ширина"));
    heightLabel_->setText(text("Height", "Высота"));
    fpsLabel_->setText(
        text("Processing frame rate:", "Частота обработки:"));
    fpsSpin_->setToolTip(
        text(
            "Target upper rate for screen capture and processing. Actual "
            "output can be lower; this does not change the monitor refresh "
            "rate.",
            "Верхняя целевая частота захвата и обработки изображения. "
            "Фактическая частота может быть ниже; настройка не меняет "
            "герцовку монитора."));
    zonesLabel_->setText(
        text("Color zone count:", "Количество цветовых зон:"));
    zonesSpin_->setToolTip(
        text(
            "Independent of the LED count. More zones preserve finer color "
            "transitions but require slightly more GPU work.",
            "Настраиваются независимо от числа LED. Большее число зон лучше "
            "сохраняет мелкие цветовые переходы, но немного увеличивает "
            "нагрузку на GPU."));
    regionColorModeLabel_->setText(
        text("Zone color algorithm:", "Алгоритм цвета зоны:"));
    regionColorModeCombo_->setItemText(
        regionColorModeCombo_->findData(QStringLiteral("mean")),
        text(
            "Mean (low GPU load)",
            "Среднее (низкая нагрузка)"));
    regionColorModeCombo_->setItemText(
        regionColorModeCombo_->findData(QStringLiteral("rms")),
        text(
            "Root mean square (low GPU load)",
            "Среднеквадратичное (низкая нагрузка)"));
    regionColorModeCombo_->setItemText(
        regionColorModeCombo_->findData(QStringLiteral("dominant")),
        text(
            "Dominant color (increased GPU load)",
            "Доминирующий цвет (повышенная нагрузка)"));
    ledTransferModeLabel_->setText(text(
        "Dark-tone transfer:",
        "Передача тёмных оттенков:"));
    ledTransferModeCombo_->setItemText(
        ledTransferModeCombo_->findData(QStringLiteral("raw")),
        text(
            "No correction (brighter shadows)",
            "Без коррекции (светлее тени)"));
    ledTransferModeCombo_->setItemText(
        ledTransferModeCombo_->findData(QStringLiteral("soft-gamma")),
        text("Soft — gamma 1.6", "Мягкая — гамма 1,6"));
    ledTransferModeCombo_->setItemText(
        ledTransferModeCombo_->findData(QStringLiteral("srgb")),
        text(
            "sRGB to LED (recommended)",
            "sRGB → LED (рекомендуется)"));
    ledTransferModeCombo_->setItemText(
        ledTransferModeCombo_->findData(QStringLiteral("strong-gamma")),
        text("Strong — gamma 2.6", "Сильная — гамма 2,6"));
    ledTransferModeCombo_->setToolTip(text(
        "Compensates for the difference between sRGB screen values and linear LED PWM. It mainly darkens shadows while preserving full white. GPU impact is negligible.",
        "Компенсирует разницу между значениями sRGB на экране и линейным ШИМ светодиодов. В основном затемняет тени, сохраняя полный белый. Влияние на GPU пренебрежимо мало."));
    samplingDepthLabel_->setText(
        text("Depth from edge:", "Глубина от края:"));
    samplingDepthAutoCheck_->setText(text("Automatic", "Автоматически"));
    ledPreview_->setToolTip(text(
        "Sampling map: cyan is top, green is right, violet is bottom, and amber is left. Thin lines separate color zones; the inner boundary shows the selected depth. Black bars are excluded.",
        "Карта выборки: голубой — верх, зелёный — правая сторона, фиолетовый — низ, оранжевый — левая сторона. Тонкие линии разделяют цветовые зоны, внутренняя граница показывает выбранную глубину. Чёрные полосы исключаются."));
    updateSamplingDepthUi();
    blackBarModeLabel_->setText(
        text("Black bars:", "Чёрные полосы:"));
    blackBarModeCombo_->setItemText(
        blackBarModeCombo_->findData(QStringLiteral("off")),
        text("Do not exclude", "Не исключать"));
    blackBarModeCombo_->setItemText(
        blackBarModeCombo_->findData(QStringLiteral("first-content")),
        text(
            "First visible content (all sides)",
            "До первого видимого контента (все стороны)"));
    blackBarModeCombo_->setItemText(
        blackBarModeCombo_->findData(QStringLiteral("limit-21-9")),
        text(
            "Up to the 21:9 boundary (top and bottom)",
            "До границы 21:9 (сверху и снизу)"));
    blackBarModeCombo_->setToolTip(text(
        "First visible content can exclude symmetric bars on all four sides. The 21:9 mode searches only the top and bottom and never goes deeper than a 21:9 frame inside the captured image.",
        "Режим первого контента может исключать симметричные полосы со всех четырёх сторон. Режим 21:9 ищет только сверху и снизу и не заходит глубже рамки 21:9 внутри захваченного изображения."));
    smoothingLabel_->setText(
        text("Transition smoothing:", "Сглаживание переходов:"));
    smoothingCombo_->setItemText(
        smoothingCombo_->findData(0),
        text("Off (minimum latency)", "Выключено (минимальная задержка)"));
    smoothingCombo_->setItemText(
        smoothingCombo_->findData(25),
        text("Fast — 25 ms (recommended)", "Быстрое — 25 мс (рекомендуется)"));
    smoothingCombo_->setItemText(
        smoothingCombo_->findData(60),
        text("Balanced — 60 ms", "Сбалансированное — 60 мс"));
    smoothingCombo_->setItemText(
        smoothingCombo_->findData(120),
        text("Soft — 120 ms", "Мягкое — 120 мс"));
    flashProtectionCheck_->setText(text(
        "Suppress sudden full-screen flashes",
        "Подавлять резкие вспышки всего экрана"));
    temporalFilterHintLabel_->setText(text(
        "Smoothing reduces flicker at scene cuts; stronger values react more slowly. Flash protection limits only abrupt increases in overall brightness and lets decreases pass immediately.",
        "Сглаживание уменьшает мерцание при смене сцен; более сильные значения реагируют медленнее. Защита от вспышек ограничивает только резкий рост общей яркости, а уменьшение пропускает сразу."));

    audioSourceGroup_->setTitle(text("Audio source", "Источник звука"));
    audioVisualizationGroup_->setTitle(
        text("Spectrum visualization", "Визуализация спектра"));
    audioSourceLabel_->setText(text("Capture:", "Захватывать:"));
    audioSourceCombo_->setItemText(
        audioSourceCombo_->findData(QStringLiteral("windows")),
        text("Windows audio", "Звуки Windows"));
    audioSourceCombo_->setItemText(
        audioSourceCombo_->findData(QStringLiteral("microphone")),
        text("Microphone", "Микрофон"));
    audioSourceCombo_->setItemText(
        audioSourceCombo_->findData(QStringLiteral("mixed")),
        text("Windows + microphone", "Windows + микрофон"));
    audioOutputDeviceLabel_->setText(
        text("Output device:", "Устройство вывода:"));
    audioInputDeviceLabel_->setText(
        text("Microphone:", "Микрофон:"));
    audioOutputGainLabel_->setText(
        text("Windows gain:", "Усиление Windows:"));
    audioMicrophoneGainLabel_->setText(
        text("Microphone gain:", "Усиление микрофона:"));
    refreshAudioDevicesButton_->setText(
        text("Refresh devices", "Обновить устройства"));
    audioDefaultsButton_->setText(
        text("Recommended settings", "Рекомендуемые настройки"));
    audioDefaultsButton_->setToolTip(text(
        "Restores only spectrum processing and visualization settings. Audio devices, gains, LED layout and connection are not changed.",
        "Восстанавливает только обработку и отображение спектра. Аудиоустройства, усиление, раскладка LED и подключение не изменяются."));
    audioLevelMeter_->setFormat(text("Level: %p%", "Уровень: %p%"));
    audioLevelMeter_->setToolTip(text(
        "Current peak after sensitivity, gains and dynamics. This meter also works without a connected ESP32.",
        "Текущий пик после чувствительности, усиления и динамики. Индикатор работает и без подключённой ESP32."));
    audioBandsLabel_->setText(
        text("Frequency bands:", "Частотные зоны:"));
    audioFrequencyRangeLabel_->setText(
        text("Frequency range:", "Диапазон частот:"));
    audioSensitivityLabel_->setText(
        text("Sensitivity:", "Чувствительность:"));
    audioNoiseGateLabel_->setText(
        text("Silence threshold:", "Порог тишины:"));
    audioAttackLabel_->setText(text("Attack:", "Загорание:"));
    audioReleaseLabel_->setText(text("Release:", "Затухание:"));
    audioDynamicsLabel_->setText(text("Dynamics:", "Динамика:"));
    audioDynamicsCombo_->setItemText(
        audioDynamicsCombo_->findData(QStringLiteral("smooth")),
        text("Smooth", "Плавная"));
    audioDynamicsCombo_->setItemText(
        audioDynamicsCombo_->findData(QStringLiteral("spring")),
        text("Spring (recommended)", "Пружинная (рекомендуется)"));
    audioSpringinessLabel_->setText(
        text("Springiness:", "Пружинистость:"));
    audioSpringinessSpin_->setToolTip(text(
        "Controls overshoot and the number of diminishing rebounds. It does not change the global brightness limit.",
        "Задаёт перелёт и количество затухающих колебаний. Общий предел яркости не изменяется."));
    audioLayoutLabel_->setText(text("Band layout:", "Расположение зон:"));
    audioLayoutCombo_->setItemText(
        audioLayoutCombo_->findData(QStringLiteral("mirrored")),
        text(
            "Mirrored from bottom center (recommended)",
            "Зеркально от центра снизу (рекомендуется)"));
    audioLayoutCombo_->setItemText(
        audioLayoutCombo_->findData(QStringLiteral("sequential")),
        text("Low to high around perimeter", "От низких к высоким по кругу"));
    audioColorModeLabel_->setText(text("Colors:", "Цвета:"));
    audioColorModeCombo_->setItemText(
        audioColorModeCombo_->findData(QStringLiteral("spectrum")),
        text("Frequency gradient", "Градиент по частоте"));
    audioColorModeCombo_->setItemText(
        audioColorModeCombo_->findData(QStringLiteral("solid")),
        text("One color", "Один цвет"));
    audioSolidColorButton_->setText(text("Choose", "Выбрать"));
    audioHintLabel_->setText(text(
        "24 bands, 40–16000 Hz, 25 ms attack, 180 ms release and 55% springiness are a good starting point. Processing stays local; audio is not recorded or sent anywhere.",
        "24 зоны, 40–16000 Гц, загорание 25 мс, затухание 180 мс и пружинистость 55% — хорошая отправная точка. Обработка выполняется локально; звук не записывается и никуда не отправляется."));
    updateAudioColorButton();
    updateAudioUi();

    ledTypeLabel_->setText(text("LED type:", "Тип светодиодов:"));
    voltageHintLabel_->setText(text(
        "WS2815 strip power: 12.0 V DC nominal. The chip's absolute upper limit is 13.5 V; do not use it as the operating target. Never connect 12 V to the ESP32.",
        "Питание ленты WS2815: номинально 12,0 В DC. Абсолютный верхний предел микросхемы — 13,5 В; не используйте его как рабочее напряжение. Никогда не подавайте 12 В на ESP32."));
    colorOrderLabel_->setText(text("Color order:", "Порядок цветов:"));
    topLedsLabel_->setText(text("Top side:", "Верхняя сторона:"));
    rightLedsLabel_->setText(text("Right side:", "Правая сторона:"));
    bottomLedsLabel_->setText(text("Bottom side:", "Нижняя сторона:"));
    leftLedsLabel_->setText(text("Left side:", "Левая сторона:"));
    offsetLabel_->setText(text("Cyclic offset:", "Циклическое смещение:"));
    directionLabel_->setText(text("Direction:", "Направление:"));
    brightnessLabel_->setText(text("Brightness limit:", "Ограничение яркости:"));
    currentLimitLabel_->setText(text(
        "Estimated strip current:",
        "Расчётный лимит тока ленты:"));
    outputLimitsHintLabel_->setText(text(
        "This adaptive budget dims a frame only when its estimated consumption exceeds the selected mA value. The brightness control is on the General page.",
        "Этот адаптивный бюджет уменьшает кадр только тогда, когда его расчётное потребление превышает выбранное значение в мА. Настройка яркости находится на вкладке «Основное»."));
    brightnessSpin_->setToolTip(text(
        "A fixed 0–100% ceiling applied to every RGB channel.",
        "Постоянный предел 0–100%, применяемый к каждому каналу RGB."));
    currentLimitSpin_->setToolTip(text(
        "Adaptive budget for the estimated consumption of the entire strip. White can reach it sooner than red or a dark scene.",
        "Адаптивный бюджет расчётного потребления всей ленты. Белый цвет достигает его раньше, чем красный или тёмная сцена."));
    layoutTestButton_->setText(
        layoutTestButton_->isChecked()
            ? text("Configuring sides...", "Настройка сторон...")
            : text("Configure and test sides", "Настроить и проверить стороны"));
    whiteBalanceGroup_->setTitle(
        text("White balance", "Баланс белого"));
    redCalibrationLabel_->setText(text("Red:", "Красный:"));
    greenCalibrationLabel_->setText(text("Green:", "Зелёный:"));
    blueCalibrationLabel_->setText(text("Blue:", "Синий:"));
    testWhiteBalanceButton_->setText(
        testWhiteBalanceButton_->isChecked()
            ? text("Stop white test", "Завершить проверку")
            : text("Test white", "Проверить белый"));
    resetWhiteBalanceButton_->setText(
        text("Reset to 100%", "Сбросить на 100%"));
    whiteBalanceGroup_->setToolTip(text(
        "Reduces individual channels after color calculation. Use a white "
        "test image and lower the excessive channels; 100% leaves a channel "
        "unchanged.",
        "Ослабляет отдельные каналы после расчёта цвета. Включите белый "
        "тестовый цвет и уменьшайте избыточные каналы; 100% не изменяет "
        "канал."));
    directionCombo_->setItemText(
        directionCombo_->findData(QStringLiteral("clockwise")),
        text("Clockwise", "По часовой стрелке"));
    directionCombo_->setItemText(
        directionCombo_->findData(QStringLiteral("counterclockwise")),
        text("Counterclockwise", "Против часовой стрелки"));

    activeModeLabel_->setText(text("Current mode:", "Текущий режим:"));
    standaloneEffectLabel_->setText(
        text("When PC is unavailable:", "Когда ПК недоступен:"));
    solidColorLabel_->setText(
        text("Solid light color:", "Цвет сплошного света:"));
    solidColorButton_->setText(text("Choose...", "Выбрать..."));
    standaloneHintLabel_->setText(
        text(
            "This fallback starts automatically when the PC color stream disappears. The button runs the same animation now without disconnecting the ESP32.",
            "Эта заглушка запускается автоматически при пропадании потока цветов с ПК. Кнопка включает ту же анимацию сейчас, не отключая ESP32 от компьютера."));

    const std::array<const char*, 7> activeValues {
        "ambilight", "equalizer", "solid", "breathing", "rainbow", "wave", "off"};
    const std::array<const char*, 7> activeEnglish {
        "Ambilight", "Audio equalizer", "Solid color", "Breathing", "Rainbow", "Wave", "Off"};
    const std::array<const char*, 7> activeRussian {
        "Ambilight", "Эквалайзер", "Сплошной цвет", "Дыхание", "Радуга", "Волна", "Выключено"};
    for (qsizetype index = 0; index < std::ssize(activeValues); ++index) {
        const int comboIndex =
            activeModeCombo_->findData(QString::fromLatin1(activeValues[index]));
        activeModeCombo_->setItemText(
            comboIndex,
            text(activeEnglish[index], activeRussian[index]));
    }

    const std::array<const char*, 5> standaloneValues {
        "solid", "breathing", "rainbow", "wave", "off"};
    for (qsizetype index = 0; index < std::ssize(standaloneValues); ++index) {
        const int comboIndex =
            standaloneEffectCombo_->findData(QString::fromLatin1(standaloneValues[index]));
        standaloneEffectCombo_->setItemText(
            comboIndex,
            text(activeEnglish[index + 2], activeRussian[index + 2]));
    }
    updateManualAnimationUi();

    portLabel_->setText(text("COM port:", "COM-порт:"));
    refreshPortsButton_->setText(text("Refresh", "Обновить"));
    const int automaticPortIndex = portCombo_->findData(QString {});
    if (automaticPortIndex >= 0) {
        portCombo_->setItemText(
            automaticPortIndex,
            text("Automatic (Ambilight device)",
                "Автоматически (устройство Ambilight)"));
    }
    baudLabel_->setText(text("Baud rate:", "Скорость порта:"));
    ambientTimeoutLabel_->setText(text("Stream timeout:", "Тайм-аут потока:"));
    connectionStatusLabel_->setText(transportStateText());

    copyDiagnosticsButton_->setText(text("Copy", "Копировать"));
    defaultsButton_->setText(text("Restore defaults", "По умолчанию"));
    saveButton_->setText(text("Save", "Сохранить"));
    quitButton_->setText(text("Quit", "Выйти"));

    if (trayIcon_ != nullptr) {
        trayIcon_->setToolTip(QStringLiteral("Ambilight by Levurgi4"));
        showAction_->setText(text("Open settings", "Открыть настройки"));
        toggleLightingAction_->setText(
            lightingEnabledCheck_->isChecked()
                ? text("Turn lighting off", "Выключить подсветку")
                : text("Turn lighting on", "Включить подсветку"));
        quitAction_->setText(text("Quit", "Выйти"));
    }

    applyCaptureOutputs(
        captureOutputs_,
        captureOutputEnumerationError_);
    refreshAudioDevices();
    applySerialPorts(serialPorts_, serialPortEnumerationError_);
    updateLedSummary();
    updateLedTestUi();
    updateConnectionShortcut();
}

void MainWindow::showTabHelp(QWidget* page)
{
    QString sectionName;
    QString helpText;
    const bool english = language_ == Language::English;

    if (page == generalPage_) {
        sectionName = text("General", "Основное");
        helpText = english
            ? QString::fromUtf8(R"HELP(This page controls the application and the current lighting mode.

• Enable lighting is the master switch. Turning it off stops capture and tells the controller to enter Off mode. The global hotkey and tray command toggle this same switch.

• Current mode: Ambilight follows the screen edges; Audio equalizer reacts to Windows audio, a microphone, or both; Solid color, Breathing, Rainbow, and Wave run on the ESP32; Off keeps the strip dark.

• Brightness is the global 0–100% ceiling for Ambilight, the equalizer, and animations.

• Solid light color is used by Solid color both as the current mode and as the fallback when the PC stream disappears.

• When PC is unavailable selects the fallback effect used when the Ambilight or equalizer stream disappears. Start animation runs that same effect immediately while USB remains connected; Stop animation returns to Current mode. The controller and strip must remain powered. After an ESP32 restart, the application must reconnect once to send this setting again.

• Start with Windows launches the application after sign-in. Close window to tray hides the window without stopping the lighting; use Quit to stop the application completely.

• The global hotkey turns lighting on or off from any application, even while this window is hidden. Use one key together with Ctrl, Alt, Shift, or Win, then release the keys; the combination is registered after a short pause. Clear the field to disable it. Avoid combinations already reserved by Windows or another application.)HELP")
            : QString::fromUtf8(R"HELP(Здесь настраиваются поведение приложения и текущий режим подсветки.

• «Включить подсветку» — главный переключатель. При отключении захват останавливается, а контроллеру отправляется режим «Выключено». Горячая клавиша и команда в трее переключают именно его.

• Текущий режим: Ambilight повторяет цвета краёв экрана; «Эквалайзер» реагирует на звук Windows, микрофон или оба источника; «Сплошной цвет», «Дыхание», «Радуга» и «Волна» выполняются на ESP32; «Выключено» гасит ленту.

• Яркость — общий предел 0–100% для Ambilight, эквалайзера и анимаций.

• «Цвет сплошного света» применяется к режиму «Сплошной цвет» — как к текущему режиму, так и к заглушке при пропадании потока с ПК.

• «Когда ПК недоступен» выбирает заглушку на случай пропадания потока Ambilight или эквалайзера. «Запустить анимацию» включает тот же эффект немедленно, не отключая USB; «Остановить анимацию» возвращает текущий режим. Контроллер и лента должны оставаться запитанными. После перезапуска ESP32 приложение должно подключиться снова и передать настройку.

• Автозапуск запускает приложение после входа в Windows. Закрытие в трей скрывает окно, но не останавливает подсветку; для полного завершения используйте «Выйти».

• Глобальная горячая клавиша включает и выключает подсветку из любой программы. Используйте одну клавишу вместе с Ctrl, Alt, Shift или Win, затем отпустите клавиши — сочетание зарегистрируется после короткой паузы. Очистите поле, чтобы отключить сочетание. Не выбирайте комбинацию, уже занятую Windows или другой программой.)HELP");
    } else if (page == screenPage_) {
        sectionName = text("Screen", "Экран");
        helpText = english
            ? QString::fromUtf8(R"HELP(This page selects the image source and how edge colors are calculated.

• Capture display selects which display is analyzed. Automatic follows the Windows primary display; select the TV explicitly if it must work independently. Use Refresh after connecting or disconnecting a display.

• Capture method: use Windows Graphics Capture if DXGI captures the desktop but freezes in a game. Select the game's display explicitly, especially with duplicated or multiple displays. Windows may show a capture border. Both methods support Game mode, but capture overhead differs. This is display capture, not an in-game hook; some games or protected content may still be unavailable. Diagnostics retains the last capture error even after minimizing the game.

• Capture resolution is detected automatically and is read-only.

• Game mode reduces GPU work: up to 30 FPS, 80 zones and 16 points per zone with mean color. Only the edge strips are copied. Black-bar detection is disabled, so letterboxed games may produce dark top/bottom lighting. Lower FPS and fewer samples reduce smoothness and detail. The depth, LED count, white balance and dark-tone correction remain available. Disabling Game mode restores your normal processing settings. The FPS benefit depends on the game, GPU and Windows capture overhead.

• Processing frame rate is the target upper rate for screen capture and analysis. Actual transport output may be lower. It does not change the monitor refresh rate. Start with 60 FPS for a 60 Hz TV.

• Color zones are logical perimeter regions independent of the LED count. More zones preserve finer local transitions but add a little GPU work. The maximum is 300.

• Zone color algorithm: Mean is calm and stable; Root mean square emphasizes bright details at the same low load; Dominant color is more distinct but can require two passes and may ignore small details.

• Dark-tone transfer compensates for the fact that screen pixels are encoded as sRGB while LED PWM controls light output almost linearly. sRGB to LED is recommended: it corrects overly bright shadows while keeping full white unchanged. Soft and strong gamma variants let you adapt the result to the strip, wall and room; No correction preserves the behavior of versions through 0.2.5. The extra GPU work is negligible.

• Depth from edge controls how far inward colors are sampled. Automatic is recommended initially; clear it to enter 8–512 px manually. Larger values include more of the image center and increase sampling from 32 to 128 points per zone. Effective depth never crosses the image center.

• The colored map in the preview shows the exact sampling layout: cyan top, green right, violet bottom, and amber left. Thin separators are individual zones and the inner edge is the effective depth. The map updates immediately and starts at the detected visible-content edge after black bars are excluded.

• Transition smoothing reduces rapid color changes. Fast 25 ms adds little inertia; 60 and 120 ms are progressively softer. Flash protection separately limits sudden increases in the average perimeter brightness over roughly 180 ms while allowing dimming immediately. Both filters cost negligible CPU time for at most 300 LEDs.

• 21:9 and 16:9 are sampled using the actual captured dimensions. Every side is normalized independently; switching 3440×1440 to 2560×1440 causes DXGI capture to reinitialize and does not require changing the LED layout.

• Black-bar handling has three modes. Do not exclude samples the full frame. First visible content scans all four sides and is useful for letterbox and pillarbox bars, but a very dark scene can move the edge inward after stabilization. Up to the 21:9 boundary scans only the top and bottom and caps the search at a 21:9 frame inside the captured image; it never shifts the side zones. The preview shows the resulting insets, calculated colors, LED 0, measured FPS, and processing time.)HELP")
            : QString::fromUtf8(R"HELP(Здесь выбираются источник изображения и способ расчёта цветов.

• «Экран захвата» определяет, изображение какого дисплея анализировать. «Автоматически» следует за основным экраном Windows; выберите телевизор явно, если он должен работать независимо. Нажмите «Обновить» после подключения или отключения дисплея.

• Способ захвата: выберите Windows Graphics Capture, если DXGI видит рабочий стол, но замирает в игре. Явно выберите экран с игрой, особенно при дублировании или нескольких дисплеях. Windows может показать рамку захвата. Оба способа поддерживают игровой режим, но нагрузка отличается. Захватывается дисплей, без внедрения в игру; некоторые игры и защищённое изображение могут оставаться недоступными. Диагностика сохраняет последнюю ошибку захвата даже после сворачивания игры.

• Разрешение захвата определяется автоматически и не редактируется.

• Игровой режим снижает нагрузку на GPU: до 30 FPS, 80 зон и 16 точек на зону со средним цветом. Копируются только краевые полосы. Поиск чёрных полос отключён: в игре с полосами сверху и снизу подсветка этих сторон может потемнеть. Снижаются плавность и детализация. Глубина, число LED, баланс белого и коррекция тёмных оттенков сохраняются. Выключение игрового режима возвращает обычные параметры обработки. Прирост FPS зависит от игры, видеокарты и расходов захвата Windows.

• Частота обработки — верхняя целевая частота захвата и анализа изображения. Фактическая передача может идти реже. Настройка не изменяет герцовку монитора. Для телевизора 60 Гц начните с 60 FPS.

• Цветовые зоны — логические участки по периметру, независимые от числа LED. Больше зон точнее сохраняет локальные переходы, но немного повышает нагрузку на GPU. Максимум — 300.

• Алгоритм цвета: «Среднее» даёт спокойный результат; «Среднеквадратичное» подчёркивает яркие детали при той же низкой нагрузке; «Доминирующий цвет» выразительнее, но может потребовать два прохода и пропускать мелкие детали.

• «Передача тёмных оттенков» компенсирует разницу между кодированием пикселей экрана в sRGB и почти линейным управлением мощностью LED через ШИМ. Рекомендуется «sRGB → LED»: этот вариант исправляет слишком светлые тени, не изменяя полный белый. Мягкая и сильная гамма позволяют подстроиться под ленту, стену и освещение комнаты; «Без коррекции» сохраняет поведение версий до 0.2.5. Дополнительная нагрузка на GPU пренебрежимо мала.

• Глубина от края определяет, насколько далеко внутрь изображения брать цвета. Для начала рекомендуется автоматический режим; снимите флажок, чтобы задать 8–512 px вручную. Большая глубина включает больше изображения ближе к центру и увеличивает выборку с 32 до 128 точек на зону. Фактическая область никогда не пересекает центр.

• Цветная карта в предпросмотре показывает реальную схему выборки: голубой — верх, зелёный — правая сторона, фиолетовый — низ, оранжевый — левая сторона. Тонкие разделители обозначают отдельные зоны, а внутренняя граница — фактическую глубину. Карта обновляется сразу и после исключения чёрных полос начинается от края видимого изображения.

• Сглаживание переходов уменьшает резкие изменения цветов. Быстрое 25 мс добавляет мало инерции; варианты 60 и 120 мс становятся последовательно мягче. Защита от вспышек отдельно ограничивает резкий рост средней яркости периметра примерно на 180 мс, но сразу пропускает затемнение. Для максимум 300 LED нагрузка обоих фильтров на CPU пренебрежимо мала.

• Форматы 21:9 и 16:9 обрабатываются по фактическому разрешению захвата. Каждая сторона нормализуется отдельно; при переходе с 3440×1440 на 2560×1440 DXGI-захват пересоздаётся, менять раскладку LED не требуется.

• Для чёрных полос есть три режима. «Не исключать» берёт весь кадр. «До первого видимого контента» проверяет все четыре стороны и подходит для letterbox- и pillarbox-полос, но после стабилизации может сдвинуть край внутрь на очень тёмной сцене. «До границы 21:9» проверяет только верх и низ и ограничивает поиск рамкой 21:9 внутри захваченного изображения; боковые зоны он никогда не сдвигает. Предпросмотр показывает итоговые отступы, цвета, LED 0, фактический FPS и время обработки.)HELP");
    } else if (page == audioPage_) {
        sectionName = text("Audio", "Звук");
        helpText = english
            ? QString::fromUtf8(R"HELP(This page configures the audio equalizer mode.

• Start equalizer enables lighting, stops a temporary animation or LED test, and selects the equalizer without leaving this page. While active, the same button returns to Ambilight. Recommended settings restore only spectrum processing and visualization, not devices, gains, strip layout, or connection.

• Windows audio captures everything played through the selected output device, including HDMI audio sent to a TV. Microphone captures only the selected input. The mixed mode opens both sources; their gain controls are independent.

• System default follows the current default Windows device. Choose a named device to keep using that exact output or microphone. Refresh after connecting USB, Bluetooth, HDMI, or another audio device.

• Frequency bands are independent of the LED count. The FFT divides the selected range logarithmically, which gives bass more useful space. Increasing the count produces finer bars but makes each one narrower.

• Sensitivity changes how strongly the spectrum reacts. The silence threshold prevents background noise from keeping LEDs on. Start with 100% and −55 dB.

• Attack controls how quickly a band lights up; release controls how slowly it fades. Short attack keeps beats responsive, while a longer release reduces flicker.

• Smooth dynamics follows those envelopes directly. Spring dynamics gives every frequency band a velocity: after a sharp change it overshoots, rebounds and settles. Springiness controls the size and duration of the diminishing wobble; 55% is a restrained starting point.

• Mirrored layout places bass at the bottom center and raises frequency in both directions toward the top. Perimeter layout runs low to high once around the strip. LED 0, direction and cyclic offset from the LED strip page are still applied afterward.

• Frequency gradient assigns a stable hue to each band. One color uses the selected color and varies only brightness. Global brightness, white balance and the estimated-current limit remain active.

The level meter verifies the selected source even without a connected ESP32. Audio is processed locally in memory and is not recorded or transmitted. The visualizer requires the PC; autonomous ESP32 effects remain available as the fallback.)HELP")
            : QString::fromUtf8(R"HELP(Здесь настраивается режим звукового эквалайзера.

• «Запустить эквалайзер» включает подсветку, завершает временную анимацию или проверку LED и выбирает эквалайзер, не уводя с этой вкладки. Во время работы та же кнопка возвращает Ambilight. «Рекомендуемые настройки» сбрасывают только обработку и отображение спектра, не меняя устройства, усиление, раскладку ленты и подключение.

• «Звуки Windows» захватывают всё, что воспроизводится через выбранное устройство вывода, включая HDMI-звук телевизора. «Микрофон» захватывает только выбранный вход. Смешанный режим открывает оба источника; усиление настраивается отдельно.

• «Системное по умолчанию» следует за текущим устройством Windows. Выберите устройство по имени, чтобы закрепить конкретный выход или микрофон. Нажмите «Обновить» после подключения USB, Bluetooth, HDMI или другого аудиоустройства.

• Частотные зоны не зависят от количества LED. FFT делит выбранный диапазон логарифмически, оставляя басам больше полезного места. Большее число зон делает спектр подробнее, но полосы становятся уже.

• Чувствительность определяет силу реакции спектра. Порог тишины не позволяет фоновому шуму постоянно подсвечивать ленту. Начните со 100% и −55 дБ.

• «Загорание» задаёт скорость роста зоны, «Затухание» — плавность её угасания. Короткое загорание сохраняет удары резкими, более длинное затухание уменьшает мерцание.

• «Плавная» динамика напрямую следует этим огибающим. «Пружинная» добавляет каждой частотной зоне скорость: после резкого изменения яркость перелетает цель, возвращается и успокаивается. «Пружинистость» задаёт силу и длительность затухающих колебаний; 55% — сдержанная отправная точка.

• Зеркальная раскладка помещает бас снизу по центру и повышает частоту в обе стороны к верху. Круговая раскладка проводит спектр от низких к высоким один раз по периметру. После этого по-прежнему применяются LED 0, направление и циклическое смещение со вкладки ленты.

• Частотный градиент закрепляет оттенок за каждой полосой. «Один цвет» меняет только яркость выбранного цвета. Общая яркость, баланс белого и расчётный лимит тока остаются активны.

Индикатор уровня помогает проверить выбранный источник даже без подключённой ESP32. Звук обрабатывается локально в памяти, не записывается и никуда не передаётся. Эквалайзеру нужен ПК; автономные эффекты ESP32 остаются резервной заглушкой.)HELP");
    } else if (page == ledPage_) {
        sectionName = text("LED strip", "Светодиодная лента");
        helpText = english
            ? QString::fromUtf8(R"HELP(This page describes the physical strip layout. Side names are viewed from the front of the screen.

• LED type currently supports WS2815. Color order corrects swapped channels; RGB is confirmed for the current prototype, but another strip may differ.

• Top, right, bottom, and left are the actual LED counts on each side. The total must be from 1 to 300.

• Cyclic offset places physical LED 0 on the logical screen perimeter. Direction tells where increasing LED numbers travel. Adjust both using the LED 0 marker in the preview—there is no need to remount the strip.

• Configure and test sides opens a compact window with one LED counter per side, the logical position of physical LED 0, strip direction, and a test-fill selector. The strip remains lit for the whole setup. Show all sides in different colors or fill one selected side with white; physical LED 0 is always white. Values apply immediately and their sum is limited to 1–300.

• WS2815 uses a nominal 12.0 V DC supply. Its documented absolute upper limit is 13.5 V, which is a damage boundary rather than a recommended setting. Keep the PSU adjusted to 12.0 V and never feed 12 V into the ESP32.

• Brightness is a fixed percentage applied to every RGB channel. The current limit is different: firmware estimates each frame after white balance and brightness, then dims it only if the entire strip would exceed the selected mA budget. White uses three channels and can hit the budget while red or a dark scene does not. Both limits apply together.

• White balance compensates for a cool or warm tint of the physical strip. Use Test white, leave the weakest channel at 100%, and reduce the excessive channels. Stop white test restores the selected mode. Reset returns all channels to 100%. Calibration affects Ambilight and standalone effects.

The software current limit does not replace a fuse, correctly sized wiring, or proper power distribution. Never feed 12 V directly into the ESP32.)HELP")
            : QString::fromUtf8(R"HELP(Здесь описывается физическое расположение ленты. Стороны указываются при взгляде на экран спереди.

• Сейчас поддерживается лента WS2815. Порядок цветов исправляет перепутанные каналы; для текущего прототипа подтверждён RGB, но другая лента может отличаться.

• Верхняя, правая, нижняя и левая стороны — фактическое число светодиодов на каждой стороне. Общая сумма должна быть от 1 до 300.

• Циклическое смещение задаёт положение физического LED 0 на периметре экрана. Направление показывает, куда растут номера LED. Настраивайте оба параметра по маркеру LED 0 в предпросмотре — переклеивать ленту не требуется.

• «Настроить и проверить стороны» открывает компактное окно со счётчиками LED, логическим положением физического LED 0, направлением ленты и выбором тестовой заливки. Лента горит всё время настройки. Можно показать стороны разными цветами либо залить одну выбранную сторону белым; физический LED 0 всегда отмечен белым. Значения применяются сразу, их сумма ограничена диапазоном 1–300.

• Номинальное питание WS2815 — 12,0 В постоянного тока. Указанный производителем абсолютный верхний предел 13,5 В — граница возможного повреждения, а не рекомендуемая настройка. Держите БП настроенным на 12,0 В и никогда не подавайте 12 В на ESP32.

• Яркость — постоянный процент, применяемый к каждому каналу RGB. Лимит тока работает иначе: прошивка оценивает каждый кадр после баланса белого и яркости, а затем уменьшает его только если вся лента превысила бы выбранный бюджет в мА. Белый использует три канала и может упереться в лимит, когда красный или тёмная сцена его не достигают. Оба ограничения действуют одновременно.

• «Баланс белого» компенсирует холодный или тёплый оттенок физической ленты. Нажмите «Проверить белый», оставьте самый слабый канал на 100% и уменьшайте избыточные. «Завершить проверку» возвращает выбранный режим. Кнопка сброса возвращает все каналы к 100%. Калибровка действует на Ambilight и автономные эффекты.

Программный лимит тока не заменяет предохранитель, подходящее сечение проводов и правильное распределение питания. Не подавайте 12 В напрямую на ESP32.)HELP");
    } else if (page == connectionPage_) {
        sectionName = text("Connection", "Подключение");
        helpText = english
            ? QString::fromUtf8(R"HELP(This page configures the USB connection to the ESP32.

• On first connection, select the controller’s USB-UART COM port manually. After verification, Automatic may reconnect only to that trusted device. If the USB-UART adapter has no reliable unique identity, keep using manual COM selection. Do not select a port belonging to unrelated hardware. Type-C is the connector; Windows still exposes the controller as a COM port.

• Refresh scans the port list after connecting or disconnecting the cable.

• Baud rate is fixed at 2,000,000 and must match the firmware.

• Stream timeout is how long the ESP32 waits without an Ambilight frame before entering the fallback effect. 1000 ms is a safe starting point.

• The connection indicator remains visible in the lower-right corner on every tab. Click it to return here. The status line reports discovery, connection, synchronization, and errors. If connection fails, first check the selected port, USB cable, and controller power.)HELP")
            : QString::fromUtf8(R"HELP(Здесь настраивается USB-соединение с ESP32.

• При первом подключении выберите COM-порт USB-UART контроллера вручную. После проверки «Автоматически» сможет переподключаться только к этому доверенному устройству. Если USB-UART не предоставляет надёжный уникальный идентификатор, оставьте ручной выбор COM. Не выбирайте порт постороннего оборудования. Type-C — это разъём; в Windows контроллер всё равно отображается как COM-порт.

• «Обновить» повторно сканирует список портов после подключения или отключения кабеля.

• Скорость порта зафиксирована на 2 000 000 бод и должна совпадать с прошивкой.

• Тайм-аут потока — сколько ESP32 ждёт новый кадр Ambilight перед переходом к резервному эффекту. 1000 мс — безопасная отправная точка.

• Индикатор подключения постоянно виден в правом нижнем углу на любой вкладке; нажмите его, чтобы вернуться сюда. Строка состояния показывает поиск, подключение, синхронизацию и ошибки. При проблеме сначала проверьте выбранный порт, USB-кабель и питание контроллера.)HELP");
    } else if (page == diagnosticsPage_) {
        sectionName = text("Diagnostics", "Диагностика");
        helpText = english
            ? QString::fromUtf8(R"HELP(This read-only page helps verify the system or prepare a bug report.

• Source, FPS, and GPU/readback show whether capture is running and keeping up.

• Transport TX shows communication with the ESP32. Replaced frames are not always an error: an outdated pending frame is intentionally replaced to keep latency low.

• Protocol, CRC, and dropped-frame counters should not keep increasing during a stable connection.

• Physical output confirms that the firmware is driving the strip when light is expected. Inactive is normal while lighting is disabled, the current mode is Off, or brightness is zero.

• Copy places the complete report on the clipboard. Review the listed COM port name before sharing it.)HELP")
            : QString::fromUtf8(R"HELP(Эта вкладка доступна только для чтения и помогает проверить систему или подготовить сообщение об ошибке.

• Источник, FPS и GPU/чтение показывают, работает ли захват и успевает ли обработка.

• Передача показывает обмен с ESP32. «Заменено кадров» не всегда означает ошибку: устаревший кадр намеренно заменяется свежим, чтобы не росла задержка.

• Счётчики ошибок протокола, CRC и отброшенных кадров не должны постоянно увеличиваться при стабильном соединении.

• «Физический выход» подтверждает работу ленты, когда ожидается свет. Неактивное состояние нормально при выключенной подсветке, режиме «Выключено» или нулевой яркости.

• «Копировать» помещает весь отчёт в буфер обмена. Перед отправкой проверьте указанное имя COM-порта.)HELP");
    } else {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(
        text("Help — %1", "Справка — %1").arg(sectionName));
    QSize minimumDialogSize(560, 420);
    QSize preferredDialogSize(680, 540);
    if (QScreen* const currentScreen = screen(); currentScreen != nullptr) {
        const QSize available =
            currentScreen->availableGeometry().size() - QSize(48, 48);
        minimumDialogSize = minimumDialogSize.boundedTo(available);
        preferredDialogSize = preferredDialogSize.boundedTo(available);
    }
    dialog.setMinimumSize(minimumDialogSize);
    dialog.resize(preferredDialogSize.expandedTo(minimumDialogSize));

    auto* layout = new QVBoxLayout(&dialog);
    auto* browser = new QTextBrowser(&dialog);
    browser->setPlainText(helpText);
    layout->addWidget(browser, 1);

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::updateWindowSizeForCurrentTab()
{
    QWidget* const currentPage = tabs_->currentWidget();
    if (currentPage == nullptr) {
        return;
    }
    const quint64 resizeGeneration = ++tabResizeGeneration_;
    if (lastSizedPage_ != nullptr) {
        if (!isMaximized() && !isFullScreen() && !isMinimized()) {
            tabWindowSizes_.insert(lastSizedPage_, size());
        } else if (normalGeometry().isValid()) {
            tabWindowSizes_.insert(
                lastSizedPage_,
                normalGeometry().size());
        }
    }
    lastSizedPage_ = nullptr;

    for (int index = 0; index < tabs_->count(); ++index) {
        QWidget* const page = tabs_->widget(index);
        const bool active = page == currentPage;
        page->setSizePolicy(
            active ? QSizePolicy::Preferred : QSizePolicy::Ignored,
            active ? QSizePolicy::Preferred : QSizePolicy::Ignored);
        page->updateGeometry();
    }
    tabs_->updateGeometry();
    if (centralWidget_->layout() != nullptr) {
        centralWidget_->layout()->invalidate();
        centralWidget_->layout()->activate();
    }

    QTimer::singleShot(
        0,
        this,
        [this, resizeGeneration, currentPage] {
            if (resizeGeneration != tabResizeGeneration_
                || tabs_->currentWidget() != currentPage) {
                return;
            }

            QWidget* const page = currentPage;
            QSize minimumSize(660, 460);
            QSize preferredSize(760, 560);
            if (page == generalPage_) {
                minimumSize = {660, 500};
                preferredSize = {760, 620};
            } else if (page == screenPage_) {
                minimumSize = {960, 620};
                preferredSize = {1180, 720};
            } else if (page == audioPage_) {
                minimumSize = {980, 650};
                preferredSize = {1180, 760};
            } else if (page == ledPage_) {
                minimumSize = {660, 500};
                preferredSize = {760, 620};
            } else if (page == connectionPage_) {
                minimumSize = {640, 420};
                preferredSize = {720, 500};
            } else if (page == diagnosticsPage_) {
                minimumSize = {660, 460};
                preferredSize = {820, 600};
            }

            minimumSize = minimumSize.expandedTo(minimumSizeHint());
            setMinimumSize(minimumSize);
            if (isMaximized() || isFullScreen() || isMinimized()) {
                return;
            }

            QSize targetSize =
                tabWindowSizes_.value(page, preferredSize);

            if (QScreen* const currentScreen = screen();
                currentScreen != nullptr) {
                const QSize available =
                    currentScreen->availableGeometry().size() - QSize(32, 32);
                targetSize.setWidth(
                    std::min(targetSize.width(), available.width()));
                targetSize.setHeight(
                    std::min(targetSize.height(), available.height()));
            }
            targetSize = targetSize.expandedTo(minimumSize);
            resize(targetSize);
            tabWindowSizes_.insert(page, size());
            lastSizedPage_ = page;
        });
}

void MainWindow::updateLedSummary()
{
    const int count = topLedsSpin_->value() + rightLedsSpin_->value()
        + bottomLedsSpin_->value() + leftLedsSpin_->value();
    const int minimumCurrentLimit =
        AppSettings::minimumCurrentLimitMilliAmps(count);
    ledOffsetSpin_->setMaximum(std::max(0, count - 1));
    currentLimitSpin_->setMinimum(minimumCurrentLimit);
    currentLimitSpin_->setToolTip(
        text("The minimum is %1 mA for %2 LEDs, allowing approximately "
             "3 mA idle current per LED.",
             "Минимум — %1 мА для %2 светодиодов с учётом примерно "
             "3 мА тока покоя на каждый.")
            .arg(minimumCurrentLimit)
            .arg(count));
    totalLedsLabel_->setText(
        text("Total LEDs: %1", "Всего светодиодов: %1").arg(count));
    if (ledPreview_ != nullptr) {
        ledPreview_->setLayout(
            topLedsSpin_->value(),
            rightLedsSpin_->value(),
            bottomLedsSpin_->value(),
            leftLedsSpin_->value());
        ledPreview_->setPhysicalMapping(
            ledOffsetSpin_->value(),
            comboValue(directionCombo_) == QStringLiteral("clockwise"));
        updateSamplingOverlay();
    }
}

int MainWindow::effectiveSamplingDepthPixels() const
{
    QSize frameSize(screenWidthSpin_->value(), screenHeightSpin_->value());
    capture::CaptureInsets contentInsets;
    if (capturedFrameSize_.isValid()) {
        frameSize = capturedFrameSize_;
        contentInsets = {
            .left = capturedContentInsets_.left(),
            .top = capturedContentInsets_.top(),
            .right = capturedContentInsets_.right(),
            .bottom = capturedContentInsets_.bottom(),
        };
    }
    return capture::resolvedSamplingDepthPixels(
        frameSize,
        contentInsets,
        samplingDepthAutoCheck_->isChecked()
            ? 0
            : samplingDepthSpin_->value());
}

void MainWindow::updateSamplingOverlay()
{
    if (ledPreview_ == nullptr) {
        return;
    }

    QSize sourceSize(screenWidthSpin_->value(), screenHeightSpin_->value());
    QMargins contentInsets;
    if (capturedFrameSize_.isValid()) {
        sourceSize = capturedFrameSize_;
        contentInsets = capturedContentInsets_;
    }
    const PerimeterLayout ledLayout{
        .top = topLedsSpin_->value(),
        .right = rightLedsSpin_->value(),
        .bottom = bottomLedsSpin_->value(),
        .left = leftLedsSpin_->value(),
    };
    const auto zones = distributeSamplingZones(
        gamingModeCheck_->isChecked()
            ? std::min(zonesSpin_->value(), capture::kGamingMaximumZones)
            : zonesSpin_->value(), ledLayout);
    ledPreview_->setSamplingOverlay(
        zones.top,
        zones.right,
        zones.bottom,
        zones.left,
        effectiveSamplingDepthPixels(),
        sourceSize,
        contentInsets);
}

void MainWindow::updateSamplingDepthUi()
{
    captureMethodLabel_->setText(text("Capture method:", "Способ захвата:"));
    captureMethodCombo_->setItemText(0, QStringLiteral("DXGI Desktop Duplication"));
    captureMethodCombo_->setItemText(1, QStringLiteral("Windows Graphics Capture"));
    captureMethodHintLabel_->setText(text(
        "If a game freezes DXGI capture, try Windows Graphics Capture and "
        "select the game's display explicitly. Windows may show a capture border. "
        "Game mode works with either method; capture overhead can differ.",
        "Если DXGI не видит игру, попробуйте Windows Graphics Capture и явно "
        "выберите экран с игрой. Windows может показывать рамку захвата. "
        "Игровой режим работает с обоими способами; нагрузка может отличаться."));
    const bool gaming = gamingModeCheck_->isChecked();
    gamingModeCheck_->setText(text("Game mode (lower GPU load)",
        "Игровой режим (меньше нагрузка на GPU)"));
    gamingModeHintLabel_->setText(text(
        "When enabled: up to 30 FPS, 80 zones, 16 points per zone, mean color. "
        "Only edge strips are copied; black-bar detection is off. "
        "Motion is less smooth. Your normal settings are preserved.",
        "При включении: до 30 FPS, 80 зон, 16 точек на зону, средний цвет. "
        "Копируются только края; поиск чёрных полос отключён. "
        "Движение менее плавное. Обычные настройки сохраняются."));
    if (gaming) {
        gamingModeHintLabel_->setText(gamingModeHintLabel_->text()
            + text(" Active limits: %1 FPS / %2 zones.",
                   " Сейчас: максимум %1 FPS / %2 зон.")
                  .arg(std::min(fpsSpin_->value(), capture::kGamingMaximumFps))
                  .arg(std::min(zonesSpin_->value(), capture::kGamingMaximumZones)));
    }
    blackBarModeCombo_->setEnabled(!gaming);
    regionColorModeCombo_->setEnabled(!gaming);
    samplingDepthSpin_->setEnabled(!samplingDepthAutoCheck_->isChecked());
    const int effectiveDepth = effectiveSamplingDepthPixels();
    const int depthSamples =
        gaming ? capture::kGamingDepthSamples : capture::adaptiveDepthSampleCount(effectiveDepth);
    const int uniqueSamples = capture::kSamplesAlongZone * depthSamples;
    updateRegionColorModeUi();
    updateSamplingOverlay();
    if (samplingDepthAutoCheck_->isChecked()) {
        samplingDepthHintLabel_->setText(
            text(
                "Current: %1 px and %2 points per zone (4 × %3). Recalculated "
                "after black-bar detection.",
                "Сейчас: %1 px и %2 точек на зону (4 × %3). Пересчитывается "
                "после обнаружения чёрных полос.")
                .arg(effectiveDepth)
                .arg(uniqueSamples)
                .arg(depthSamples));
        return;
    }

    const int requestedDepth = samplingDepthSpin_->value();
    if (effectiveDepth < requestedDepth) {
        samplingDepthHintLabel_->setText(
            text(
                "Requested: %1 px; effective: %2 px because the content "
                "center is closer. %3 points per zone (4 × %4).",
                "Задано: %1 px; фактически: %2 px, потому что центр контента "
                "ближе. %3 точек на зону (4 × %4).")
                .arg(requestedDepth)
                .arg(effectiveDepth)
                .arg(uniqueSamples)
                .arg(depthSamples));
        return;
    }

    samplingDepthHintLabel_->setText(
        text(
            "%1 px inward from each content edge; %2 points per zone "
            "(4 × %3).",
            "%1 px от каждого края контента; %2 точек на зону (4 × %3).")
            .arg(effectiveDepth)
            .arg(uniqueSamples)
            .arg(depthSamples));
}

void MainWindow::updateRegionColorModeUi()
{
    const int uniqueSamples =
        gamingModeCheck_->isChecked() ? 16 : capture::samplesPerZone(effectiveSamplingDepthPixels());
    const QString mode = gamingModeCheck_->isChecked()
        ? QStringLiteral("mean") : comboValue(regionColorModeCombo_);
    QString hint;
    if (mode == QStringLiteral("rms")) {
        hint = text(
            "%1 points per zone. Low GPU load; bright details influence the "
            "lighting more strongly.",
            "%1 точек на зону. Низкая нагрузка на GPU; яркие детали сильнее "
            "влияют на подсветку.")
                   .arg(uniqueSamples);
    } else if (mode == QStringLiteral("dominant")) {
        hint = text(
            "Up to %1 texture reads per zone. Increased GPU load; the most "
            "common color group wins.",
            "До %1 чтений текстуры на зону. Повышенная нагрузка на GPU; "
            "выбирается самая распространённая группа цветов.")
                   .arg(uniqueSamples * 2);
    } else {
        hint = text(
            "%1 points per zone. Low GPU load; stable blending of mixed "
            "colors.",
            "%1 точек на зону. Низкая нагрузка на GPU; стабильное смешивание "
            "цветов.")
                   .arg(uniqueSamples);
    }
    regionColorModeHintLabel_->setText(hint);
    regionColorModeCombo_->setToolTip(hint);
}

void MainWindow::updateAudioUi()
{
    audioMinimumFrequencySpin_->setMaximum(std::min(
        2'000, audioMaximumFrequencySpin_->value() - 1));
    audioMaximumFrequencySpin_->setMinimum(std::max(
        1'000, audioMinimumFrequencySpin_->value() + 1));

    const QString source = comboValue(audioSourceCombo_);
    const bool usesOutput = source == QStringLiteral("windows")
        || source == QStringLiteral("mixed");
    const bool usesInput = source == QStringLiteral("microphone")
        || source == QStringLiteral("mixed");
    audioOutputDeviceCombo_->setEnabled(usesOutput);
    audioOutputGainSpin_->setEnabled(usesOutput);
    audioInputDeviceCombo_->setEnabled(usesInput);
    audioMicrophoneGainSpin_->setEnabled(usesInput);
    audioSolidColorButton_->setEnabled(
        comboValue(audioColorModeCombo_) == QStringLiteral("solid"));
    const bool springDynamics =
        comboValue(audioDynamicsCombo_) == QStringLiteral("spring");
    audioSpringinessLabel_->setEnabled(springDynamics);
    audioSpringinessSpin_->setEnabled(springDynamics);
    audioStartButton_->setText(
        audioShouldBeActive()
            ? text("Return to Ambilight", "Вернуться к Ambilight")
            : text("Start equalizer", "Запустить эквалайзер"));

    QString state;
    switch (audioStatus_.state) {
    case audio::AudioState::Stopped:
        if (!lightingEnabledCheck_->isChecked()) {
            state = text(
                "Ready — enable lighting to start the equalizer",
                "Готово — включите подсветку для запуска эквалайзера");
        } else if (comboValue(activeModeCombo_)
            != QStringLiteral("equalizer")) {
            state = text(
                "Ready — select Audio equalizer on the General page",
                "Готово — выберите «Эквалайзер» на вкладке «Основное»");
        } else if (manualAnimationButton_->isChecked()
            || testWhiteBalanceButton_->isChecked()
            || layoutTestButton_->isChecked()) {
            state = text(
                "Equalizer paused during animation or LED test",
                "Эквалайзер приостановлен на время анимации или проверки LED");
        } else {
            state = text("Audio capture stopped", "Захват звука остановлен");
        }
        break;
    case audio::AudioState::Starting:
        state = text("Starting audio capture", "Запуск захвата звука");
        break;
    case audio::AudioState::Running:
        state = text("Audio equalizer active", "Эквалайзер активен");
        break;
    case audio::AudioState::Error:
        state = text("Audio capture error", "Ошибка захвата звука");
        break;
    }
    if (audioStatus_.state == audio::AudioState::Running) {
        state += text(" — %1 FPS, peak %2%", " — %1 FPS, пик %2%")
                     .arg(measuredAudioFps_, 0, 'f', 1)
                     .arg(static_cast<int>(std::lround(audioPeakLevel_ * 100.0)));
    } else if (!audioStatus_.message.isEmpty()
        && audioStatus_.state == audio::AudioState::Error) {
        state += QStringLiteral(": ") + audioStatus_.message;
    }
    audioStatusLabel_->setText(state);
    audioStatusLabel_->setStyleSheet(
        audioStatus_.state == audio::AudioState::Error
            ? QStringLiteral("color: #c62828;")
            : QString {});
    audioPreview_->setStatus(
        state, audioStatus_.state == audio::AudioState::Running);
}

void MainWindow::updateAudioColorButton()
{
    QColor color(audioSolidColorButton_->property("audioColor").toString());
    if (!color.isValid()) {
        color = QColor(0, 170, 255);
        audioSolidColorButton_->setProperty(
            "audioColor", color.name(QColor::HexRgb));
    }
    const int luminance = color.red() * 299
        + color.green() * 587 + color.blue() * 114;
    const QString foreground = luminance >= 128'000
        ? QStringLiteral("#000000")
        : QStringLiteral("#ffffff");
    audioSolidColorButton_->setStyleSheet(
        QStringLiteral("background-color: %1; color: %2;")
            .arg(color.name(QColor::HexRgb), foreground));
    audioSolidColorButton_->setToolTip(color.name(QColor::HexRgb));
}

void MainWindow::updateSolidColorButton()
{
    QColor color(solidColorButton_->property("solidColor").toString());
    if (!color.isValid()) {
        color = QColor(255, 96, 24);
        solidColorButton_->setProperty(
            "solidColor", color.name(QColor::HexRgb));
    }
    const int luminance = color.red() * 299
        + color.green() * 587 + color.blue() * 114;
    const QString foreground = luminance >= 128'000
        ? QStringLiteral("#000000")
        : QStringLiteral("#ffffff");
    solidColorButton_->setStyleSheet(
        QStringLiteral("background-color: %1; color: %2;")
            .arg(color.name(QColor::HexRgb), foreground));
    solidColorButton_->setToolTip(color.name(QColor::HexRgb));
}

void MainWindow::updateHotkeyStatusUi()
{
    QString statusText;
    bool isError = false;
    switch (hotkeyStatus_) {
    case HotkeyStatus::Disabled:
        statusText = text(
            "Global hotkey is disabled.",
            "Глобальная горячая клавиша отключена.");
        break;
    case HotkeyStatus::Active:
        statusText = text(
            "Global hotkey is active.",
            "Глобальная горячая клавиша активна.");
        break;
    case HotkeyStatus::InvalidSequence:
        statusText = text(
            "Not applied. Use one key with at least one Ctrl, Alt, Shift, or "
            "Win modifier. The active shortcut was not changed.",
            "Не применено. Используйте одну клавишу хотя бы с одним "
            "модификатором Ctrl, Alt, Shift или Win. Активное сочетание "
            "не изменено.");
        isError = true;
        break;
    case HotkeyStatus::Unavailable:
        statusText = text(
            "Not applied. This shortcut may be used by Windows or another "
            "application. The active shortcut was not changed.",
            "Не применено. Это сочетание может быть занято Windows или "
            "другой программой. Активное сочетание не изменено.");
        isError = true;
        break;
    case HotkeyStatus::Unsupported:
        statusText = text(
            "Global hotkeys are supported only on Windows.",
            "Глобальные горячие клавиши поддерживаются только в Windows.");
        isError = true;
        break;
    }

    hotkeyStatusLabel_->setText(statusText);
    hotkeyStatusLabel_->setStyleSheet(
        isError ? QStringLiteral("color: #c62828;") : QString {});
}

void MainWindow::updateDiagnostics()
{
    // Keep the source data current, but do not rebuild a hidden text document.
    // Opening the tab/window explicitly refreshes it even on a static desktop.
    if (!diagnosticsPage_->isVisible() || isMinimized()) return;

    const int ledCount = topLedsSpin_->value() + rightLedsSpin_->value()
        + bottomLedsSpin_->value() + leftLedsSpin_->value();
    const QString hotkey = registeredHotkey_.isEmpty()
        ? text("not registered", "не зарегистрирована")
        : registeredHotkey_.toString(QKeySequence::NativeText);
    const QSize sourceSize = capturedFrameSize_.isValid()
        ? capturedFrameSize_
        : QSize(screenWidthSpin_->value(), screenHeightSpin_->value());
    const QString captureState = captureStatusText_.isEmpty()
        ? text("not started", "не запущен")
        : captureStatusText_;

    QStringList lines;
    lines
        << QStringLiteral("Ambilight by Levurgi4")
        << text("Version: %1", "Версия: %1")
               .arg(QCoreApplication::applicationVersion())
        << text("Lighting: %1", "Подсветка: %1")
               .arg(lightingEnabledCheck_->isChecked()
                       ? text("enabled", "включена")
                       : text("disabled", "выключена"))
        << text("Capture display: %1", "Экран захвата: %1")
               .arg(captureOutputCombo_->currentText())
        << text("Capture state: %1", "Состояние захвата: %1")
               .arg(captureState)
        << text("Capture method: %1", "Способ захвата: %1")
               .arg(captureMethodCombo_->currentText())
        << text("Last capture error this session: %1", "Последняя ошибка захвата за сеанс: %1")
               .arg(lastCaptureError_.isEmpty() ? text("none", "нет") : lastCaptureError_)
        << text("Source: %1 × %2; target %3 FPS",
               "Источник: %1 × %2; цель %3 FPS")
               .arg(sourceSize.width())
               .arg(sourceSize.height())
               .arg(gamingModeCheck_->isChecked()
                    ? std::min(fpsSpin_->value(), capture::kGamingMaximumFps)
                    : fpsSpin_->value())
        << text("Game mode: %1", "Игровой режим: %1")
               .arg(gamingModeCheck_->isChecked()
                    ? text("enabled", "включён") : text("disabled", "выключен"))
        << text("Measured: %1 FPS; GPU/readback %2 ms; frame %3",
               "Измерено: %1 FPS; GPU/чтение %2 мс; кадр %3")
               .arg(measuredCaptureFps_, 0, 'f', 1)
               .arg(captureProcessingMilliseconds_, 0, 'f', 2)
               .arg(capturedFrameNumber_)
        << text("Content insets: L %1 / T %2 / R %3 / B %4 px",
               "Отступы контента: слева %1 / сверху %2 / справа %3 / снизу %4 px")
               .arg(capturedContentInsets_.left())
               .arg(capturedContentInsets_.top())
               .arg(capturedContentInsets_.right())
               .arg(capturedContentInsets_.bottom())
        << text("Black bars: %1", "Чёрные полосы: %1")
               .arg(gamingModeCheck_->isChecked()
                    ? text("off (game mode)", "отключено (игровой режим)")
                    : blackBarModeCombo_->currentText())
        << text("GPU sampling zones: %1", "Зоны выборки GPU: %1")
               .arg(gamingModeCheck_->isChecked()
                    ? std::min(zonesSpin_->value(), capture::kGamingMaximumZones)
                    : zonesSpin_->value())
        << text(
               "Region processing: %1; %2 source points per zone",
               "Обработка области: %1; %2 исходных точек на зону")
               .arg(gamingModeCheck_->isChecked()
                    ? text("Mean (game mode)", "Среднее (игровой режим)")
                    : regionColorModeCombo_->currentText())
               .arg(gamingModeCheck_->isChecked() ? 16 : capture::samplesPerZone(
                   effectiveSamplingDepthPixels()))
        << text("Dark-tone transfer: %1", "Передача тёмных оттенков: %1")
               .arg(ledTransferModeCombo_->currentText())
        << text("Capture depth: %1 px (%2)",
               "Глубина захвата: %1 px (%2)")
               .arg(effectiveSamplingDepthPixels())
               .arg(
                   samplingDepthAutoCheck_->isChecked()
                       ? text("automatic", "автоматически")
                       : text("manual", "вручную"))
        << text("Temporal filter: %1; flash protection %2",
               "Временной фильтр: %1; защита от вспышек %2")
               .arg(
                   smoothingCombo_->currentText(),
                   flashProtectionCheck_->isChecked()
                       ? text("enabled", "включена")
                       : text("disabled", "выключена"))
        << text("Manual animation: %1",
               "Ручная анимация: %1")
               .arg(
                   manualAnimationButton_->isChecked()
                       ? text("active", "активна")
                       : text("inactive", "неактивна"))
        << text("LED strip: %1, %2 LEDs, %3 order",
               "Лента: %1, %2 светодиодов, порядок %3")
               .arg(comboValue(ledTypeCombo_))
               .arg(ledCount)
               .arg(comboValue(colorOrderCombo_))
        << text("Layout: top %1 / right %2 / bottom %3 / left %4",
               "Раскладка: верх %1 / справа %2 / низ %3 / слева %4")
               .arg(topLedsSpin_->value())
               .arg(rightLedsSpin_->value())
               .arg(bottomLedsSpin_->value())
               .arg(leftLedsSpin_->value())
        << text("Physical order: LED 0 at %1, %2",
               "Физический порядок: LED 0 на позиции %1, %2")
               .arg(ledOffsetSpin_->value())
               .arg(
                   comboValue(directionCombo_) == QStringLiteral("clockwise")
                       ? text("clockwise", "по часовой стрелке")
                       : text("counterclockwise", "против часовой стрелки"))
        << text("White balance: R %1% / G %2% / B %3%",
               "Баланс белого: R %1% / G %2% / B %3%")
               .arg(redCalibrationSpin_->value())
               .arg(greenCalibrationSpin_->value())
               .arg(blueCalibrationSpin_->value())
        << text("Connection: %1 @ %2 baud",
               "Подключение: %1 @ %2 бод")
               .arg(
                   serialPortValue().isEmpty()
                       ? text("automatic COM selection",
                           "автоматический выбор COM")
                       : serialPortValue(),
                   QString::number(baudCombo_->currentData().toInt()))
        << text("Serial state: %1", "Состояние порта: %1")
               .arg(transportStateText())
        << text("Stream timeout: %1 ms", "Тайм-аут потока: %1 мс")
               .arg(ambientTimeoutSpin_->value())
        << text("Global hotkey: %1", "Глобальная клавиша: %1").arg(hotkey)
        << text("Transport TX: %1 frames / %2 bytes; replaced %3",
               "Передача: %1 кадров / %2 байт; заменено %3")
               .arg(transportStatistics_.transmittedFrames)
               .arg(transportStatistics_.transmittedBytes)
               .arg(transportStatistics_.replacedFrames)
        << text("Transport RX: %1 bytes; protocol errors %2",
               "Приём: %1 байт; ошибок протокола %2")
               .arg(transportStatistics_.receivedBytes)
               .arg(transportStatistics_.protocolErrors);

    lines
        << text("Selected mode: %1", "Выбранный режим: %1")
               .arg(activeModeCombo_->currentText())
        << text("Equalizer: %1", "Эквалайзер: %1")
               .arg(audioStatusLabel_->text())
        << text("Audio source: %1; output %2; microphone %3",
               "Источник звука: %1; вывод %2; микрофон %3")
               .arg(
                   audioSourceCombo_->currentText(),
                   audioOutputDeviceCombo_->currentText(),
                   audioInputDeviceCombo_->currentText())
        << text("Equalizer dynamics: %1; springiness %2%",
               "Динамика эквалайзера: %1; пружинистость %2%")
               .arg(audioDynamicsCombo_->currentText())
               .arg(audioSpringinessSpin_->value());

    if (!transportStatus_.message.isEmpty()) {
        lines << text("Transport detail: %1", "Подробности соединения: %1")
                     .arg(transportStatus_.message);
    }
    if (deviceInformation_.protocolVersion > 0) {
        const bool physicalOutputActive =
            (deviceState_.flags & protocol::StateFlagOutputActive) != 0;
        const bool ambientLeaseActive =
            (deviceState_.flags
                & protocol::StateFlagAmbientLeaseActive) != 0;
        lines
            << text("Firmware: %1.%2.%3; protocol %4; maximum %5 LEDs",
                   "Прошивка: %1.%2.%3; протокол %4; максимум %5 LED")
                   .arg(deviceInformation_.firmwareMajor)
                   .arg(deviceInformation_.firmwareMinor)
                   .arg(deviceInformation_.firmwarePatch)
                   .arg(deviceInformation_.protocolVersion)
                   .arg(deviceInformation_.maximumLedCount)
            << text("ESP frames: received %1 / applied %2 / dropped %3; CRC %4",
                   "Кадры ESP: принято %1 / применено %2 / отброшено %3; CRC %4")
                    .arg(deviceState_.lastReceivedSequence)
                    .arg(deviceState_.lastAppliedSequence)
                    .arg(deviceState_.droppedFrames)
                    .arg(deviceState_.crcErrors)
            << text("Physical output: %1; ambient lease: %2",
                   "Физический выход: %1; аренда Ambilight: %2")
                   .arg(
                       physicalOutputActive
                           ? text("active", "активен")
                           : text("inactive", "неактивен"),
                       ambientLeaseActive
                           ? text("active", "активна")
                           : text("inactive", "неактивна"));
        const bool outputExpected =
            deviceState_.mode
                != static_cast<int>(protocol::OperatingMode::Off)
            && deviceState_.brightness > 0;
        if (outputExpected && !physicalOutputActive) {
            lines << text(
                "Output warning: physical output is unavailable or RMT "
                "recovery is in progress.",
                "Предупреждение выхода: физический выход недоступен либо "
                "выполняется восстановление RMT.");
        }
    }
    if (!serialPortEnumerationError_.isEmpty()) {
        lines << text("COM enumeration: %1", "Поиск COM: %1")
                     .arg(serialPortEnumerationError_);
    }
    if (!audioDeviceEnumerationError_.isEmpty()) {
        lines << text("Audio device enumeration: %1",
                     "Поиск аудиоустройств: %1")
                     .arg(audioDeviceEnumerationError_);
    }
    if (!lastTransportCommandError_.isEmpty()) {
        lines << text("Last controller error: %1",
                    "Последняя ошибка контроллера: %1")
                     .arg(lastTransportCommandError_);
    }
    diagnosticsText_->setPlainText(lines.join(QLatin1Char('\n')));
}

void MainWindow::updateEnabledState()
{
    if (toggleLightingAction_ != nullptr) {
        toggleLightingAction_->setText(
            lightingEnabledCheck_->isChecked()
                ? text("Turn lighting off", "Выключить подсветку")
                : text("Turn lighting on", "Включить подсветку"));
    }
    updateAudioUi();
}

void MainWindow::updateLedTestUi()
{
    const bool connected =
        transportStatus_.state == transport::ConnectionState::Connected;
    const bool whiteTest = testWhiteBalanceButton_->isChecked();
    const bool layoutTest = layoutTestButton_->isChecked();

    testWhiteBalanceButton_->setText(
        whiteTest
            ? text("Stop white test", "Завершить проверку")
            : text("Test white", "Проверить белый"));
    layoutTestButton_->setText(
        layoutTest
            ? text("Configuring sides...", "Настройка сторон...")
            : text("Configure and test sides", "Настроить и проверить стороны"));

    if (!connected) {
        ledTestHintLabel_->setText(
            whiteTest || layoutTest
                ? text(
                    "Waiting for ESP32; the test will start after connection.",
                    "Ожидание ESP32 — проверка начнётся после подключения.")
                : text(
                    "ESP32 is not connected. Values are saved but cannot yet be applied to the strip.",
                    "ESP32 не подключена. Значения сохраняются, но пока не применяются к ленте."));
    } else if (layoutTest) {
        ledTestHintLabel_->setText(text(
            "Top: red, right: green, bottom: blue, left: amber; physical LED 0 is white.",
            "Верх — красный, справа — зелёный, низ — синий, слева — оранжевый; физический LED 0 — белый."));
    } else if (whiteTest) {
        ledTestHintLabel_->setText(text(
            "White test is active; R/G/B changes are applied immediately.",
            "Проверка белого активна — изменения R/G/B применяются сразу."));
    } else {
        ledTestHintLabel_->setText(text(
            "ESP32 is connected; strip settings are applied immediately.",
            "ESP32 подключена — настройки ленты применяются сразу."));
    }
}

void MainWindow::showLedLayoutDialog()
{
    if (!layoutTestButton_->isChecked()) {
        return;
    }

    layoutTestPattern_ = LayoutTestPattern::AllSides;

    QDialog dialog(this);
    dialog.setWindowTitle(text(
        "Configure LED sides",
        "Настройка сторон ленты"));
    dialog.setModal(true);
    dialog.setMinimumWidth(470);

    auto* dialogLayout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(
        text(
            "Use the arrows or type the number of LEDs on each side. The test stays lit while this window is open. Show all sides in different colors or select one side to fill it with white.",
            "Введите количество LED на каждой стороне или используйте стрелки. Пока окно открыто, тест горит постоянно. Можно показать все стороны разными цветами либо залить выбранную сторону белым."),
        &dialog);
    explanation->setWordWrap(true);
    dialogLayout->addWidget(explanation);

    auto* form = new QFormLayout();
    const std::array<QSpinBox*, 4> sourceSpins {
        topLedsSpin_, rightLedsSpin_, bottomLedsSpin_, leftLedsSpin_};
    const std::array<QString, 4> sideNames {
        text("Top:", "Верх:"),
        text("Right:", "Справа:"),
        text("Bottom:", "Низ:"),
        text("Left:", "Слева:")};
    std::array<QSpinBox*, 4> dialogSpins {};
    for (std::size_t index = 0; index < dialogSpins.size(); ++index) {
        auto* spin = makeSpinBox(&dialog, 0, 300, QStringLiteral(" LED"));
        spin->setValue(sourceSpins[index]->value());
        spin->setMaximumWidth(140);
        dialogSpins[index] = spin;
        form->addRow(sideNames[index], spin);
    }

    auto* offsetSpin = makeSpinBox(
        &dialog,
        0,
        std::max(0, ledOffsetSpin_->maximum()),
        QStringLiteral(" LED"));
    offsetSpin->setValue(ledOffsetSpin_->value());
    offsetSpin->setMaximumWidth(140);
    form->addRow(text("LED 0 position:", "Положение LED 0:"), offsetSpin);

    auto* directionCombo = new QComboBox(&dialog);
    directionCombo->addItem(
        text("Clockwise", "По часовой стрелке"),
        QStringLiteral("clockwise"));
    directionCombo->addItem(
        text("Counterclockwise", "Против часовой стрелки"),
        QStringLiteral("counterclockwise"));
    directionCombo->setCurrentIndex(std::max(
        0,
        directionCombo->findData(comboValue(directionCombo_))));
    directionCombo->setMaximumWidth(240);
    form->addRow(text("Direction:", "Направление:"), directionCombo);

    auto* patternCombo = new QComboBox(&dialog);
    patternCombo->addItem(
        text("All sides (different colors)", "Все стороны (разные цвета)"),
        static_cast<int>(LayoutTestPattern::AllSides));
    patternCombo->addItem(
        text("Top only (white)", "Только верх (белый)"),
        static_cast<int>(LayoutTestPattern::Top));
    patternCombo->addItem(
        text("Right only (white)", "Только справа (белый)"),
        static_cast<int>(LayoutTestPattern::Right));
    patternCombo->addItem(
        text("Bottom only (white)", "Только низ (белый)"),
        static_cast<int>(LayoutTestPattern::Bottom));
    patternCombo->addItem(
        text("Left only (white)", "Только слева (белый)"),
        static_cast<int>(LayoutTestPattern::Left));
    patternCombo->addItem(
        text("Physical LED 0 only", "Только физический LED 0"),
        static_cast<int>(LayoutTestPattern::LedZero));
    patternCombo->setMaximumWidth(280);
    form->addRow(text("Test fill:", "Тестовая заливка:"), patternCombo);
    dialogLayout->addLayout(form);

    auto* totalLabel = new QLabel(&dialog);
    totalLabel->setStyleSheet(QStringLiteral("font-weight: 600;"));
    dialogLayout->addWidget(totalLabel);

    const auto updateDialogState = [&] {
        int total = 0;
        for (const auto* spin : dialogSpins) {
            total += spin->value();
        }
        for (auto* spin : dialogSpins) {
            const int otherSides = total - spin->value();
            const QSignalBlocker blocker(spin);
            spin->setRange(otherSides == 0 ? 1 : 0, 300 - otherSides);
        }
        {
            const QSignalBlocker blocker(offsetSpin);
            offsetSpin->setMaximum(std::max(0, total - 1));
            offsetSpin->setValue(ledOffsetSpin_->value());
        }
        totalLabel->setText(text(
            "Total: %1 of 300 LEDs",
            "Всего: %1 из 300 LED").arg(total));
    };

    for (std::size_t index = 0; index < dialogSpins.size(); ++index) {
        connect(
            dialogSpins[index],
            &QSpinBox::valueChanged,
            &dialog,
            [&, index](const int value) {
                sourceSpins[index]->setValue(value);
                for (std::size_t syncIndex = 0;
                     syncIndex < dialogSpins.size();
                     ++syncIndex) {
                    const QSignalBlocker blocker(dialogSpins[syncIndex]);
                    dialogSpins[syncIndex]->setValue(
                        sourceSpins[syncIndex]->value());
                }
                updateDialogState();
            });
    }
    updateDialogState();

    connect(
        offsetSpin,
        &QSpinBox::valueChanged,
        &dialog,
        [this](const int value) {
            ledOffsetSpin_->setValue(value);
        });
    connect(
        directionCombo,
        &QComboBox::currentIndexChanged,
        &dialog,
        [this, directionCombo] {
            selectComboValue(directionCombo_, directionCombo->currentData().toString());
        });
    connect(
        patternCombo,
        &QComboBox::currentIndexChanged,
        &dialog,
        [this, patternCombo] {
            layoutTestPattern_ = static_cast<LayoutTestPattern>(
                patternCombo->currentData().toInt());
            submitLayoutTestFrame();
        });

    auto* mappingHint = new QLabel(
        text(
            "The white pixel is always the physical LED 0 immediately after DI; changing its logical position does not move this physical pixel. Adjust LED 0 position and direction until the illuminated side boundaries coincide with the TV sides.",
            "Белая точка — это всегда физический LED 0 сразу после DI; изменение его логического положения не перемещает этот диод по ленте. Меняйте положение LED 0 и направление, пока границы подсвеченных участков не совпадут со сторонами телевизора."),
        &dialog);
    mappingHint->setWordWrap(true);
    dialogLayout->addWidget(mappingHint);

    auto* limitHint = new QLabel(
        text(
            "At least one LED is required; the application currently supports no more than 300 in total.",
            "Нужен хотя бы один LED; сейчас приложение поддерживает не более 300 LED суммарно."),
        &dialog);
    limitHint->setWordWrap(true);
    dialogLayout->addWidget(limitHint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* doneButton = buttons->button(QDialogButtonBox::Close);
    doneButton->setText(text("Done", "Готово"));
    doneButton->setAutoDefault(false);
    doneButton->setDefault(false);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    dialogLayout->addWidget(buttons);

    dialogSpins.front()->setFocus();
    dialog.exec();

    layoutTestPattern_ = LayoutTestPattern::AllSides;

    if (layoutTestButton_->isChecked()) {
        layoutTestButton_->setChecked(false);
    }
}

void MainWindow::updateManualAnimationUi()
{
    const bool active = manualAnimationButton_->isChecked();
    const bool animationSelected =
        comboValue(standaloneEffectCombo_) != QStringLiteral("off");
    manualAnimationButton_->setEnabled(
        lightingEnabledCheck_->isChecked() && animationSelected);
    manualAnimationButton_->setText(
        active
            ? text("Stop animation", "Остановить анимацию")
            : text("Start animation", "Запустить анимацию"));
}

void MainWindow::updateConnectionShortcut()
{
    const bool connected =
        transportStatus_.state == transport::ConnectionState::Connected;
    QString label = connected
        ? text("ESP32: connected", "ESP32: подключена")
        : text("ESP32: no connection", "ESP32: нет связи");
    if (connected && !transportStatus_.portName.isEmpty()) {
        label += QStringLiteral(" (") + transportStatus_.portName
            + QLatin1Char(')');
    }
    connectionShortcutButton_->setText(label);
    connectionShortcutButton_->setToolTip(
        transportStatus_.message.isEmpty()
            ? text(
                "Open connection settings.",
                "Открыть настройки подключения.")
            : transportStatus_.message);
}

void MainWindow::refreshCaptureOutputs()
{
    QString errorMessage;
    const auto outputs =
        capture::ScreenCaptureWorker::enumerateOutputs(&errorMessage);
    applyCaptureOutputs(outputs, errorMessage);
    if (!outputs.isEmpty()) {
        statusBar()->showMessage(
            text("Display list refreshed.", "Список экранов обновлён."),
            2000);
    }
    scheduleCaptureUpdate();
}

void MainWindow::applyCaptureOutputs(
    const QVector<capture::CaptureOutput>& outputs,
    const QString& errorMessage)
{
    captureOutputs_ = outputs;
    captureOutputEnumerationError_ = errorMessage;
    const QString selectedId = captureOutputCombo_->count() > 0
        ? comboValue(captureOutputCombo_)
        : QString {};

    const QSignalBlocker blocker(captureOutputCombo_);
    captureOutputCombo_->clear();
    captureOutputCombo_->addItem(
        text("Automatic (primary display)", "Автоматически (основной экран)"),
        QString {});
    const auto automaticOutput = std::find_if(
        outputs.cbegin(),
        outputs.cend(),
        [](const capture::CaptureOutput& output) {
            return output.primary;
        });
    if (automaticOutput != outputs.cend()) {
        captureOutputCombo_->setItemData(
            0,
            automaticOutput->geometry.size(),
            kCaptureOutputSizeRole);
    } else if (!outputs.isEmpty()) {
        captureOutputCombo_->setItemData(
            0,
            outputs.constFirst().geometry.size(),
            kCaptureOutputSizeRole);
    }

    for (const auto& output : outputs) {
        QString label = output.label;
        if (output.primary) {
            label += text(" — primary", " — основной");
        }
        captureOutputCombo_->addItem(label, output.id);
        const int index = captureOutputCombo_->count() - 1;
        captureOutputCombo_->setItemData(
            index,
            output.geometry.size(),
            kCaptureOutputSizeRole);
        captureOutputCombo_->setItemData(
            index,
            output.legacyId,
            kCaptureOutputLegacyIdRole);
    }

    int selectedIndex = captureOutputCombo_->findData(selectedId);
    if (selectedIndex < 0 && !selectedId.isEmpty()) {
        for (int index = 1; index < captureOutputCombo_->count(); ++index) {
            if (capture::captureOutputIdEquals(
                    captureOutputCombo_
                        ->itemData(index, kCaptureOutputLegacyIdRole)
                        .toString(),
                    selectedId)) {
                selectedIndex = index;
                break;
            }
        }
    }
    if (selectedIndex < 0 && !selectedId.isEmpty()) {
        captureOutputCombo_->addItem(
            text("Unavailable display (%1)", "Недоступный экран (%1)")
                .arg(selectedId),
            selectedId);
        selectedIndex = captureOutputCombo_->count() - 1;
    }
    captureOutputCombo_->setCurrentIndex(std::max(0, selectedIndex));
    {
        const QSignalBlocker sourceBlocker(mirrorSourceCombo_), targetBlocker(mirrorTargetCombo_);
        const QString targetId = mirrorTargetCombo_->currentData().toString();
        mirrorSourceCombo_->clear();
        mirrorTargetCombo_->clear();
        for (int i = 0; i < captureOutputCombo_->count(); ++i)
            mirrorSourceCombo_->addItem(captureOutputCombo_->itemText(i), captureOutputCombo_->itemData(i));
        for (const auto& output : outputs) mirrorTargetCombo_->addItem(output.label, output.id);
        int index = mirrorTargetCombo_->findData(targetId);
        if (index < 0 && !targetId.isEmpty()) {
            mirrorTargetCombo_->addItem(text("Unavailable display (%1)", "Недоступный экран (%1)")
                .arg(targetId), targetId);
            index = mirrorTargetCombo_->count() - 1;
        }
        if (index < 0) {
            index = outputs.size() > 1 ? 1 : 0;
            for (int i = 0; i < outputs.size(); ++i) if (!outputs[i].primary) { index = i; break; }
        }
        mirrorTargetCombo_->setCurrentIndex(index);
    }
    updateMirrorUi();
    const QSize selectedSize =
        captureOutputCombo_->currentData(kCaptureOutputSizeRole).toSize();
    if (selectedSize.isValid()) {
        const QSignalBlocker widthBlocker(screenWidthSpin_);
        const QSignalBlocker heightBlocker(screenHeightSpin_);
        screenWidthSpin_->setValue(selectedSize.width());
        screenHeightSpin_->setValue(selectedSize.height());
    }
    updateSamplingDepthUi();

    if (outputs.isEmpty()) {
        captureStatusText_ = errorMessage.isEmpty()
            ? text("No attached DXGI displays found.",
                "Подключённые DXGI-экраны не найдены.")
            : errorMessage;
        ledPreview_->setStatus(captureStatusText_, false);
    }
    updateDiagnostics();
}

void MainWindow::refreshAudioDevices()
{
    if (exiting_) {
        return;
    }

    const QString selectedOutput = comboValue(audioOutputDeviceCombo_);
    const QString selectedInput = comboValue(audioInputDeviceCombo_);
    QString outputError;
    QString inputError;
    const auto outputDevices = audio::AudioCaptureWorker::enumerateDevices(
        audio::AudioDeviceKind::Output, &outputError);
    const auto inputDevices = audio::AudioCaptureWorker::enumerateDevices(
        audio::AudioDeviceKind::Input, &inputError);

    const auto populate = [this](
                              QComboBox* combo,
                              const QVector<audio::AudioDevice>& devices,
                              const QString& selectedId,
                              const bool output) {
        const QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItem(
            output
                ? text("System default output", "Системное устройство вывода")
                : text("System default microphone", "Системный микрофон"),
            QString {});
        for (const auto& device : devices) {
            QString label = device.name;
            if (device.isDefault) {
                label += text(" — default", " — по умолчанию");
            }
            combo->addItem(label, device.id);
        }
        int selectedIndex = combo->findData(selectedId);
        if (selectedIndex < 0 && !selectedId.isEmpty()) {
            combo->addItem(
                text("Unavailable device (%1)", "Недоступное устройство (%1)")
                    .arg(selectedId),
                selectedId);
            selectedIndex = combo->count() - 1;
        }
        combo->setCurrentIndex(std::max(0, selectedIndex));
    };

    populate(audioOutputDeviceCombo_, outputDevices, selectedOutput, true);
    populate(audioInputDeviceCombo_, inputDevices, selectedInput, false);
    QStringList errors;
    if (!outputError.isEmpty()) {
        errors << text("Output: %1", "Вывод: %1").arg(outputError);
    }
    if (!inputError.isEmpty()) {
        errors << text("Microphone: %1", "Микрофон: %1").arg(inputError);
    }
    audioDeviceEnumerationError_ = errors.join(QStringLiteral("; "));
    updateAudioUi();
    updateDiagnostics();
    scheduleAudioUpdate();
}

void MainWindow::scheduleAudioUpdate()
{
    if (!applyingSettings_ && !exiting_) {
        if (!audioShouldBeActive()) {
            clearAudioFrame(!captureShouldBeActive());
        }
        audioUpdateTimer_->start();
    }
}

bool MainWindow::audioShouldBeActive() const
{
    return lightingEnabledCheck_->isChecked()
        && comboValue(activeModeCombo_) == QStringLiteral("equalizer")
        && !manualAnimationButton_->isChecked()
        && !testWhiteBalanceButton_->isChecked()
        && !layoutTestButton_->isChecked();
}

void MainWindow::updateAudioConfiguration()
{
    if (audioWorker_ == nullptr || exiting_) {
        return;
    }
    if (!audioShouldBeActive()) {
        audioWorker_->stop();
        clearAudioFrame(!captureShouldBeActive());
        audioStatus_ = {
            .state = audio::AudioState::Stopped,
            .message = {},
        };
        updateAudioUi();
        return;
    }

    audio::AudioConfiguration configuration;
    const QString source = comboValue(audioSourceCombo_);
    configuration.sourceMode = source == QStringLiteral("microphone")
        ? audio::AudioSourceMode::Microphone
        : source == QStringLiteral("mixed")
            ? audio::AudioSourceMode::Mixed
            : audio::AudioSourceMode::WindowsOutput;
    configuration.outputDeviceId = comboValue(audioOutputDeviceCombo_);
    configuration.inputDeviceId = comboValue(audioInputDeviceCombo_);
    configuration.analyzer.bandCount = audioBandsSpin_->value();
    configuration.analyzer.minimumFrequencyHz =
        audioMinimumFrequencySpin_->value();
    configuration.analyzer.maximumFrequencyHz =
        audioMaximumFrequencySpin_->value();
    configuration.analyzer.sensitivityPercent = audioSensitivitySpin_->value();
    configuration.analyzer.noiseGateDecibels = audioNoiseGateSpin_->value();
    configuration.perimeter = {
        .top = topLedsSpin_->value(),
        .right = rightLedsSpin_->value(),
        .bottom = bottomLedsSpin_->value(),
        .left = leftLedsSpin_->value(),
    };
    configuration.layout = comboValue(audioLayoutCombo_)
            == QStringLiteral("sequential")
        ? audio::SpectrumLayout::Sequential
        : audio::SpectrumLayout::Mirrored;
    configuration.colorMode = comboValue(audioColorModeCombo_)
            == QStringLiteral("solid")
        ? audio::SpectrumColorMode::Solid
        : audio::SpectrumColorMode::FrequencyGradient;
    configuration.solidColor = QColor(
        audioSolidColorButton_->property("audioColor").toString());
    configuration.outputGainPercent = audioOutputGainSpin_->value();
    configuration.microphoneGainPercent = audioMicrophoneGainSpin_->value();
    configuration.attackMilliseconds = audioAttackSpin_->value();
    configuration.releaseMilliseconds = audioReleaseSpin_->value();
    configuration.dynamics = comboValue(audioDynamicsCombo_)
            == QStringLiteral("spring")
        ? audio::SpectrumDynamics::Spring
        : audio::SpectrumDynamics::Smooth;
    configuration.springinessPercent = audioSpringinessSpin_->value();
    configuration.targetFps = 60;

    QString validationError;
    if (!configuration.isValid(&validationError)) {
        audioWorker_->stop();
        clearAudioFrame(true);
        audioStatus_ = {
            .state = audio::AudioState::Error,
            .message = validationError,
        };
        updateAudioUi();
        updateDiagnostics();
        return;
    }

    audioPreview_->setLayout(
        configuration.perimeter.top,
        configuration.perimeter.right,
        configuration.perimeter.bottom,
        configuration.perimeter.left);
    audioStatus_ = {
        .state = audio::AudioState::Starting,
        .message = text("Opening the selected audio source…",
            "Открытие выбранного источника звука…"),
    };
    updateAudioUi();
    audioWorker_->updateConfiguration(configuration);
    audioWorker_->start();
}

void MainWindow::clearAudioFrame(const bool clearLedOutput)
{
    lastAudioGeneration_ = 0;
    audioFrameNumber_ = 0;
    measuredAudioFps_ = 0.0;
    audioPeakLevel_ = 0.0;
    audioLevelMeter_->setValue(0);
    audioPreview_->clearFrame();
    if (clearLedOutput) {
        lastLogicalLedColors_.clear();
        lastLedFrame_.clear();
        if (transportWorker_ != nullptr) {
            transportWorker_->clearPendingLedFrame();
        }
    }
}

void MainWindow::handleAudioFrame(const audio::AudioFrame& frame)
{
    if (!audioShouldBeActive()
        || frame.configurationGeneration < lastAudioGeneration_) {
        return;
    }
    const int ledCount = topLedsSpin_->value() + rightLedsSpin_->value()
        + bottomLedsSpin_->value() + leftLedsSpin_->value();
    if (frame.colors.size() != ledCount) {
        return;
    }
    lastAudioGeneration_ = frame.configurationGeneration;
    lastLogicalLedColors_.clear();
    lastLogicalLedColors_.reserve(frame.colors.size());
    for (const QRgb value : frame.colors) {
        lastLogicalLedColors_.push_back(QColor::fromRgb(value));
    }
    lastLedFrame_ = makePhysicalLedFrame();
    if (transportWorker_ != nullptr && !lastLedFrame_.isEmpty()) {
        transportWorker_->submitLedFrame(lastLedFrame_);
    }

    audioFrameNumber_ = frame.frameNumber;
    measuredAudioFps_ = frame.framesPerSecond;
    audioPeakLevel_ = frame.peakLevel;
    audioLevelMeter_->setValue(static_cast<int>(
        std::lround(std::clamp(audioPeakLevel_, 0.0, 1.0) * 100.0)));
    audioStatus_.state = audio::AudioState::Running;

    auto previewColors = lastLogicalLedColors_;
    const int brightness = brightnessSpin_->value();
    if (brightness < 100) {
        for (auto& color : previewColors) {
            color.setRgb(
                (color.red() * brightness + 50) / 100,
                (color.green() * brightness + 50) / 100,
                (color.blue() * brightness + 50) / 100);
        }
    }
    audioPreview_->setFrame(
        std::move(previewColors), {}, {}, measuredAudioFps_, 0.0);
    if ((frame.frameNumber % 15U) == 0U || frame.frameNumber == 1U) {
        updateAudioUi();
        updateDiagnostics();
    }
}

void MainWindow::handleAudioStatus(const audio::AudioStatus& status)
{
    if (!audioShouldBeActive()) {
        return;
    }
    audioStatus_ = status;
    if (status.state == audio::AudioState::Error
        || status.state == audio::AudioState::Stopped) {
        clearAudioFrame(true);
    }
    updateAudioUi();
    updateDiagnostics();
}

void MainWindow::stopAudio()
{
    if (audioUpdateTimer_ != nullptr) {
        audioUpdateTimer_->stop();
    }
    if (audioWorker_ != nullptr) {
        audioWorker_->stop();
    }
}

void MainWindow::refreshSerialPorts()
{
    if (transportWorker_ == nullptr
        || exiting_
        || serialPortRefreshInProgress_) {
        return;
    }

    serialPortRefreshInProgress_ = true;
    refreshPortsButton_->setEnabled(false);
    if (!transportWorker_->requestPortRefresh()) {
        serialPortRefreshInProgress_ = false;
        refreshPortsButton_->setEnabled(true);
        applySerialPorts(
            serialPorts_,
            text(
                "COM port refresh could not be started because the "
                "transport worker is stopping.",
                "Не удалось обновить список COM-портов: "
                "транспортный поток завершает работу."));
    }
}

void MainWindow::applySerialPorts(
    const QVector<transport::SerialPortDescriptor>& ports,
    const QString& errorMessage)
{
    serialPorts_ = ports;
    const QString selectedPort = portCombo_->count() > 0
        ? serialPortValue().trimmed().toUpper()
        : QString {};

    const QSignalBlocker blocker(portCombo_);
    portCombo_->clear();
    portCombo_->addItem(
        text("Automatic (Ambilight device)",
            "Автоматически (устройство Ambilight)"),
        QString {});

    for (const auto& port : ports) {
        QString label = port.portName;
        if (!port.description.isEmpty()) {
            label += QStringLiteral(" — ") + port.description;
        }
        portCombo_->addItem(label, port.portName.toUpper());
    }

    int selectedIndex = portCombo_->findData(selectedPort);
    if (selectedIndex < 0 && !selectedPort.isEmpty()) {
        portCombo_->addItem(
            text("Unavailable port (%1)", "Недоступный порт (%1)")
                .arg(selectedPort),
            selectedPort);
        selectedIndex = portCombo_->count() - 1;
    }
    portCombo_->setCurrentIndex(std::max(0, selectedIndex));
    serialPortEnumerationError_ = errorMessage;
    updateDiagnostics();
}

void MainWindow::scheduleTransportUpdate()
{
    if (!applyingSettings_ && !exiting_) {
        transportUpdateTimer_->start();
    }
}

void MainWindow::updateTransportConfiguration()
{
    if (transportWorker_ == nullptr || exiting_) {
        return;
    }

    transport::ConnectionConfiguration configuration;
    configuration.portName = serialPortValue().trimmed().toUpper();
    configuration.selectionMode = configuration.portName.isEmpty()
        ? transport::PortSelectionMode::Automatic
        : transport::PortSelectionMode::Explicit;
    configuration.baudRate = kFirmwareBaudRate;

    transportWorker_->updateConnectionConfiguration(configuration);
    transportWorker_->start();
}

void MainWindow::synchronizeDeviceConfiguration()
{
    if (transportWorker_ == nullptr || exiting_) {
        return;
    }

    const int ledCount = topLedsSpin_->value() + rightLedsSpin_->value()
        + bottomLedsSpin_->value() + leftLedsSpin_->value();
    if (ledCount <= 0 || ledCount > 300) {
        transportWorker_->clearPendingLedFrame();
        return;
    }

    protocol::StripConfig configuration;
    configuration.ledCount = static_cast<std::uint16_t>(ledCount);
    configuration.topCount =
        static_cast<std::uint16_t>(topLedsSpin_->value());
    configuration.rightCount =
        static_cast<std::uint16_t>(rightLedsSpin_->value());
    configuration.bottomCount =
        static_cast<std::uint16_t>(bottomLedsSpin_->value());
    configuration.leftCount =
        static_cast<std::uint16_t>(leftLedsSpin_->value());
    configuration.startOffset =
        static_cast<std::uint16_t>(ledOffsetSpin_->value());
    configuration.reversed =
        comboValue(directionCombo_) != QStringLiteral("clockwise");
    configuration.colorOrder =
        protocolColorOrder(comboValue(colorOrderCombo_));
    configuration.brightnessLimit =
        static_cast<protocol::Byte>(
            (brightnessSpin_->value() * 255 + 50) / 100);
    configuration.currentLimitMilliamps =
        static_cast<std::uint16_t>(std::max(
            currentLimitSpin_->value(),
            AppSettings::minimumCurrentLimitMilliAmps(ledCount)));
    configuration.ambientTimeoutMilliseconds =
        static_cast<std::uint16_t>(ambientTimeoutSpin_->value());
    const auto calibrationByte = [](const int percent) {
        return static_cast<protocol::Byte>((percent * 255 + 50) / 100);
    };
    configuration.redCalibration =
        calibrationByte(redCalibrationSpin_->value());
    configuration.greenCalibration =
        calibrationByte(greenCalibrationSpin_->value());
    configuration.blueCalibration =
        calibrationByte(blueCalibrationSpin_->value());

    lastTransportCommandError_.clear();
    transportWorker_->sendStripConfiguration(configuration);
    transportWorker_->sendBrightness(configuration.brightnessLimit);

    if (lightingEnabledCheck_->isChecked()
        && testWhiteBalanceButton_->isChecked()) {
        transportWorker_->clearPendingLedFrame();
        transportWorker_->sendEffect({
            .effect = protocol::EffectType::Solid,
            .intensity = 255,
            .red = 255,
            .green = 255,
            .blue = 255,
            .reserved = 0,
            .speedPermille = 1000,
        });
        transportWorker_->sendMode(protocol::OperatingMode::Solid);
        return;
    }

    if (lightingEnabledCheck_->isChecked()
        && layoutTestButton_->isChecked()) {
        transportWorker_->clearPendingLedFrame();
        transportWorker_->sendFallbackMode(protocol::OperatingMode::Off);
        transportWorker_->sendMode(protocol::OperatingMode::Ambilight);
        submitLayoutTestFrame();
        return;
    }

    if (lightingEnabledCheck_->isChecked()
        && manualAnimationButton_->isChecked()) {
        const QString animation = comboValue(standaloneEffectCombo_);
        if (animation != QStringLiteral("off")) {
            transportWorker_->clearPendingLedFrame();
            transportWorker_->sendEffect(effectConfiguration(
                animation,
                QColor(solidColorButton_->property("solidColor").toString())));
            transportWorker_->sendMode(protocolMode(animation));
            return;
        }
    }

    const QString activeMode = lightingEnabledCheck_->isChecked()
        ? comboValue(activeModeCombo_)
        : QStringLiteral("off");
    if (activeMode == QStringLiteral("ambilight")
        || activeMode == QStringLiteral("equalizer")) {
        const QString fallback = comboValue(standaloneEffectCombo_);
        if (fallback == QStringLiteral("off")) {
            transportWorker_->sendFallbackMode(
                protocol::OperatingMode::Off);
        } else {
            transportWorker_->sendEffect(effectConfiguration(
                fallback,
                QColor(solidColorButton_->property("solidColor").toString())));
            transportWorker_->sendFallbackMode(protocolMode(fallback));
        }
        transportWorker_->sendMode(protocol::OperatingMode::Ambilight);

        lastLedFrame_ = makePhysicalLedFrame();
        if (!lastLedFrame_.isEmpty()) {
            transportWorker_->submitLedFrame(lastLedFrame_);
        }
    } else {
        transportWorker_->clearPendingLedFrame();
        if (activeMode != QStringLiteral("off")) {
            transportWorker_->sendEffect(effectConfiguration(
                activeMode,
                QColor(solidColorButton_->property("solidColor").toString())));
        }
        transportWorker_->sendMode(protocolMode(activeMode));
    }
}

void MainWindow::handleTransportStatus(
    const transport::ConnectionStatus& status)
{
    transportStatus_ = status;
    if (status.state != transport::ConnectionState::Connected) {
        deviceInformation_ = {};
        deviceState_ = {};
    }
    connectionStatusLabel_->setText(transportStateText());
    connectionStatusLabel_->setToolTip(status.message);
    updateConnectionShortcut();
    updateLedTestUi();
    if (status.state == transport::ConnectionState::Connected) {
        lastTransportCommandError_.clear();
    }
    updateDiagnostics();
}

void MainWindow::handleTransportCommand(
    const transport::CommandResult& result)
{
    if (result.success) {
        return;
    }

    lastTransportCommandError_ =
        QStringLiteral("%1 (type 0x%2, code %3, detail %4)")
            .arg(result.message)
            .arg(result.messageType, 2, 16, QLatin1Char('0'))
            .arg(result.errorCode)
            .arg(result.detail);
    statusBar()->showMessage(
        text("ESP32 rejected a command: %1",
            "ESP32 отклонил команду: %1")
            .arg(lastTransportCommandError_),
        6000);
    updateDiagnostics();
}

QString MainWindow::transportStateText() const
{
    QString state;
    switch (transportStatus_.state) {
    case transport::ConnectionState::Stopped:
        state = text("Stopped", "Остановлен");
        break;
    case transport::ConnectionState::Searching:
        state = text("Searching for the controller",
            "Поиск контроллера");
        break;
    case transport::ConnectionState::Opening:
        state = text("Opening the port", "Открытие порта");
        break;
    case transport::ConnectionState::Handshaking:
        state = text("Checking the controller", "Проверка контроллера");
        break;
    case transport::ConnectionState::Connected:
        state = text("Connected", "Подключено");
        break;
    case transport::ConnectionState::Reconnecting:
        state = text("Connection lost; retrying",
            "Связь потеряна; повторное подключение");
        break;
    case transport::ConnectionState::Error:
        state = text("Connection error", "Ошибка подключения");
        break;
    }

    if (!transportStatus_.portName.isEmpty()) {
        state += QStringLiteral(" — ") + transportStatus_.portName;
    }
    if (transportStatus_.nativeError != 0) {
        state += QStringLiteral(" [0x%1]")
                     .arg(
                         static_cast<qulonglong>(
                             transportStatus_.nativeError),
                         0,
                         16);
    }
    return state;
}

QByteArray MainWindow::makePhysicalLedFrame() const
{
    const int expectedLedCount = topLedsSpin_->value()
        + rightLedsSpin_->value()
        + bottomLedsSpin_->value()
        + leftLedsSpin_->value();
    if (lastLogicalLedColors_.size() != expectedLedCount) {
        return {};
    }

    const auto physicalColors = mapToPhysicalStrip(
        lastLogicalLedColors_,
        ledOffsetSpin_->value(),
        comboValue(directionCombo_) == QStringLiteral("clockwise"));
    QByteArray bytes;
    bytes.reserve(physicalColors.size() * 3);
    for (const auto& color : physicalColors) {
        bytes.append(static_cast<char>(color.red()));
        bytes.append(static_cast<char>(color.green()));
        bytes.append(static_cast<char>(color.blue()));
    }
    return bytes;
}

QByteArray MainWindow::makeLayoutTestFrame() const
{
    const PerimeterLayout layout{
        .top = topLedsSpin_->value(),
        .right = rightLedsSpin_->value(),
        .bottom = bottomLedsSpin_->value(),
        .left = leftLedsSpin_->value(),
    };
    QVector<QColor> logicalColors;
    if (layoutTestPattern_ == LayoutTestPattern::AllSides) {
        logicalColors = makeLayoutTestPattern(layout);
    } else {
        logicalColors.fill(Qt::black, layout.total());
        int first = 0;
        int count = 0;
        switch (layoutTestPattern_) {
        case LayoutTestPattern::Top:
            count = layout.top;
            break;
        case LayoutTestPattern::Right:
            first = layout.top;
            count = layout.right;
            break;
        case LayoutTestPattern::Bottom:
            first = layout.top + layout.right;
            count = layout.bottom;
            break;
        case LayoutTestPattern::Left:
            first = layout.top + layout.right + layout.bottom;
            count = layout.left;
            break;
        case LayoutTestPattern::LedZero:
        case LayoutTestPattern::AllSides:
            break;
        }
        for (int index = first; index < first + count; ++index) {
            logicalColors[index] = Qt::white;
        }
    }
    auto physicalColors = mapToPhysicalStrip(
        logicalColors,
        ledOffsetSpin_->value(),
        comboValue(directionCombo_) == QStringLiteral("clockwise"));
    if (physicalColors.isEmpty()) {
        return {};
    }

    // A white first physical pixel remains a stable reference while offset
    // and direction move the four logical side colors around the strip.
    physicalColors[0] = Qt::white;

    QByteArray bytes;
    bytes.reserve(physicalColors.size() * 3);
    for (const auto& color : physicalColors) {
        bytes.push_back(static_cast<char>(color.red()));
        bytes.push_back(static_cast<char>(color.green()));
        bytes.push_back(static_cast<char>(color.blue()));
    }
    return bytes;
}

void MainWindow::submitLayoutTestFrame()
{
    if (transportWorker_ == nullptr
        || !lightingEnabledCheck_->isChecked()
        || !layoutTestButton_->isChecked()) {
        return;
    }
    lastLedFrame_ = makeLayoutTestFrame();
    if (!lastLedFrame_.isEmpty()) {
        transportWorker_->submitLedFrame(lastLedFrame_);
    }
}

void MainWindow::scheduleCaptureUpdate()
{
    if (!applyingSettings_ && !exiting_) {
        if (!lightingEnabledCheck_->isChecked()
            || comboValue(activeModeCombo_) != QStringLiteral("ambilight")
            || manualAnimationButton_->isChecked()
            || testWhiteBalanceButton_->isChecked()
            || layoutTestButton_->isChecked()) {
            clearCapturedFrame(!audioShouldBeActive());
        }
        captureUpdateTimer_->start();
    }
}

bool MainWindow::captureShouldBeActive() const
{
    return lightingEnabledCheck_->isChecked()
        && comboValue(activeModeCombo_) == QStringLiteral("ambilight")
        && !manualAnimationButton_->isChecked()
        && !testWhiteBalanceButton_->isChecked()
        && !layoutTestButton_->isChecked();
}

void MainWindow::updateCaptureConfiguration()
{
    if (captureWorker_ == nullptr || exiting_) {
        return;
    }

    const bool shouldCapture = captureShouldBeActive();
    if (!shouldCapture && !mirrorRunning_) {
        captureWorker_->stop();
        clearCapturedFrame(!audioShouldBeActive());
        const bool ledTestActive = testWhiteBalanceButton_->isChecked()
            || layoutTestButton_->isChecked();
        if (!lightingEnabledCheck_->isChecked()) {
            captureStatusText_ =
                text("Lighting is disabled.", "Подсветка выключена.");
        } else if (manualAnimationButton_->isChecked()) {
            captureStatusText_ = text(
                "Capture paused while the manual animation is active.",
                "Захват приостановлен на время ручной анимации.");
        } else if (ledTestActive) {
            captureStatusText_ = text(
                "Capture paused during the LED test.",
                "Захват приостановлен на время проверки ленты.");
        } else {
            captureStatusText_ = text(
                "Capture paused for the selected mode.",
                "Захват приостановлен для выбранного режима.");
        }
        ledPreview_->setStatus(captureStatusText_, false);
        updateDiagnostics();
        return;
    }

    if (!shouldCapture) {
        clearCapturedFrame(!audioShouldBeActive()
            && !testWhiteBalanceButton_->isChecked() && !layoutTestButton_->isChecked());
        captureStatusText_ = text("Capture for TV mirror only; Ambilight is paused.",
            "Захват только для зеркала ТВ; Ambilight приостановлен.");
        ledPreview_->setStatus(captureStatusText_, false);
    }
    const PerimeterLayout ledLayout{
        .top = topLedsSpin_->value(),
        .right = rightLedsSpin_->value(),
        .bottom = bottomLedsSpin_->value(),
        .left = leftLedsSpin_->value(),
    };
    const bool gaming = gamingModeCheck_->isChecked();
    const auto samplingLayout = distributeSamplingZones(
        gaming ? std::min(zonesSpin_->value(), capture::kGamingMaximumZones)
               : zonesSpin_->value(), ledLayout);

    capture::CaptureConfiguration configuration;
    configuration.processLedColors = shouldCapture;
    if (mirrorRunning_) {
        configuration.mirrorWindow = static_cast<quintptr>(mirrorWindow_->winId());
        configuration.mirrorOutputId = comboValue(mirrorTargetCombo_);
        configuration.mirrorFps = mirrorFpsSpin_->value();
    }
    configuration.outputId = comboValue(captureOutputCombo_);
    configuration.useWindowsGraphicsCapture =
        comboValue(captureMethodCombo_) == QStringLiteral("wgc");
    configuration.topLedCount = samplingLayout.top;
    configuration.rightLedCount = samplingLayout.right;
    configuration.bottomLedCount = samplingLayout.bottom;
    configuration.leftLedCount = samplingLayout.left;
    configuration.gamingMode = gaming;
    configuration.targetFps = gaming
        ? std::min(fpsSpin_->value(), capture::kGamingMaximumFps) : fpsSpin_->value();
    configuration.blackBarDetectionMode = gaming
        ? capture::BlackBarDetectionMode::Off
        : captureBlackBarDetectionMode(comboValue(blackBarModeCombo_));
    configuration.samplingDepthPixels =
        samplingDepthAutoCheck_->isChecked()
        ? 0
        : samplingDepthSpin_->value();
    configuration.regionColorMode = gaming ? capture::RegionColorMode::Mean
        : captureRegionColorMode(comboValue(regionColorModeCombo_));
    configuration.ledTransferMode =
        captureLedTransferMode(comboValue(ledTransferModeCombo_));

    QString validationError;
    if (!configuration.isValid(&validationError)) {
        captureStatusText_ = validationError;
        ledPreview_->setStatus(captureStatusText_, false);
        captureWorker_->stop();
        clearCapturedFrame();
        updateDiagnostics();
        return;
    }

    ledPreview_->setLayout(
        ledLayout.top,
        ledLayout.right,
        ledLayout.bottom,
        ledLayout.left);
    captureWorker_->updateConfiguration(configuration);
    captureWorker_->start();
}

void MainWindow::clearCapturedFrame(const bool clearLedOutput)
{
    previewRefreshTimer_->stop();
    const int previousSamplingDepth = effectiveSamplingDepthPixels();
    ledFrameFilter_.reset();
    if (clearLedOutput) {
        lastLogicalLedColors_.clear();
        lastLedFrame_.clear();
    }
    capturedFrameNumber_ = 0;
    capturedFrameSize_ = {};
    capturedContentInsets_ = {};
    measuredCaptureFps_ = 0.0;
    captureProcessingMilliseconds_ = 0.0;
    if (effectiveSamplingDepthPixels() != previousSamplingDepth) {
        updateSamplingDepthUi();
    }
    ledPreview_->clearFrame();
    updateSamplingOverlay();
    if (clearLedOutput && transportWorker_ != nullptr) {
        transportWorker_->clearPendingLedFrame();
    }
}

void MainWindow::updatePreviewFromLastCapture()
{
    previewRefreshTimer_->stop();
    if (!ledPreview_->isVisible() || isMinimized()) return;
    if (lastLogicalLedColors_.isEmpty()
        || lastLogicalLedColors_.size()
            != topLedsSpin_->value() + rightLedsSpin_->value()
                + bottomLedsSpin_->value() + leftLedsSpin_->value()) {
        return;
    }

    auto previewColors = lastLogicalLedColors_;
    const int brightness = brightnessSpin_->value();
    if (brightness < 100) {
        for (auto& color : previewColors) {
            color.setRgb(
                (color.red() * brightness + 50) / 100,
                (color.green() * brightness + 50) / 100,
                (color.blue() * brightness + 50) / 100);
        }
    }
    ledPreview_->setFrame(
        std::move(previewColors),
        capturedFrameSize_,
        capturedContentInsets_,
        measuredCaptureFps_,
        captureProcessingMilliseconds_);
    updateSamplingOverlay();
}

void MainWindow::handleCaptureSample(const capture::CaptureSample& sample)
{
    if (!captureShouldBeActive()
        || sample.configurationGeneration < lastCaptureGeneration_) {
        return;
    }
    lastCaptureGeneration_ = sample.configurationGeneration;

    QVector<QColor> zoneColors;
    zoneColors.reserve(sample.colors.size());
    for (const QRgb color : sample.colors) {
        zoneColors.push_back(QColor::fromRgb(color));
    }

    const PerimeterLayout zoneLayout{
        .top = sample.samplingLayout.top,
        .right = sample.samplingLayout.right,
        .bottom = sample.samplingLayout.bottom,
        .left = sample.samplingLayout.left,
    };
    const PerimeterLayout ledLayout{
        .top = topLedsSpin_->value(),
        .right = rightLedsSpin_->value(),
        .bottom = bottomLedsSpin_->value(),
        .left = leftLedsSpin_->value(),
    };
    const auto ledColors = resamplePerimeter(zoneColors, zoneLayout, ledLayout);
    if (ledColors.size() != ledLayout.total()) {
        return;
    }

    if (capturedFrameSize_.isValid()
        && sample.frameSize != capturedFrameSize_) {
        // A display mode switch changes the pixel-to-zone geometry. Do not
        // blend the last 21:9 frame into the first 16:9 frame (or vice versa).
        ledFrameFilter_.reset();
    }
    lastLogicalLedColors_ = ledFrameFilter_.apply(
        ledColors,
        sample.capturedAtMilliseconds,
        smoothingCombo_->currentData().toInt(),
        flashProtectionCheck_->isChecked());
    lastLedFrame_ = makePhysicalLedFrame();
    if (transportWorker_ != nullptr && !lastLedFrame_.isEmpty()) {
        transportWorker_->submitLedFrame(lastLedFrame_);
    }

    const int previousSamplingDepth = effectiveSamplingDepthPixels();
    capturedFrameSize_ = sample.frameSize;
    capturedContentInsets_ = {
        sample.contentInsets.left,
        sample.contentInsets.top,
        sample.contentInsets.right,
        sample.contentInsets.bottom,
    };
    measuredCaptureFps_ = sample.framesPerSecond;
    captureProcessingMilliseconds_ = sample.processingMilliseconds;
    capturedFrameNumber_ = sample.frameNumber;
    if (effectiveSamplingDepthPixels() != previousSamplingDepth) {
        updateSamplingDepthUi();
    }
    captureStatusText_ = text("Capture active: %1", "Захват активен: %1")
        .arg(captureMethodCombo_->currentText());
    if (sample.frameSize.isValid()) {
        const QSignalBlocker widthBlocker(screenWidthSpin_);
        const QSignalBlocker heightBlocker(screenHeightSpin_);
        screenWidthSpin_->setValue(sample.frameSize.width());
        screenHeightSpin_->setValue(sample.frameSize.height());
    }

    ledPreview_->setStatus(captureStatusText_, true);
    // The strip keeps the full capture rate. Only the on-screen preview is
    // limited to 20 Hz, and hidden previews do no color conversion/layout work.
    // A trailing refresh also displays the final frame if the desktop stops
    // changing before the next 50 ms interval has elapsed.
    if (ledPreview_->isVisible() && !isMinimized()
        && !previewRefreshTimer_->isActive()) {
        previewRefreshTimer_->start();
    }

    if ((sample.frameNumber % 15U) == 0U) {
        updateDiagnostics();
    }
}

void MainWindow::handleCaptureStatus(const capture::CaptureStatus& status)
{
    if (mirrorRunning_ && status.state == capture::CaptureState::Error) {
        stopMirror();
        mirrorStatusLabel_->setText(text("Mirror error: %1", "Ошибка зеркала: %1").arg(status.message));
    }
    if (mirrorRunning_ && status.state != capture::CaptureState::Running)
        mirrorStatusLabel_->setText(status.message);
    // stop() is asynchronous. Once an LED test has taken ownership of the
    // strip, a late Stopped status from Desktop Duplication must not clear
    // the retained test frame and let the ESP32 lease expire.
    if (!captureShouldBeActive()) {
        return;
    }

    QString stateText;
    bool active = false;
    switch (status.state) {
    case capture::CaptureState::Stopped:
        stateText = text("Capture stopped", "Захват остановлен");
        break;
    case capture::CaptureState::Starting:
        stateText = text("Starting capture", "Запуск захвата");
        break;
    case capture::CaptureState::Running:
        stateText = text("Capture active: %1", "Захват активен: %1")
            .arg(captureMethodCombo_->currentText());
        active = true;
        break;
    case capture::CaptureState::WaitingForFrame:
        stateText = text("Waiting for a new desktop frame",
            "Ожидание нового кадра рабочего стола");
        active = true;
        break;
    case capture::CaptureState::Reinitializing:
        stateText = text("Reinitializing capture", "Перезапуск захвата");
        break;
    case capture::CaptureState::Error:
        stateText = text("Capture error", "Ошибка захвата");
        break;
    }

    if (!status.message.isEmpty()
        && status.state != capture::CaptureState::Running) {
        stateText += QStringLiteral(": ") + status.message;
    }
    if (status.nativeError != 0) {
        stateText += QStringLiteral(" [0x%1]")
                         .arg(
                             static_cast<qulonglong>(status.nativeError),
                             0,
                             16);
        lastCaptureError_ = stateText;
    }

    captureStatusText_ = stateText;
    ledPreview_->setStatus(captureStatusText_, active);
    if (status.state == capture::CaptureState::Starting
        || status.state == capture::CaptureState::Reinitializing
        || status.state == capture::CaptureState::Error
        || status.state == capture::CaptureState::Stopped) {
        clearCapturedFrame();
    }
    updateDiagnostics();
}

void MainWindow::stopCapture()
{
    if (captureUpdateTimer_ != nullptr) {
        captureUpdateTimer_->stop();
    }
    if (captureWorker_ != nullptr) {
        captureWorker_->stop();
    }
}

void MainWindow::stopTransport()
{
    if (transportUpdateTimer_ != nullptr) {
        transportUpdateTimer_->stop();
    }
    if (transportWorker_ != nullptr) {
        transportWorker_->stop();
    }
}

void MainWindow::startMinimized()
{
    if (trayIcon_ != nullptr) {
        hide();
    } else {
        showMinimized();
    }
}

void MainWindow::showWindow()
{
    if (exiting_) {
        return;
    }
    if (isMinimized()) {
        setWindowState(windowState() & ~Qt::WindowMinimized);
    }
    show();
    raise();
    activateWindow();
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    // Also covers restoration through the taskbar, not just the tray action.
    QTimer::singleShot(0, this, [this] {
        updatePreviewFromLastCapture();
        updateDiagnostics();
    });
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    const bool sessionShutdown = qApp->isSavingSession();
    if (!sessionShutdown && !exiting_ && trayIcon_ != nullptr
        && closeToTrayCheck_->isChecked()) {
        hide();
        event->ignore();
        if (!trayNoticeShown_) {
            trayNoticeShown_ = true;
            trayIcon_->showMessage(
                QStringLiteral("Ambilight by Levurgi4"),
                text("The application is still running in the notification area.",
                    "Приложение продолжает работать в области уведомлений."),
                QSystemTrayIcon::Information,
                3000);
        }
        return;
    }

    event->accept();
    exitApplication();
}

void MainWindow::exitApplication()
{
    if (exiting_) {
        return;
    }

    const bool sessionShutdown = qApp->isSavingSession();
    transportUpdateTimer_->stop();
    if (!sessionShutdown) {
        // Flush the latest UI choices into the desired transport state before
        // shutting its thread down. This matters when the user changes the
        // standalone fallback and immediately presses Quit.
        synchronizeDeviceConfiguration();
    }
    const bool allowControllerFlush =
        !sessionShutdown
        && transportStatus_.state == transport::ConnectionState::Connected;
    exiting_ = true;
    stopMirror();
    saveSettings(false);
    stopCapture();
    stopAudio();
    unregisterGlobalHotkey();
    if (trayIcon_ != nullptr) {
        trayIcon_->hide();
    }
    hide();

    if (allowControllerFlush) {
        transportWorker_->requestDrain(1'500);
        // The transport owns its own bounded deadline; this second deadline
        // also protects application shutdown if its worker thread stops
        // delivering queued signals.
        QTimer::singleShot(1'750, this, &MainWindow::finishExit);
    } else {
        finishExit();
    }
}

void MainWindow::finishExit()
{
    if (exitFinished_) {
        return;
    }
    exitFinished_ = true;
    stopTransport();
    qApp->quit();
}

bool MainWindow::registerGlobalHotkey(
    const QKeySequence& sequence,
    HotkeyStatus* failureStatus)
{
#ifdef Q_OS_WIN
    if (sequence == registeredHotkey_) {
        return true;
    }

    UINT modifiers = 0;
    UINT virtualKey = 0;
    if (!sequence.isEmpty()
        && !toWindowsHotkey(sequence, modifiers, virtualKey)) {
        if (failureStatus != nullptr) {
            *failureStatus = HotkeyStatus::InvalidSequence;
        }
        return false;
    }

    const QKeySequence previous = registeredHotkey_;
    unregisterGlobalHotkey();
    if (sequence.isEmpty()) {
        return true;
    }

    if (RegisterHotKey(nullptr, kHotkeyId, modifiers, virtualKey) != FALSE) {
        registeredHotkey_ = sequence;
        return true;
    }

    if (!previous.isEmpty()) {
        UINT previousModifiers = 0;
        UINT previousVirtualKey = 0;
        if (toWindowsHotkey(previous, previousModifiers, previousVirtualKey)
            && RegisterHotKey(
                nullptr,
                kHotkeyId,
                previousModifiers,
                previousVirtualKey)
                != FALSE) {
            registeredHotkey_ = previous;
        }
    }

    if (failureStatus != nullptr) {
        *failureStatus = HotkeyStatus::Unavailable;
    }
    return false;
#else
    Q_UNUSED(sequence)
    if (failureStatus != nullptr) {
        *failureStatus = HotkeyStatus::Unsupported;
    }
    return false;
#endif
}

void MainWindow::unregisterGlobalHotkey()
{
#ifdef Q_OS_WIN
    if (!registeredHotkey_.isEmpty()) {
        UnregisterHotKey(nullptr, kHotkeyId);
    }
#endif
    registeredHotkey_ = {};
}

void MainWindow::applyHotkey(const QKeySequence& sequence)
{
    HotkeyStatus failureStatus = HotkeyStatus::Unavailable;
    if (!registerGlobalHotkey(sequence, &failureStatus)) {
        hotkeyStatus_ = failureStatus;
        updateHotkeyStatusUi();
        updateDiagnostics();
        return;
    }

    configuredHotkey_ = sequence;
    hotkeyStatus_ = sequence.isEmpty()
        ? HotkeyStatus::Disabled
        : HotkeyStatus::Active;
    updateHotkeyStatusUi();
    updateDiagnostics();
    scheduleSave();
}

bool MainWindow::nativeEventFilter(
    const QByteArray& eventType,
    void* message,
    qintptr* result)
{
    Q_UNUSED(eventType)
    Q_UNUSED(result)
#ifdef Q_OS_WIN
    const auto* nativeMessage = static_cast<MSG*>(message);
    if (nativeMessage != nullptr) {
        static const UINT activationMessage =
            RegisterWindowMessageW(kSingleInstanceActivationMessageName);
        if (activationMessage != 0
            && nativeMessage->message == activationMessage) {
            if (!exiting_) {
                showWindow();
                const HANDLE acknowledgementEvent =
                    OpenEventW(
                        EVENT_MODIFY_STATE,
                        FALSE,
                        kSingleInstanceActivationAckEventName);
                if (acknowledgementEvent != nullptr) {
                    SetEvent(acknowledgementEvent);
                    CloseHandle(acknowledgementEvent);
                }
            }
            return true;
        }
    }
    if (nativeMessage != nullptr
        && nativeMessage->message == WM_DISPLAYCHANGE && mirrorRunning_) {
        // Defer out of the native callback before synchronizing with D3D.
        QTimer::singleShot(0, this, [this] {
            if (!mirrorRunning_) return;
            stopMirror();
            refreshCaptureOutputs();
            mirrorStatusLabel_->setText(text("Displays changed; restart mirroring after checking the destination.",
                "Конфигурация экранов изменилась. Проверьте назначение и запустите зеркало снова."));
        });
    }
    if (nativeMessage != nullptr
        && nativeMessage->message == WM_HOTKEY
        && nativeMessage->wParam == kHotkeyId) {
        lightingEnabledCheck_->toggle();
        return true;
    }
#else
    Q_UNUSED(message)
#endif
    return false;
}

QString MainWindow::comboValue(const QComboBox* combo) const
{
    const QVariant itemData = combo->currentData();
    return itemData.isValid() ? itemData.toString() : combo->currentText();
}

QString MainWindow::serialPortValue() const
{
    const int index = portCombo_->currentIndex();
    if (index >= 0
        && portCombo_->currentText() == portCombo_->itemText(index)) {
        const QVariant itemData = portCombo_->itemData(index);
        if (itemData.isValid()) {
            return itemData.toString();
        }
    }
    return portCombo_->currentText();
}

void MainWindow::selectComboValue(QComboBox* combo, const QString& value)
{
    const int index = combo->findData(value);
    if (index >= 0) {
        combo->setCurrentIndex(index);
    } else if (combo->isEditable()) {
        combo->setEditText(value);
    }
}

} // namespace ambilight
