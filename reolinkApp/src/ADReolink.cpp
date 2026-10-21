#include "ADReolink.h"

#include "NDArray.h"
#include "NDAttribute.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>

#include <asynDriver.h>
#include <epicsExit.h>
#include <epicsExport.h>
#include <epicsThread.h>
#include <epicsTime.h>
#include <iocsh.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {

const char *driverVersion = "1.0.0";
std::once_flag ffmpegInitFlag;

void ffmpegLog(void *, int level, const char *format, va_list arguments) {
    if (level > AV_LOG_WARNING) {
        return;
    }
    char message[1024];
    vsnprintf(message, sizeof(message), format, arguments);
    const std::string safe = ReolinkClient::redact(message);
    fprintf(stderr, "FFmpeg: %s", safe.c_str());
}

void initializeFFmpeg() {
    avformat_network_init();
    av_log_set_callback(ffmpegLog);
}

int saturatingIncrement(int value) {
    return value < 2147483647 ? value + 1 : value;
}

} // namespace

void ADReolink::streamTaskC(void *privateData) {
    static_cast<ADReolink *>(privateData)->streamTask();
}

void ADReolink::controlTaskC(void *privateData) {
    static_cast<ADReolink *>(privateData)->controlTask();
}

void ADReolink::shutdownC(void *privateData) {
    static_cast<ADReolink *>(privateData)->requestShutdown();
}

int ADReolink::ffmpegInterruptC(void *privateData) {
    ADReolink *driver = static_cast<ADReolink *>(privateData);
    return driver->shutdown_.load() || !driver->acquireRequested_.load();
}

double ADReolink::monotonicSeconds() {
    epicsTimeStamp now;
    epicsTimeGetMonotonic(&now);
    return static_cast<double>(now.secPastEpoch) +
           static_cast<double>(now.nsec) / 1.0e9;
}

std::string ADReolink::percentEncode(const std::string &value) {
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (std::string::const_iterator it = value.begin(); it != value.end();
         ++it) {
        const unsigned char character = static_cast<unsigned char>(*it);
        if (std::isalnum(character) || character == '-' || character == '_' ||
            character == '.' || character == '~') {
            encoded << static_cast<char>(character);
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0')
                    << static_cast<int>(character);
        }
    }
    return encoded.str();
}

std::string ADReolink::ffmpegError(int status) {
    char buffer[AV_ERROR_MAX_STRING_SIZE];
    if (av_strerror(status, buffer, sizeof(buffer)) == 0) {
        return ReolinkClient::redact(buffer);
    }
    std::ostringstream message;
    message << "FFmpeg error " << status;
    return message.str();
}

const char *ADReolink::ptzOperation(int direction) {
    static const char *operations[] = {
        "Stop", "Up", "Down", "Left", "Right", "LeftUp", "RightUp",
        "LeftDown", "RightDown"};
    if (direction < 0 || direction >=
                             static_cast<int>(sizeof(operations) /
                                              sizeof(operations[0]))) {
        return NULL;
    }
    return operations[direction];
}

