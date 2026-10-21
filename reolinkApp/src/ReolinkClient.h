#ifndef REOLINK_CLIENT_H
#define REOLINK_CLIENT_H

#include <string>

struct ReolinkCapabilities {
    bool ptz;
    bool ptzSpeed;
    bool zoom;
    bool focus;
    bool autoFocus;
    int zoomMin;
    int zoomMax;
    int focusMin;
    int focusMax;
    int zoomPosition;
    int focusPosition;
    bool autoFocusEnabled;
    std::string model;
    std::string firmware;

    ReolinkCapabilities();
};

class ReolinkClient {
public:
    ReolinkClient(const std::string &host, int httpPort, int channel,
                  const std::string &username, const std::string &password);
    ~ReolinkClient();

    bool login(std::string &error);
    bool discover(ReolinkCapabilities &capabilities, std::string &error);
    bool getZoomFocus(ReolinkCapabilities &capabilities, std::string &error);
    bool getAutoFocus(bool &enabled, std::string &error);
    bool ptz(const std::string &operation, int speed, std::string &error);
    bool setZoom(int position, std::string &error);
    bool setFocus(int position, std::string &error);
    bool setAutoFocus(bool enabled, std::string &error);

    static std::string redact(const std::string &message);
    static bool parseLoginResponse(const std::string &body, std::string &token,
                                   std::string &error);
    static bool parseCapabilitiesResponse(const std::string &body,
                                          ReolinkCapabilities &capabilities,
                                          std::string &error);
    static bool parseZoomFocusResponse(const std::string &body,
                                       ReolinkCapabilities &capabilities,
                                       std::string &error);
    static bool parseAutoFocusResponse(const std::string &body, bool &enabled,
                                       std::string &error);

private:
    ReolinkClient(const ReolinkClient &);
    ReolinkClient &operator=(const ReolinkClient &);

    struct Impl;
    Impl *impl_;

    bool request(const std::string &command, int action,
                 const std::string &parameterJson, std::string &body,
                 std::string &error, bool retryAuthentication = true);
    bool post(const std::string &url, const std::string &body,
              std::string &response, std::string &error);
};

#endif
