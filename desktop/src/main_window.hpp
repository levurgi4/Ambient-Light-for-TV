#pragma once

#include "app_settings.hpp"
#include "audio/audio_types.hpp"
#include "capture/capture_types.hpp"
#include "led_frame_filter.hpp"
#include "transport/transport_types.hpp"

#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QKeySequence>
#include <QMainWindow>
#include <QMargins>
#include <QSize>

class QAction;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QKeySequenceEdit;
class QMenu;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QSystemTrayIcon;
class QTabWidget;
class QTimer;
class QTranslator;

namespace ambilight {

inline constexpr wchar_t kSingleInstanceActivationMessageName[] =
    L"Levurgi4.AmbilightByLevurgi4.Activate";
inline constexpr wchar_t kSingleInstanceActivationAckEventName[] =
    L"Local\\Levurgi4.AmbilightByLevurgi4.ActivationAcknowledged";

class LedPreviewWidget;

namespace capture {
class ScreenCaptureWorker;
} // namespace capture

namespace audio {
class AudioCaptureWorker;
} // namespace audio

namespace transport {
class SerialTransportWorker;
} // namespace transport

class MainWindow final : public QMainWindow, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void startMinimized();

protected:
    void closeEvent(QCloseEvent* event) override;
    bool nativeEventFilter(
        const QByteArray& eventType,
        void* message,
        qintptr* result) override;

private:
    enum class Language {
        English,
        Russian,
    };

    enum class HotkeyStatus {
        Disabled,
        Active,
        InvalidSequence,
        Unavailable,
        Unsupported,
    };

    enum class LayoutTestPattern {
        AllSides,
        Top,
        Right,
        Bottom,
        Left,
        LedZero,
    };

    void buildUi();
    void buildGeneralTab();
    void buildScreenTab();
    void buildAudioTab();
    void buildLedTab();
    void buildConnectionTab();
    void buildDiagnosticsTab();
    void buildTray();
    void connectSignals();

    void loadSettings();
    void applySettings(const AppSettings& settings);
    [[nodiscard]] AppSettings settingsFromUi() const;
    void saveSettings(bool showConfirmation = true);
    void scheduleSave();
    void restoreDefaults();

    void setLanguage(Language language);
    void updateQtTranslator();
    [[nodiscard]] QString text(const char* english, const char* russian) const;
    void retranslateUi();
    void showTabHelp(QWidget* page);
    void updateWindowSizeForCurrentTab();
    void updateLedSummary();
    void updateSamplingDepthUi();
    void updateSamplingOverlay();
    void updateRegionColorModeUi();
    void updateAudioUi();
    void updateAudioColorButton();
    void updateSolidColorButton();
    void updateHotkeyStatusUi();
    void updateLedTestUi();
    void showLedLayoutDialog();
    void updateManualAnimationUi();
    void updateConnectionShortcut();
    void updateDiagnostics();
    void updateEnabledState();
    void refreshCaptureOutputs();
    void applyCaptureOutputs(
        const QVector<capture::CaptureOutput>& outputs,
        const QString& errorMessage);
    void scheduleCaptureUpdate();
    [[nodiscard]] bool captureShouldBeActive() const;
    void updateCaptureConfiguration();
    void clearCapturedFrame(bool clearLedOutput = true);
    void updatePreviewFromLastCapture();
    void handleCaptureSample(const capture::CaptureSample& sample);
    void handleCaptureStatus(const capture::CaptureStatus& status);
    void stopCapture();
    void refreshAudioDevices();
    void scheduleAudioUpdate();
    [[nodiscard]] bool audioShouldBeActive() const;
    void updateAudioConfiguration();
    void clearAudioFrame(bool clearLedOutput = true);
    void handleAudioFrame(const audio::AudioFrame& frame);
    void handleAudioStatus(const audio::AudioStatus& status);
    void stopAudio();
    void refreshSerialPorts();
    void applySerialPorts(
        const QVector<transport::SerialPortDescriptor>& ports,
        const QString& errorMessage = {});
    void scheduleTransportUpdate();
    void updateTransportConfiguration();
    void synchronizeDeviceConfiguration();
    void handleTransportStatus(
        const transport::ConnectionStatus& status);
    void handleTransportCommand(
        const transport::CommandResult& result);
    void stopTransport();
    [[nodiscard]] QString transportStateText() const;
    [[nodiscard]] QByteArray makePhysicalLedFrame() const;
    [[nodiscard]] QByteArray makeLayoutTestFrame() const;
    void submitLayoutTestFrame();
    [[nodiscard]] int effectiveSamplingDepthPixels() const;
    void showWindow();
    void exitApplication();
    void finishExit();