ADReolink::ADReolink(const char *portName, const char *host, int channel,
                     int rtspPort, int httpPort, const char *usernameEnv,
                     const char *passwordEnv, int maxBuffers,
                     size_t maxMemory, int priority, int stackSize)
    : ADDriver(portName, 1, NUM_ADREOLINK_PARAMS, maxBuffers, maxMemory, 0, 0,
               0, 1, priority, stackSize),
      portName_(portName != NULL ? portName : ""),
      host_(host != NULL ? host : ""), channel_(channel),
      rtspPort_(rtspPort > 0 ? rtspPort : 554),
      httpPort_(httpPort > 0 ? httpPort : 80), shutdown_(false),
      acquireRequested_(false), streamEvent_(new epicsEvent()),
      controlEvent_(new epicsEvent()), client_(NULL),
      capabilitiesKnown_(false), ptzMoving_(false), ptzDeadline_(0.0),
      lastControlCommand_(0.0), lastControlPoll_(0.0),
      lastPublishedWallTime_(0.0), formatContext_(NULL),
      codecContext_(NULL), swsContext_(NULL), decodeFrame_(NULL),
      videoStreamIndex_(-1), sourceWidth_(0), sourceHeight_(0) {
    const char *username =
        usernameEnv != NULL ? getenv(usernameEnv) : NULL;
    const char *password =
        passwordEnv != NULL ? getenv(passwordEnv) : NULL;
    username_ = username != NULL ? username : "";
    password_ = password != NULL ? password : "";

    std::ostringstream rtsp;
    rtsp << "rtsp://";
    if (!username_.empty()) {
        rtsp << percentEncode(username_) << ":" << percentEncode(password_)
             << "@";
    }
    rtsp << host_ << ":" << rtspPort_ << "/Preview_"
         << std::setw(2) << std::setfill('0') << channel_ + 1 << "_main";
    rtspUrl_ = rtsp.str();

    std::call_once(ffmpegInitFlag, initializeFFmpeg);

    createParam(ReolinkPTZDirectionString, asynParamInt32,
                &ReolinkPTZDirection);
    createParam(ReolinkPTZStopString, asynParamInt32, &ReolinkPTZStop);
    createParam(ReolinkPTZSpeedString, asynParamInt32, &ReolinkPTZSpeed);
    createParam(ReolinkPTZMoveTimeoutString, asynParamFloat64,
                &ReolinkPTZMoveTimeout);
    createParam(ReolinkZoomString, asynParamInt32, &ReolinkZoom);
    createParam(ReolinkZoomMinString, asynParamInt32, &ReolinkZoomMin);
    createParam(ReolinkZoomMaxString, asynParamInt32, &ReolinkZoomMax);
    createParam(ReolinkFocusString, asynParamInt32, &ReolinkFocus);
    createParam(ReolinkFocusMinString, asynParamInt32, &ReolinkFocusMin);
    createParam(ReolinkFocusMaxString, asynParamInt32, &ReolinkFocusMax);
    createParam(ReolinkAutoFocusString, asynParamInt32, &ReolinkAutoFocus);
    createParam(ReolinkRefocusString, asynParamInt32, &ReolinkRefocus);
    createParam(ReolinkPTZSupportedString, asynParamInt32,
                &ReolinkPTZSupported);
    createParam(ReolinkPTZSpeedSupportedString, asynParamInt32,
                &ReolinkPTZSpeedSupported);
    createParam(ReolinkZoomSupportedString, asynParamInt32,
                &ReolinkZoomSupported);
    createParam(ReolinkFocusSupportedString, asynParamInt32,
                &ReolinkFocusSupported);
    createParam(ReolinkAutoFocusSupportedString, asynParamInt32,
                &ReolinkAutoFocusSupported);
    createParam(ReolinkRTSPConnectedString, asynParamInt32,
                &ReolinkRTSPConnected);
    createParam(ReolinkControlConnectedString, asynParamInt32,
                &ReolinkControlConnected);
    createParam(ReolinkCapabilityStatusString, asynParamOctet,
                &ReolinkCapabilityStatus);
    createParam(ReolinkMeasuredFPSString, asynParamFloat64,
                &ReolinkMeasuredFPS);
    createParam(ReolinkPublishedFPSString, asynParamFloat64,
                &ReolinkPublishedFPS);
    createParam(ReolinkFrameAgeString, asynParamFloat64, &ReolinkFrameAge);
    createParam(ReolinkDroppedFramesString, asynParamInt32,
                &ReolinkDroppedFrames);
    createParam(ReolinkReconnectCountString, asynParamInt32,
                &ReolinkReconnectCount);
    createParam(ReolinkLastErrorString, asynParamOctet, &ReolinkLastError);

    setStringParam(ADManufacturer, "Reolink");
    setStringParam(ADModel, "Unknown");
    setStringParam(ADSerialNumber, host_.c_str());
    setStringParam(ADFirmwareVersion, "Unknown");
    setStringParam(NDDriverVersion, driverVersion);
    setIntegerParam(ADMaxSizeX, 3840);
    setIntegerParam(ADMaxSizeY, 2160);
    setIntegerParam(ADSizeX, 3840);
    setIntegerParam(ADSizeY, 2160);
    setIntegerParam(NDArraySizeX, 3840);
    setIntegerParam(NDArraySizeY, 2160);
    setIntegerParam(NDDataType, NDUInt8);
    setIntegerParam(NDColorMode, NDColorModeRGB1);
    setIntegerParam(NDArrayCallbacks, 1);
    setIntegerParam(ADImageMode, ADImageContinuous);
    setIntegerParam(ADNumImages, 1);
    setIntegerParam(ADNumImagesCounter, 0);
    setIntegerParam(ADAcquire, 0);
    setDoubleParam(ADAcquirePeriod, 0.25);
    setDoubleParam(ADAcquireTime, 0.0);
    setIntegerParam(ADStatus, ADStatusIdle);
    setStringParam(ADStatusMessage, "Idle");

    setIntegerParam(ReolinkPTZDirection, 0);
    setIntegerParam(ReolinkPTZStop, 0);
    setIntegerParam(ReolinkPTZSpeed, 20);
    setDoubleParam(ReolinkPTZMoveTimeout, 2.0);
    setIntegerParam(ReolinkRefocus, 0);
    setIntegerParam(ReolinkRTSPConnected, 0);
    setIntegerParam(ReolinkControlConnected, 0);
    setStringParam(ReolinkCapabilityStatus, "Not discovered");
    setDoubleParam(ReolinkMeasuredFPS, 0.0);
    setDoubleParam(ReolinkPublishedFPS, 0.0);
    setDoubleParam(ReolinkFrameAge, -1.0);
    setIntegerParam(ReolinkDroppedFrames, 0);
    setIntegerParam(ReolinkReconnectCount, 0);
    setStringParam(ReolinkLastError, "");
    updateCapabilityParameters();
    callParamCallbacks();
    asynPortDriver::connect(this->pasynUserSelf);

    if (username_.empty() || password_.empty()) {
        std::ostringstream error;
        error << "Credentials unavailable; set "
              << (usernameEnv != NULL ? usernameEnv : "<username env>")
              << " and "
              << (passwordEnv != NULL ? passwordEnv : "<password env>")
              << " before starting the IOC";
        setStringParam(ReolinkLastError, error.str().c_str());
    } else {
        client_ =
            new ReolinkClient(host_, httpPort_, channel_, username_, password_);
    }

    const std::string streamThreadName = portName_ + "_RTSP";
    if (epicsThreadCreate(streamThreadName.c_str(), epicsThreadPriorityHigh,
                          epicsThreadGetStackSize(epicsThreadStackMedium),
                          streamTaskC, this) == NULL) {
        setStringParam(ReolinkLastError, "Failed to start RTSP worker");
        setIntegerParam(ADStatus, ADStatusError);
    }

    const std::string controlThreadName = portName_ + "_CTRL";
    if (epicsThreadCreate(controlThreadName.c_str(), epicsThreadPriorityMedium,
                          epicsThreadGetStackSize(epicsThreadStackMedium),
                          controlTaskC, this) == NULL) {
        setStringParam(ReolinkLastError, "Failed to start control worker");
        setIntegerParam(ADStatus, ADStatusError);
    }
    epicsAtExit(shutdownC, this);
    controlEvent_->signal();
}

