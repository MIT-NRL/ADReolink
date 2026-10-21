#ifndef AD_REOLINK_H
#define AD_REOLINK_H

#include "ADDriver.h"
#include "ReolinkClient.h"

#include <atomic>
#include <deque>
#include <string>

#include <epicsEvent.h>
#include <epicsMutex.h>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct SwsContext;

#define ReolinkPTZDirectionString        "REOLINK_PTZ_DIRECTION"
#define ReolinkPTZStopString             "REOLINK_PTZ_STOP"
#define ReolinkPTZSpeedString            "REOLINK_PTZ_SPEED"
#define ReolinkPTZMoveTimeoutString      "REOLINK_PTZ_MOVE_TIMEOUT"
#define ReolinkZoomString                "REOLINK_ZOOM"
#define ReolinkZoomMinString             "REOLINK_ZOOM_MIN"
#define ReolinkZoomMaxString             "REOLINK_ZOOM_MAX"
#define ReolinkFocusString               "REOLINK_FOCUS"
#define ReolinkFocusMinString            "REOLINK_FOCUS_MIN"
#define ReolinkFocusMaxString            "REOLINK_FOCUS_MAX"
#define ReolinkAutoFocusString           "REOLINK_AUTO_FOCUS"
#define ReolinkRefocusString             "REOLINK_REFOCUS"
#define ReolinkPTZSupportedString        "REOLINK_PTZ_SUPPORTED"
#define ReolinkPTZSpeedSupportedString   "REOLINK_PTZ_SPEED_SUPPORTED"
#define ReolinkZoomSupportedString       "REOLINK_ZOOM_SUPPORTED"
#define ReolinkFocusSupportedString      "REOLINK_FOCUS_SUPPORTED"
#define ReolinkAutoFocusSupportedString  "REOLINK_AUTO_FOCUS_SUPPORTED"
#define ReolinkRTSPConnectedString       "REOLINK_RTSP_CONNECTED"
#define ReolinkControlConnectedString    "REOLINK_CONTROL_CONNECTED"
#define ReolinkCapabilityStatusString    "REOLINK_CAPABILITY_STATUS"
#define ReolinkMeasuredFPSString         "REOLINK_MEASURED_FPS"
#define ReolinkPublishedFPSString        "REOLINK_PUBLISHED_FPS"
#define ReolinkFrameAgeString            "REOLINK_FRAME_AGE"
#define ReolinkDroppedFramesString       "REOLINK_DROPPED_FRAMES"
#define ReolinkReconnectCountString      "REOLINK_RECONNECT_COUNT"
#define ReolinkLastErrorString           "REOLINK_LAST_ERROR"

class ADReolink : public ADDriver {
public:
    ADReolink(const char *portName, const char *host, int channel,
              int rtspPort, int httpPort, const char *usernameEnv,
              const char *passwordEnv, int maxBuffers, size_t maxMemory,
              int priority, int stackSize);
    virtual ~ADReolink();

    virtual asynStatus writeInt32(asynUser *pasynUser, epicsInt32 value);
    virtual asynStatus writeFloat64(asynUser *pasynUser, epicsFloat64 value);

    void streamTask();
    void controlTask();
    void requestShutdown();

protected:
    int ReolinkPTZDirection;
    #define FIRST_ADREOLINK_PARAM ReolinkPTZDirection
    int ReolinkPTZStop;
    int ReolinkPTZSpeed;
    int ReolinkPTZMoveTimeout;
    int ReolinkZoom;
    int ReolinkZoomMin;
    int ReolinkZoomMax;
    int ReolinkFocus;
    int ReolinkFocusMin;
    int ReolinkFocusMax;
    int ReolinkAutoFocus;
    int ReolinkRefocus;
    int ReolinkPTZSupported;
    int ReolinkPTZSpeedSupported;
    int ReolinkZoomSupported;
    int ReolinkFocusSupported;
    int ReolinkAutoFocusSupported;
    int ReolinkRTSPConnected;
    int ReolinkControlConnected;
    int ReolinkCapabilityStatus;
    int ReolinkMeasuredFPS;
    int ReolinkPublishedFPS;
    int ReolinkFrameAge;
    int ReolinkDroppedFrames;
    int ReolinkReconnectCount;
    int ReolinkLastError;
    #define LAST_ADREOLINK_PARAM ReolinkLastError

private:
    enum CommandType {
        CommandDiscover,
        CommandPTZ,
        CommandStop,
        CommandZoom,
        CommandFocus,
        CommandAutoFocus,
        CommandRefocus
    };

    struct ControlCommand {
        CommandType type;
        int value;
        ControlCommand(CommandType commandType, int commandValue)
            : type(commandType), value(commandValue) {}
    };

    std::string portName_;
    std::string host_;
    int channel_;
    int rtspPort_;
    int httpPort_;
    std::string username_;
    std::string password_;
    std::string rtspUrl_;

    std::atomic<bool> shutdown_;
    std::atomic<bool> acquireRequested_;
    epicsEvent *streamEvent_;
    epicsEvent *controlEvent_;
    epicsMutex controlMutex_;
    std::deque<ControlCommand> commands_;

    ReolinkClient *client_;
    ReolinkCapabilities capabilities_;
    bool capabilitiesKnown_;
    bool ptzMoving_;
    double ptzDeadline_;
    double lastControlCommand_;
    double lastControlPoll_;
    double lastPublishedWallTime_;

    AVFormatContext *formatContext_;
    AVCodecContext *codecContext_;
    SwsContext *swsContext_;
    AVFrame *decodeFrame_;
    int videoStreamIndex_;
    int sourceWidth_;
    int sourceHeight_;

    static void streamTaskC(void *privateData);
    static void controlTaskC(void *privateData);
    static void shutdownC(void *privateData);
    static int ffmpegInterruptC(void *privateData);

    void enqueue(const ControlCommand &command);
    bool popCommand(ControlCommand &command);
    bool executeCommand(const ControlCommand &command, std::string &error);
    void discoverCapabilities();
    void updateCapabilityParameters();
    void pollControlStatus();
    void setControlConnection(bool connected, const std::string &error);

    bool openStream(std::string &error);
    void closeStream();
    bool publishFrame(AVFrame *frame, std::string &error);
    void setStreamConnection(bool connected, const std::string &message);
    void setLastError(const std::string &message);
    static std::string percentEncode(const std::string &value);
    static double monotonicSeconds();
    static std::string ffmpegError(int status);
    static const char *ptzOperation(int direction);
};

#define NUM_ADREOLINK_PARAMS ((int)(&LAST_ADREOLINK_PARAM - &FIRST_ADREOLINK_PARAM + 1))

#endif