    bool registerGlobalHotkey(
        const QKeySequence& sequence,
        HotkeyStatus* failureStatus);
    void unregisterGlobalHotkey();
    void applyHotkey(const QKeySequence& sequence);

    [[nodiscard]] QString comboValue(const QComboBox* combo) const;
    [[nodiscard]] QString serialPortValue() const;
    static void selectComboValue(QComboBox* combo, const QString& value);

    Language language_ = Language::Russian;
    bool exiting_ = false;
    bool exitFinished_ = false;
    bool trayNoticeShown_ = false;
    bool applyingSettings_ = false;
    QHash<QWidget*, QSize> tabWindowSizes_;
    QWidget* lastSizedPage_ = nullptr;
    quint64 tabResizeGeneration_ = 0;
    QKeySequence configuredHotkey_;
    QKeySequence registeredHotkey_;
    HotkeyStatus hotkeyStatus_ = HotkeyStatus::Disabled;

    QSettings settings_;
    QTimer* saveTimer_ = nullptr;
    QTimer* captureUpdateTimer_ = nullptr;
    QTimer* audioUpdateTimer_ = nullptr;
    QTimer* transportUpdateTimer_ = nullptr;
    QTimer* layoutTestRefreshTimer_ = nullptr;
    QTranslator* qtTranslator_ = nullptr;
    capture::ScreenCaptureWorker* captureWorker_ = nullptr;
    audio::AudioCaptureWorker* audioWorker_ = nullptr;
    transport::SerialTransportWorker* transportWorker_ = nullptr;

    quint64 lastCaptureGeneration_ = 0;
    quint64 capturedFrameNumber_ = 0;
    QSize capturedFrameSize_;
    QMargins capturedContentInsets_;
    double measuredCaptureFps_ = 0.0;
    double captureProcessingMilliseconds_ = 0.0;
    QString captureStatusText_;
    QVector<capture::CaptureOutput> captureOutputs_;
    QString captureOutputEnumerationError_;
    quint64 lastAudioGeneration_ = 0;
    quint64 audioFrameNumber_ = 0;
    double measuredAudioFps_ = 0.0;
    double audioPeakLevel_ = 0.0;
    audio::AudioStatus audioStatus_;
    QString audioDeviceEnumerationError_;
    transport::ConnectionStatus transportStatus_;
    transport::DeviceInformation deviceInformation_;
    transport::DeviceState deviceState_;
    transport::TransportStatistics transportStatistics_;
    QString serialPortEnumerationError_;
    bool serialPortRefreshInProgress_ = false;
    LayoutTestPattern layoutTestPattern_ = LayoutTestPattern::AllSides;
    QString lastTransportCommandError_;
    LedFrameFilter ledFrameFilter_;
    QByteArray lastLedFrame_;
    QVector<QColor> lastLogicalLedColors_;
    QVector<transport::SerialPortDescriptor> serialPorts_;

    QWidget* centralWidget_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QWidget* generalPage_ = nullptr;
    QWidget* screenPage_ = nullptr;
    QWidget* audioPage_ = nullptr;
    QWidget* ledPage_ = nullptr;
    QWidget* connectionPage_ = nullptr;
    QWidget* diagnosticsPage_ = nullptr;

    QGroupBox* generalSettingsGroup_ = nullptr;
    QGroupBox* lightingModeGroup_ = nullptr;
    QComboBox* languageCombo_ = nullptr;
    QCheckBox* lightingEnabledCheck_ = nullptr;
    QCheckBox* autostartCheck_ = nullptr;
    QCheckBox* closeToTrayCheck_ = nullptr;
    QKeySequenceEdit* hotkeyEdit_ = nullptr;
    QLabel* languageLabel_ = nullptr;
    QLabel* hotkeyLabel_ = nullptr;
    QLabel* hotkeyHintLabel_ = nullptr;
    QLabel* hotkeyStatusLabel_ = nullptr;
    QPushButton* generalHelpButton_ = nullptr;