ADReolink::~ADReolink() {
    requestShutdown();
    closeStream();
    delete client_;
    delete streamEvent_;
    delete controlEvent_;
}

void ADReolink::requestShutdown() {
    shutdown_.store(true);
    acquireRequested_.store(false);
    streamEvent_->signal();
    controlEvent_->signal();
}

void ADReolink::setLastError(const std::string &message) {
    const std::string safe = ReolinkClient::redact(message);
    lock();
    setStringParam(ReolinkLastError, safe.c_str());
    callParamCallbacks();
    unlock();
}

void ADReolink::setStreamConnection(bool connected,
                                     const std::string &message) {
    lock();
    setIntegerParam(ReolinkRTSPConnected, connected ? 1 : 0);
    if (connected) {
        setIntegerParam(ADStatus, ADStatusAcquire);
        setStringParam(ADStatusMessage, "Streaming");
    } else if (acquireRequested_.load()) {
        setIntegerParam(ADStatus, ADStatusWaiting);
        setStringParam(ADStatusMessage, message.c_str());
    } else {
        setIntegerParam(ADStatus, ADStatusIdle);
        setStringParam(ADStatusMessage, "Idle");
    }
    callParamCallbacks();
    unlock();
}

void ADReolink::setControlConnection(bool connected,
                                      const std::string &error) {
    lock();
    setIntegerParam(ReolinkControlConnected, connected ? 1 : 0);
    if (!error.empty()) {
        const std::string safe = ReolinkClient::redact(error);
        setStringParam(ReolinkLastError, safe.c_str());
    }
    callParamCallbacks();
    unlock();
}

void ADReolink::updateCapabilityParameters() {
    setIntegerParam(ReolinkPTZSupported, capabilities_.ptz ? 1 : 0);
    setIntegerParam(ReolinkPTZSpeedSupported,
                    capabilities_.ptzSpeed ? 1 : 0);
    setIntegerParam(ReolinkZoomSupported, capabilities_.zoom ? 1 : 0);
    setIntegerParam(ReolinkFocusSupported, capabilities_.focus ? 1 : 0);
    setIntegerParam(ReolinkAutoFocusSupported,
                    capabilities_.autoFocus ? 1 : 0);
    setIntegerParam(ReolinkZoomMin, capabilities_.zoomMin);
    setIntegerParam(ReolinkZoomMax, capabilities_.zoomMax);
    setIntegerParam(ReolinkZoom, capabilities_.zoomPosition);
    setIntegerParam(ReolinkFocusMin, capabilities_.focusMin);
    setIntegerParam(ReolinkFocusMax, capabilities_.focusMax);
    setIntegerParam(ReolinkFocus, capabilities_.focusPosition);
    setIntegerParam(ReolinkAutoFocus,
                    capabilities_.autoFocusEnabled ? 1 : 0);
}