    QGroupBox* screenSourceGroup_ = nullptr;
    QGroupBox* screenColorGroup_ = nullptr;
    QSpinBox* screenWidthSpin_ = nullptr;
    QSpinBox* screenHeightSpin_ = nullptr;
    QSpinBox* fpsSpin_ = nullptr;
    QCheckBox* gamingModeCheck_ = nullptr;
    QLabel* gamingModeHintLabel_ = nullptr;
    QSpinBox* zonesSpin_ = nullptr;
    QComboBox* regionColorModeCombo_ = nullptr;
    QComboBox* ledTransferModeCombo_ = nullptr;
    QComboBox* smoothingCombo_ = nullptr;
    QCheckBox* samplingDepthAutoCheck_ = nullptr;
    QSpinBox* samplingDepthSpin_ = nullptr;
    QComboBox* blackBarModeCombo_ = nullptr;
    QCheckBox* flashProtectionCheck_ = nullptr;
    QComboBox* captureOutputCombo_ = nullptr;
    QPushButton* refreshOutputsButton_ = nullptr;
    LedPreviewWidget* ledPreview_ = nullptr;
    QLabel* captureOutputLabel_ = nullptr;
    QLabel* resolutionLabel_ = nullptr;
    QLabel* widthLabel_ = nullptr;
    QLabel* heightLabel_ = nullptr;
    QLabel* fpsLabel_ = nullptr;
    QLabel* zonesLabel_ = nullptr;
    QLabel* regionColorModeLabel_ = nullptr;
    QLabel* regionColorModeHintLabel_ = nullptr;
    QLabel* ledTransferModeLabel_ = nullptr;
    QLabel* samplingDepthLabel_ = nullptr;
    QLabel* samplingDepthHintLabel_ = nullptr;
    QLabel* blackBarModeLabel_ = nullptr;
    QLabel* smoothingLabel_ = nullptr;
    QLabel* temporalFilterHintLabel_ = nullptr;
    QPushButton* screenHelpButton_ = nullptr;

    QGroupBox* audioSourceGroup_ = nullptr;
    QGroupBox* audioVisualizationGroup_ = nullptr;
    QComboBox* audioSourceCombo_ = nullptr;
    QComboBox* audioOutputDeviceCombo_ = nullptr;
    QComboBox* audioInputDeviceCombo_ = nullptr;
    QPushButton* refreshAudioDevicesButton_ = nullptr;
    QSpinBox* audioOutputGainSpin_ = nullptr;
    QSpinBox* audioMicrophoneGainSpin_ = nullptr;
    QSpinBox* audioBandsSpin_ = nullptr;
    QSpinBox* audioMinimumFrequencySpin_ = nullptr;
    QSpinBox* audioMaximumFrequencySpin_ = nullptr;
    QSpinBox* audioSensitivitySpin_ = nullptr;
    QSpinBox* audioNoiseGateSpin_ = nullptr;
    QSpinBox* audioAttackSpin_ = nullptr;
    QSpinBox* audioReleaseSpin_ = nullptr;
    QComboBox* audioDynamicsCombo_ = nullptr;
    QSpinBox* audioSpringinessSpin_ = nullptr;
    QComboBox* audioLayoutCombo_ = nullptr;
    QComboBox* audioColorModeCombo_ = nullptr;
    QPushButton* audioSolidColorButton_ = nullptr;
    QPushButton* audioDefaultsButton_ = nullptr;
    QPushButton* audioStartButton_ = nullptr;
    QProgressBar* audioLevelMeter_ = nullptr;
    LedPreviewWidget* audioPreview_ = nullptr;
    QLabel* audioSourceLabel_ = nullptr;
    QLabel* audioOutputDeviceLabel_ = nullptr;
    QLabel* audioInputDeviceLabel_ = nullptr;
    QLabel* audioOutputGainLabel_ = nullptr;
    QLabel* audioMicrophoneGainLabel_ = nullptr;
    QLabel* audioBandsLabel_ = nullptr;
    QLabel* audioFrequencyRangeLabel_ = nullptr;
    QLabel* audioSensitivityLabel_ = nullptr;
    QLabel* audioNoiseGateLabel_ = nullptr;
    QLabel* audioAttackLabel_ = nullptr;
    QLabel* audioReleaseLabel_ = nullptr;
    QLabel* audioDynamicsLabel_ = nullptr;
    QLabel* audioSpringinessLabel_ = nullptr;
    QLabel* audioLayoutLabel_ = nullptr;
    QLabel* audioColorModeLabel_ = nullptr;
    QLabel* audioStatusLabel_ = nullptr;
    QLabel* audioHintLabel_ = nullptr;
    QPushButton* audioHelpButton_ = nullptr;