void ADReolink::discoverCapabilities() {
    if (client_ == NULL) {
        setControlConnection(false, "Camera credentials are unavailable");
        return;
    }

    ReolinkCapabilities discovered;
    std::string error;
    if (!client_->discover(discovered, error)) {
        setControlConnection(false, error);
        return;
    }

    capabilities_ = discovered;
    capabilitiesKnown_ = true;
    lock();
    updateCapabilityParameters();
    if (!capabilities_.model.empty()) {
        setStringParam(ADModel, capabilities_.model.c_str());
    }
    if (!capabilities_.firmware.empty()) {
        setStringParam(ADFirmwareVersion, capabilities_.firmware.c_str());
    }
    std::ostringstream status;
    status << "PTZ=" << (capabilities_.ptz ? "yes" : "no")
           << ", speed=" << (capabilities_.ptzSpeed ? "yes" : "no")
           << ", zoom=" << (capabilities_.zoom ? "yes" : "no")
           << ", focus=" << (capabilities_.focus ? "yes" : "no")
           << ", autofocus=" << (capabilities_.autoFocus ? "yes" : "no");
    setStringParam(ReolinkCapabilityStatus, status.str().c_str());
    setIntegerParam(ReolinkControlConnected, 1);
    setStringParam(ReolinkLastError, "");
    callParamCallbacks();
    unlock();
}

void ADReolink::enqueue(const ControlCommand &command) {
    controlMutex_.lock();
    if (command.type == CommandStop) {
        for (std::deque<ControlCommand>::iterator it = commands_.begin();
             it != commands_.end();) {
            if (it->type == CommandPTZ) {
                it = commands_.erase(it);
            } else {
                ++it;
            }
        }
        commands_.push_front(command);
    } else if (command.type == CommandZoom ||
               command.type == CommandFocus ||
               command.type == CommandPTZ) {
        for (std::deque<ControlCommand>::iterator it = commands_.begin();
             it != commands_.end();) {
            if (it->type == command.type) {
                it = commands_.erase(it);
            } else {
                ++it;
            }
        }
        commands_.push_back(command);
    } else {
        commands_.push_back(command);
    }
    controlMutex_.unlock();
    controlEvent_->signal();
}

bool ADReolink::popCommand(ControlCommand &command) {
    bool available = false;
    controlMutex_.lock();
    if (!commands_.empty()) {
        command = commands_.front();
        commands_.pop_front();
        available = true;
    }
    controlMutex_.unlock();
    return available;
}

bool ADReolink::executeCommand(const ControlCommand &command,
                               std::string &error) {
    if (client_ == NULL) {
        error = "Camera credentials are unavailable";
        return false;
    }

    int speed = 20;
    double moveTimeout = 2.0;
    lock();
    getIntegerParam(ReolinkPTZSpeed, &speed);
    getDoubleParam(ReolinkPTZMoveTimeout, &moveTimeout);
    unlock();

    bool result = false;
    switch (command.type) {
    case CommandDiscover:
        discoverCapabilities();
        return capabilitiesKnown_;
    case CommandPTZ: {
        const char *operation = ptzOperation(command.value);
        if (operation == NULL) {
            error = "Invalid PTZ direction";
            return false;
        }
        result = client_->ptz(
            operation, capabilities_.ptzSpeed ? speed : 0, error);
        if (result) {
            ptzMoving_ = command.value != 0;
            ptzDeadline_ =
                ptzMoving_ ? monotonicSeconds() + moveTimeout : 0.0;
            lock();
            setIntegerParam(ReolinkPTZDirection, command.value);
            callParamCallbacks();
            unlock();
        }
        break;
    }
    case CommandStop:
        // Reolink's web client always omits speed from Stop.
        result = client_->ptz("Stop", 0, error);
        ptzMoving_ = false;
        ptzDeadline_ = 0.0;
        lock();
        setIntegerParam(ReolinkPTZDirection, 0);
        setIntegerParam(ReolinkPTZStop, 0);
        callParamCallbacks();
        unlock();
        break;
    case CommandZoom:
        result = client_->setZoom(command.value, error);
        break;
    case CommandFocus:
        result = client_->setFocus(command.value, error);
        break;
    case CommandAutoFocus:
        result = client_->setAutoFocus(command.value != 0, error);
        break;
    case CommandRefocus:
        // Reolink has no separate one-shot autofocus command. With autofocus
        // enabled, reissuing the current ZoomPos runs the normal autofocus
        // cycle without changing magnification.
        result = client_->getZoomFocus(capabilities_, error);
        if (result) {
            result = client_->setZoom(capabilities_.zoomPosition, error);
        }
        lock();
        setIntegerParam(ReolinkRefocus, 0);
        callParamCallbacks();
        unlock();
        break;
    }
    lastControlCommand_ = monotonicSeconds();
    return result;
}

void ADReolink::pollControlStatus() {
    if (client_ == NULL || !capabilitiesKnown_) {
        return;
    }
    std::string error;
    bool success = true;
    if (capabilities_.zoom || capabilities_.focus) {
        success = client_->getZoomFocus(capabilities_, error);
    }
    if (success && capabilities_.autoFocus) {
        bool enabled = false;
        success = client_->getAutoFocus(enabled, error);
        if (success) {
            capabilities_.autoFocusEnabled = enabled;
        }
    }
    if (!success) {
        setControlConnection(false, error);
        return;
    }
    lock();
    updateCapabilityParameters();
    setIntegerParam(ReolinkControlConnected, 1);
    const double age = lastPublishedWallTime_ > 0.0
                           ? monotonicSeconds() - lastPublishedWallTime_
                           : -1.0;
    setDoubleParam(ReolinkFrameAge, age);
    callParamCallbacks();
    unlock();
}

void ADReolink::controlTask() {
    double nextDiscovery = 0.0;
    while (!shutdown_.load()) {
        const double now = monotonicSeconds();
        if (!capabilitiesKnown_ && now >= nextDiscovery) {
            discoverCapabilities();
            nextDiscovery = monotonicSeconds() + 5.0;
        }

        if (ptzMoving_ && now >= ptzDeadline_) {
            std::string error;
            const bool success =
                executeCommand(ControlCommand(CommandStop, 0), error);
            setControlConnection(success, success ? "" : error);
        }

        ControlCommand command(CommandDiscover, 0);
        if (popCommand(command)) {
            std::string error;
            const bool success = executeCommand(command, error);
            setControlConnection(success, success ? "" : error);
        }

        const double pollInterval =
            (ptzMoving_ || now - lastControlCommand_ < 3.0) ? 0.25 : 2.0;
        if (now - lastControlPoll_ >= pollInterval) {
            pollControlStatus();
            lastControlPoll_ = monotonicSeconds();
        }
        controlEvent_->wait(pollInterval);
    }
}

asynStatus ADReolink::writeInt32(asynUser *pasynUser, epicsInt32 value) {
    const int function = pasynUser->reason;

    if (function == ADAcquire) {
        if (value != 0 && value != 1) {
            return asynError;
        }
        setIntegerParam(ADAcquire, value);
        acquireRequested_.store(value != 0);
        if (value) {
            setIntegerParam(ADNumImagesCounter, 0);
            setIntegerParam(ADStatus, ADStatusWaiting);
            setStringParam(ADStatusMessage, "Connecting to RTSP");
        } else {
            setIntegerParam(ADStatus, ADStatusIdle);
            setStringParam(ADStatusMessage, "Idle");
        }
        callParamCallbacks();
        streamEvent_->signal();
        return asynSuccess;
    }

    if (function == ReolinkPTZDirection) {
        int supported = 0;
        getIntegerParam(ReolinkPTZSupported, &supported);
        if (!supported || ptzOperation(value) == NULL) {
            return asynError;
        }
        enqueue(value == 0 ? ControlCommand(CommandStop, 0)
                           : ControlCommand(CommandPTZ, value));
        return asynSuccess;
    }
    if (function == ReolinkPTZStop) {
        if (value) {
            enqueue(ControlCommand(CommandStop, 0));
        }
        return asynSuccess;
    }
    if (function == ReolinkPTZSpeed) {
        int supported = 0;
        getIntegerParam(ReolinkPTZSpeedSupported, &supported);
        if (!supported || value < 1 || value > 64) {
            return asynError;
        }
        setIntegerParam(ReolinkPTZSpeed, value);
        callParamCallbacks();
        return asynSuccess;
    }
    if (function == ReolinkZoom) {
        int supported = 0;
        int minimum = 0;
        int maximum = 0;
        getIntegerParam(ReolinkZoomSupported, &supported);
        getIntegerParam(ReolinkZoomMin, &minimum);
        getIntegerParam(ReolinkZoomMax, &maximum);
        if (!supported || value < minimum || value > maximum) {
            return asynError;
        }
        enqueue(ControlCommand(CommandZoom, value));
        return asynSuccess;
    }
    if (function == ReolinkFocus) {
        int supported = 0;
        int minimum = 0;
        int maximum = 0;
        getIntegerParam(ReolinkFocusSupported, &supported);
        getIntegerParam(ReolinkFocusMin, &minimum);
        getIntegerParam(ReolinkFocusMax, &maximum);
        if (!supported || value < minimum || value > maximum) {
            return asynError;
        }
        enqueue(ControlCommand(CommandFocus, value));
        return asynSuccess;
    }
    if (function == ReolinkAutoFocus) {
        int supported = 0;
        getIntegerParam(ReolinkAutoFocusSupported, &supported);
        if (!supported || (value != 0 && value != 1)) {
            return asynError;
        }
        enqueue(ControlCommand(CommandAutoFocus, value));
        return asynSuccess;
    }
    if (function == ReolinkRefocus) {
        setIntegerParam(ReolinkRefocus, value != 0 ? 1 : 0);
        callParamCallbacks();
        if (!value) {
            return asynSuccess;
        }
        int zoomSupported = 0;
        int autoFocusSupported = 0;
        int autoFocusEnabled = 0;
        getIntegerParam(ReolinkZoomSupported, &zoomSupported);
        getIntegerParam(ReolinkAutoFocusSupported, &autoFocusSupported);
        getIntegerParam(ReolinkAutoFocus, &autoFocusEnabled);
        if (!zoomSupported || !autoFocusSupported || !autoFocusEnabled) {
            setIntegerParam(ReolinkRefocus, 0);
            callParamCallbacks();
            return asynError;
        }
        enqueue(ControlCommand(CommandRefocus, 0));
        return asynSuccess;
    }

    return ADDriver::writeInt32(pasynUser, value);
}