    QComboBox* ledTypeCombo_ = nullptr;
    QComboBox* colorOrderCombo_ = nullptr;
    QSpinBox* topLedsSpin_ = nullptr;
    QSpinBox* rightLedsSpin_ = nullptr;
    QSpinBox* bottomLedsSpin_ = nullptr;
    QSpinBox* leftLedsSpin_ = nullptr;
    QSpinBox* ledOffsetSpin_ = nullptr;
    QComboBox* directionCombo_ = nullptr;
    QSpinBox* brightnessSpin_ = nullptr;
    QSpinBox* currentLimitSpin_ = nullptr;
    QGroupBox* whiteBalanceGroup_ = nullptr;
    QSlider* redCalibrationSlider_ = nullptr;
    QSlider* greenCalibrationSlider_ = nullptr;
    QSlider* blueCalibrationSlider_ = nullptr;
    QSpinBox* redCalibrationSpin_ = nullptr;
    QSpinBox* greenCalibrationSpin_ = nullptr;
    QSpinBox* blueCalibrationSpin_ = nullptr;
    QPushButton* testWhiteBalanceButton_ = nullptr;
    QPushButton* resetWhiteBalanceButton_ = nullptr;
    QPushButton* layoutTestButton_ = nullptr;
    QLabel* ledTestHintLabel_ = nullptr;
    QLabel* ledTypeLabel_ = nullptr;
    QLabel* voltageHintLabel_ = nullptr;
    QLabel* colorOrderLabel_ = nullptr;
    QLabel* topLedsLabel_ = nullptr;
    QLabel* rightLedsLabel_ = nullptr;
    QLabel* bottomLedsLabel_ = nullptr;
    QLabel* leftLedsLabel_ = nullptr;
    QLabel* totalLedsLabel_ = nullptr;
    QLabel* offsetLabel_ = nullptr;
    QLabel* directionLabel_ = nullptr;
    QLabel* brightnessLabel_ = nullptr;
    QLabel* currentLimitLabel_ = nullptr;
    QLabel* outputLimitsHintLabel_ = nullptr;
    QLabel* redCalibrationLabel_ = nullptr;
    QLabel* greenCalibrationLabel_ = nullptr;
    QLabel* blueCalibrationLabel_ = nullptr;
    QPushButton* ledHelpButton_ = nullptr;

    QComboBox* activeModeCombo_ = nullptr;
    QComboBox* standaloneEffectCombo_ = nullptr;
    QPushButton* solidColorButton_ = nullptr;
    QPushButton* manualAnimationButton_ = nullptr;
    QLabel* activeModeLabel_ = nullptr;
    QLabel* standaloneEffectLabel_ = nullptr;
    QLabel* solidColorLabel_ = nullptr;
    QLabel* standaloneHintLabel_ = nullptr;

    QComboBox* portCombo_ = nullptr;
    QPushButton* refreshPortsButton_ = nullptr;
    QComboBox* baudCombo_ = nullptr;
    QSpinBox* ambientTimeoutSpin_ = nullptr;
    QLabel* portLabel_ = nullptr;
    QLabel* baudLabel_ = nullptr;
    QLabel* ambientTimeoutLabel_ = nullptr;
    QLabel* connectionStatusLabel_ = nullptr;
    QPushButton* connectionHelpButton_ = nullptr;

    QPlainTextEdit* diagnosticsText_ = nullptr;
    QPushButton* copyDiagnosticsButton_ = nullptr;
    QPushButton* diagnosticsHelpButton_ = nullptr;

    QPushButton* defaultsButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* quitButton_ = nullptr;
    QPushButton* connectionShortcutButton_ = nullptr;

    QSystemTrayIcon* trayIcon_ = nullptr;
    QMenu* trayMenu_ = nullptr;
    QAction* showAction_ = nullptr;
    QAction* toggleLightingAction_ = nullptr;
    QAction* quitAction_ = nullptr;
};

} // namespace ambilight