asynStatus ADReolink::writeFloat64(asynUser *pasynUser,
                                    epicsFloat64 value) {
    const int function = pasynUser->reason;
    if (function == ADAcquirePeriod) {
        if (value < 0.1 || value > 5.0) {
            return asynError;
        }
        setDoubleParam(ADAcquirePeriod, value);
        callParamCallbacks();
        return asynSuccess;
    }
    if (function == ReolinkPTZMoveTimeout) {
        if (value < 0.1 || value > 30.0) {
            return asynError;
        }
        setDoubleParam(ReolinkPTZMoveTimeout, value);
        callParamCallbacks();
        return asynSuccess;
    }
    return ADDriver::writeFloat64(pasynUser, value);
}

bool ADReolink::openStream(std::string &error) {
    closeStream();
    formatContext_ = avformat_alloc_context();
    if (formatContext_ == NULL) {
        error = "Unable to allocate FFmpeg format context";
        return false;
    }
    formatContext_->interrupt_callback.callback = ffmpegInterruptC;
    formatContext_->interrupt_callback.opaque = this;

    AVDictionary *options = NULL;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "stimeout", "5000000", 0);
    av_dict_set(&options, "rw_timeout", "5000000", 0);
    av_dict_set(&options, "fflags", "nobuffer", 0);
    av_dict_set(&options, "flags", "low_delay", 0);

    int status =
        avformat_open_input(&formatContext_, rtspUrl_.c_str(), NULL, &options);
    av_dict_free(&options);
    if (status < 0) {
        error = std::string("Opening RTSP stream failed: ") +
                ffmpegError(status);
        closeStream();
        return false;
    }
    status = avformat_find_stream_info(formatContext_, NULL);
    if (status < 0) {
        error = std::string("Reading RTSP stream information failed: ") +
                ffmpegError(status);
        closeStream();
        return false;
    }

    const AVCodec *decoder = NULL;
    videoStreamIndex_ =
        av_find_best_stream(formatContext_, AVMEDIA_TYPE_VIDEO, -1, -1,
                            &decoder, 0);
    if (videoStreamIndex_ < 0 || decoder == NULL) {
        error = "RTSP stream does not contain a supported video track";
        closeStream();
        return false;
    }

    codecContext_ = avcodec_alloc_context3(decoder);
    if (codecContext_ == NULL) {
        error = "Unable to allocate FFmpeg decoder context";
        closeStream();
        return false;
    }
    status = avcodec_parameters_to_context(
        codecContext_,
        formatContext_->streams[videoStreamIndex_]->codecpar);
    if (status < 0) {
        error = std::string("Copying video parameters failed: ") +
                ffmpegError(status);
        closeStream();
        return false;
    }
    codecContext_->thread_count = 0;
    codecContext_->thread_type = FF_THREAD_FRAME;
    status = avcodec_open2(codecContext_, decoder, NULL);
    if (status < 0) {
        error = std::string("Opening video decoder failed: ") +
                ffmpegError(status);
        closeStream();
        return false;
    }

    decodeFrame_ = av_frame_alloc();
    if (decodeFrame_ == NULL) {
        error = "Unable to allocate FFmpeg video frame";
        closeStream();
        return false;
    }
    sourceWidth_ = codecContext_->width;
    sourceHeight_ = codecContext_->height;
    if (sourceWidth_ <= 0 || sourceHeight_ <= 0) {
        error = "RTSP stream reported an invalid frame size";
        closeStream();
        return false;
    }

    lock();
    setIntegerParam(ADMaxSizeX, sourceWidth_);
    setIntegerParam(ADMaxSizeY, sourceHeight_);
    setIntegerParam(ADSizeX, sourceWidth_);
    setIntegerParam(ADSizeY, sourceHeight_);
    setIntegerParam(NDArraySizeX, sourceWidth_);
    setIntegerParam(NDArraySizeY, sourceHeight_);
    callParamCallbacks();
    unlock();
    return true;
}

void ADReolink::closeStream() {
    if (swsContext_ != NULL) {
        sws_freeContext(swsContext_);
        swsContext_ = NULL;
    }
    if (decodeFrame_ != NULL) {
        av_frame_free(&decodeFrame_);
    }
    if (codecContext_ != NULL) {
        avcodec_free_context(&codecContext_);
    }
    if (formatContext_ != NULL) {
        avformat_close_input(&formatContext_);
    }
    videoStreamIndex_ = -1;
    sourceWidth_ = 0;
    sourceHeight_ = 0;
}

bool ADReolink::publishFrame(AVFrame *frame, std::string &error) {
    const size_t dimensions[3] = {
        3, static_cast<size_t>(frame->width),
        static_cast<size_t>(frame->height)};
    NDArray *array =
        pNDArrayPool->alloc(3, const_cast<size_t *>(dimensions), NDUInt8, 0,
                            NULL);
    if (array == NULL) {
        error = "NDArray allocation failed";
        return false;
    }

    swsContext_ =
        sws_getCachedContext(swsContext_, frame->width, frame->height,
                             static_cast<AVPixelFormat>(frame->format),
                             frame->width, frame->height, AV_PIX_FMT_RGB24,
                             SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (swsContext_ == NULL) {
        array->release();
        error = "RGB conversion context allocation failed";
        return false;
    }
    uint8_t *output[] = {static_cast<uint8_t *>(array->pData), NULL, NULL,
                         NULL};
    int outputStride[] = {frame->width * 3, 0, 0, 0};
    const int converted =
        sws_scale(swsContext_, frame->data, frame->linesize, 0, frame->height,
                  output, outputStride);
    if (converted != frame->height) {
        array->release();
        error = "RGB frame conversion failed";
        return false;
    }

    lock();
    int arrayCounter = 0;
    int imageCounter = 0;
    int arrayCallbacks = 0;
    getIntegerParam(NDArrayCounter, &arrayCounter);
    getIntegerParam(ADNumImagesCounter, &imageCounter);
    getIntegerParam(NDArrayCallbacks, &arrayCallbacks);
    ++arrayCounter;
    ++imageCounter;

    epicsTimeStamp timestamp;
    epicsTimeGetCurrent(&timestamp);
    array->uniqueId = arrayCounter;
    array->timeStamp =
        timestamp.secPastEpoch + static_cast<double>(timestamp.nsec) / 1.0e9;
    array->epicsTS = timestamp;
    int colorMode = NDColorModeRGB1;
    array->pAttributeList->add("ColorMode", "Color mode", NDAttrInt32,
                               &colorMode);
    getAttributes(array->pAttributeList);

    setIntegerParam(NDArrayCounter, arrayCounter);
    setIntegerParam(ADNumImagesCounter, imageCounter);
    setIntegerParam(NDArraySize, static_cast<int>(array->dataSize));
    setIntegerParam(NDArraySizeX, frame->width);
    setIntegerParam(NDArraySizeY, frame->height);
    setIntegerParam(ADStatus, ADStatusAcquire);
    setStringParam(ADStatusMessage, "Streaming");
    lastPublishedWallTime_ = monotonicSeconds();
    setDoubleParam(ReolinkFrameAge, 0.0);
    if (arrayCallbacks) {
        doCallbacksGenericPointer(array, NDArrayData, 0);
    }
    callParamCallbacks();

    int imageMode = ADImageContinuous;
    int numImages = 1;
    getIntegerParam(ADImageMode, &imageMode);
    getIntegerParam(ADNumImages, &numImages);
    if (imageMode == ADImageSingle ||
        (imageMode == ADImageMultiple && imageCounter >= numImages)) {
        setIntegerParam(ADAcquire, 0);
        setIntegerParam(ADStatus, ADStatusIdle);
        setStringParam(ADStatusMessage, "Idle");
        acquireRequested_.store(false);
        callParamCallbacks();
        streamEvent_->signal();
    }
    unlock();
    array->release();
    return true;
}

void ADReolink::streamTask() {
    static const double reconnectDelays[] = {1.0, 2.0, 4.0, 8.0, 15.0};
    size_t reconnectStep = 0;

    while (!shutdown_.load()) {
        if (!acquireRequested_.load()) {
            setStreamConnection(false, "Idle");
            streamEvent_->wait(1.0);
            continue;
        }

        std::string error;
        if (!openStream(error)) {
            if (!acquireRequested_.load() || shutdown_.load()) {
                closeStream();
                continue;
            }
            setStreamConnection(false, "Waiting to reconnect");
            setLastError(error);
            lock();
            int reconnectCount = 0;
            getIntegerParam(ReolinkReconnectCount, &reconnectCount);
            setIntegerParam(ReolinkReconnectCount,
                            saturatingIncrement(reconnectCount));
            callParamCallbacks();
            unlock();
            const double delay = reconnectDelays[std::min(
                reconnectStep,
                sizeof(reconnectDelays) / sizeof(reconnectDelays[0]) - 1)];
            reconnectStep =
                std::min(reconnectStep + 1,
                         sizeof(reconnectDelays) /
                                 sizeof(reconnectDelays[0]) -
                             1);
            streamEvent_->wait(delay);
            continue;
        }

        reconnectStep = 0;
        setStreamConnection(true, "Streaming");
        AVPacket *packet = av_packet_alloc();
        if (packet == NULL) {
            setLastError("Unable to allocate FFmpeg packet");
            closeStream();
            streamEvent_->wait(1.0);
            continue;
        }

        double nextPublish = 0.0;
        double rateWindowStart = monotonicSeconds();
        int sourceFrames = 0;
        int publishedFrames = 0;

        while (acquireRequested_.load() && !shutdown_.load()) {
            const int readStatus = av_read_frame(formatContext_, packet);
            if (readStatus < 0) {
                if (acquireRequested_.load() && !shutdown_.load()) {
                    setLastError(std::string("Reading RTSP stream failed: ") +
                                 ffmpegError(readStatus));
                }
                break;
            }

            if (packet->stream_index == videoStreamIndex_) {
                int decodeStatus = avcodec_send_packet(codecContext_, packet);
                if (decodeStatus >= 0 || decodeStatus == AVERROR(EAGAIN)) {
                    while (true) {
                        decodeStatus =
                            avcodec_receive_frame(codecContext_, decodeFrame_);
                        if (decodeStatus == AVERROR(EAGAIN) ||
                            decodeStatus == AVERROR_EOF) {
                            break;
                        }
                        if (decodeStatus < 0) {
                            setLastError(
                                std::string("Decoding RTSP frame failed: ") +
                                ffmpegError(decodeStatus));
                            break;
                        }

                        ++sourceFrames;
                        const double now = monotonicSeconds();
                        double acquirePeriod = 0.25;
                        lock();
                        getDoubleParam(ADAcquirePeriod, &acquirePeriod);
                        unlock();

                        if (nextPublish == 0.0) {
                            nextPublish = now;
                        }
                        if (now >= nextPublish) {
                            std::string publishError;
                            if (publishFrame(decodeFrame_, publishError)) {
                                ++publishedFrames;
                                do {
                                    nextPublish += acquirePeriod;
                                } while (nextPublish <= now);
                            } else {
                                setLastError(publishError);
                            }
                        } else {
                            lock();
                            int dropped = 0;
                            getIntegerParam(ReolinkDroppedFrames, &dropped);
                            setIntegerParam(ReolinkDroppedFrames,
                                            saturatingIncrement(dropped));
                            unlock();
                        }

                        if (now - rateWindowStart >= 2.0) {
                            const double elapsed = now - rateWindowStart;
                            lock();
                            setDoubleParam(ReolinkMeasuredFPS,
                                           sourceFrames / elapsed);
                            setDoubleParam(ReolinkPublishedFPS,
                                           publishedFrames / elapsed);
                            callParamCallbacks();
                            unlock();
                            rateWindowStart = now;
                            sourceFrames = 0;
                            publishedFrames = 0;
                        }
                    }
                } else {
                    setLastError(
                        std::string("Submitting RTSP packet failed: ") +
                        ffmpegError(decodeStatus));
                }
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        closeStream();
        setStreamConnection(false, acquireRequested_.load()
                                       ? "Waiting to reconnect"
                                       : "Idle");
    }
    closeStream();
}

extern "C" int ADReolinkConfig(const char *portName, const char *host,
                                int channel, int rtspPort, int httpPort,
                                const char *usernameEnv,
                                const char *passwordEnv, int maxBuffers,
                                size_t maxMemory, int priority, int stackSize) {
    new ADReolink(portName, host, channel, rtspPort, httpPort, usernameEnv,
                  passwordEnv, maxBuffers, maxMemory, priority, stackSize);
    return asynSuccess;
}

static const iocshArg configArg0 = {"portName", iocshArgString};
static const iocshArg configArg1 = {"host", iocshArgString};
static const iocshArg configArg2 = {"channel", iocshArgInt};
static const iocshArg configArg3 = {"RTSP port", iocshArgInt};
static const iocshArg configArg4 = {"HTTP port", iocshArgInt};
static const iocshArg configArg5 = {"username environment variable",
                                    iocshArgString};
static const iocshArg configArg6 = {"password environment variable",
                                    iocshArgString};
static const iocshArg configArg7 = {"maxBuffers", iocshArgInt};
static const iocshArg configArg8 = {"maxMemory", iocshArgInt};
static const iocshArg configArg9 = {"priority", iocshArgInt};
static const iocshArg configArg10 = {"stackSize", iocshArgInt};
static const iocshArg *const configArgs[] = {
    &configArg0, &configArg1, &configArg2, &configArg3,
    &configArg4, &configArg5, &configArg6, &configArg7,
    &configArg8, &configArg9, &configArg10};
static const iocshFuncDef configFuncDef = {"ADReolinkConfig", 11, configArgs};

static void configCallFunc(const iocshArgBuf *args) {
    ADReolinkConfig(args[0].sval, args[1].sval, args[2].ival, args[3].ival,
                    args[4].ival, args[5].sval, args[6].sval, args[7].ival,
                    static_cast<size_t>(args[8].ival), args[9].ival,
                    args[10].ival);
}

extern "C" void ADReolinkRegister(void) {
    iocshRegister(&configFuncDef, configCallFunc);
}

extern "C" {
epicsExportRegistrar(ADReolinkRegister);
}
