#include "ReolinkClient.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <sstream>

using json = nlohmann::json;

namespace {

std::once_flag curlInitFlag;

void initializeCurl() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

size_t appendResponse(char *data, size_t size, size_t count, void *user) {
    std::string *response = static_cast<std::string *>(user);
    const size_t bytes = size * count;
    response->append(data, bytes);
    return bytes;
}

bool firstResult(const std::string &body, json &result, std::string &error) {
    try {
        const json root = json::parse(body);
        if (!root.is_array() || root.empty() || !root[0].is_object()) {
            error = "camera response was not a non-empty JSON array";
            return false;
        }
        result = root[0];
        const int code = result.value("code", -1);
        if (code != 0) {
            std::ostringstream stream;
            stream << "camera API returned code " << code;
            if (result.contains("error") && result["error"].is_object()) {
                const json &apiError = result["error"];
                if (apiError.contains("detail") &&
                    apiError["detail"].is_string()) {
                    stream << ": " << apiError["detail"].get<std::string>();
                } else if (apiError.contains("rspCode")) {
                    stream << " (rspCode " << apiError["rspCode"] << ")";
                }
            }
            error = stream.str();
            return false;
        }
        return true;
    } catch (const std::exception &exception) {
        error = std::string("invalid camera JSON: ") + exception.what();
        return false;
    }
}

int capabilityVersion(const json &node, const std::string &name) {
    if (node.is_object()) {
        json::const_iterator direct = node.find(name);
        if (direct != node.end()) {
            if (direct->is_object()) {
                json::const_iterator version = direct->find("ver");
                if (version != direct->end() && version->is_number_integer()) {
                    return version->get<int>();
                }
            } else if (direct->is_number_integer()) {
                return direct->get<int>();
            }
        }
        for (json::const_iterator it = node.begin(); it != node.end(); ++it) {
            const int version = capabilityVersion(it.value(), name);
            if (version >= 0) {
                return version;
            }
        }
    } else if (node.is_array()) {
        for (json::const_iterator it = node.begin(); it != node.end(); ++it) {
            const int version = capabilityVersion(*it, name);
            if (version >= 0) {
                return version;
            }
        }
    }
    return -1;
}

const json *findNamedObject(const json &node, const std::string &name) {
    if (node.is_object()) {
        json::const_iterator direct = node.find(name);
        if (direct != node.end() && direct->is_object()) {
            return &(*direct);
        }
        for (json::const_iterator it = node.begin(); it != node.end(); ++it) {
            const json *found = findNamedObject(it.value(), name);
            if (found != NULL) {
                return found;
            }
        }
    } else if (node.is_array()) {
        for (json::const_iterator it = node.begin(); it != node.end(); ++it) {
            const json *found = findNamedObject(*it, name);
            if (found != NULL) {
                return found;
            }
        }
    }
    return NULL;
}

int integerValue(const json &node, const char *name, int fallback) {
    json::const_iterator value = node.find(name);
    if (value != node.end() && value->is_number_integer()) {
        return value->get<int>();
    }
    return fallback;
}

bool containsApiSuccess(const std::string &body, std::string &error) {
    json result;
    return firstResult(body, result, error);
}

} // namespace

struct ReolinkClient::Impl {
    std::string host;
    int httpPort;
    int channel;
    std::string username;
    std::string password;
    std::string token;
    CURL *curl;

    Impl(const std::string &hostValue, int portValue, int channelValue,
         const std::string &usernameValue, const std::string &passwordValue)
        : host(hostValue), httpPort(portValue), channel(channelValue),
          username(usernameValue), password(passwordValue), curl(NULL) {}
};

ReolinkCapabilities::ReolinkCapabilities()
    : ptz(false), ptzSpeed(false), zoom(false), focus(false),
      autoFocus(false), zoomMin(0), zoomMax(0), focusMin(0), focusMax(0),
      zoomPosition(0), focusPosition(0), autoFocusEnabled(false) {}

ReolinkClient::ReolinkClient(const std::string &host, int httpPort,
                             int channel, const std::string &username,
                             const std::string &password)
    : impl_(new Impl(host, httpPort, channel, username, password)) {
    std::call_once(curlInitFlag, initializeCurl);
    impl_->curl = curl_easy_init();
}

ReolinkClient::~ReolinkClient() {
    if (impl_ != NULL) {
        if (impl_->curl != NULL) {
            curl_easy_cleanup(impl_->curl);
        }
        delete impl_;
        impl_ = NULL;
    }
}

std::string ReolinkClient::redact(const std::string &message) {
    std::string redacted = message;

    std::string::size_type scheme = redacted.find("://");
    if (scheme != std::string::npos) {
        const std::string::size_type at = redacted.find('@', scheme + 3);
        const std::string::size_type slash = redacted.find('/', scheme + 3);
        if (at != std::string::npos &&
            (slash == std::string::npos || at < slash)) {
            redacted.replace(scheme + 3, at - (scheme + 3), "<redacted>");
        }
    }

    std::string::size_type token = 0;
    while ((token = redacted.find("token=", token)) != std::string::npos) {
        const std::string::size_type valueStart = token + 6;
        std::string::size_type valueEnd =
            redacted.find_first_of("&# \t\r\n", valueStart);
        if (valueEnd == std::string::npos) {
            valueEnd = redacted.size();
        }
        redacted.replace(valueStart, valueEnd - valueStart, "<redacted>");
        token = valueStart + 10;
    }
    return redacted;
}

bool ReolinkClient::post(const std::string &url, const std::string &body,
                         std::string &response, std::string &error) {
    if (impl_->curl == NULL) {
        error = "libcurl initialization failed";
        return false;
    }

    response.clear();
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_reset(impl_->curl);
    curl_easy_setopt(impl_->curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(impl_->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(impl_->curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(impl_->curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body.size()));
    curl_easy_setopt(impl_->curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(impl_->curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(impl_->curl, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(impl_->curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(impl_->curl, CURLOPT_WRITEFUNCTION, appendResponse);
    curl_easy_setopt(impl_->curl, CURLOPT_WRITEDATA, &response);

    const CURLcode result = curl_easy_perform(impl_->curl);
    long httpStatus = 0;
    curl_easy_getinfo(impl_->curl, CURLINFO_RESPONSE_CODE, &httpStatus);
    curl_slist_free_all(headers);

    if (result != CURLE_OK) {
        error = std::string("HTTP request failed: ") +
                curl_easy_strerror(result);
        return false;
    }
    if (httpStatus < 200 || httpStatus >= 300) {
        std::ostringstream stream;
        stream << "camera HTTP status " << httpStatus;
        error = stream.str();
        return false;
    }
    return true;
}

bool ReolinkClient::parseLoginResponse(const std::string &body,
                                        std::string &token,
                                        std::string &error) {
    json result;
    if (!firstResult(body, result, error)) {
        return false;
    }
    try {
        token = result.at("value").at("Token").at("name").get<std::string>();
        if (token.empty()) {
            error = "camera returned an empty session token";
            return false;
        }
        return true;
    } catch (const std::exception &) {
        error = "login response did not contain value.Token.name";
        return false;
    }
}

bool ReolinkClient::login(std::string &error) {
    const json requestBody = json::array(
        {{{"cmd", "Login"},
          {"action", 0},
          {"param",
           {{"User",
             {{"userName", impl_->username}, {"password", impl_->password}}}}}}});
    std::ostringstream url;
    url << "http://" << impl_->host << ":" << impl_->httpPort
        << "/cgi-bin/api.cgi?cmd=Login";

    std::string response;
    if (!post(url.str(), requestBody.dump(), response, error)) {
        error = redact(error);
        return false;
    }
    if (!parseLoginResponse(response, impl_->token, error)) {
        error = redact(error);
        return false;
    }
    return true;
}

bool ReolinkClient::request(const std::string &command, int action,
                            const std::string &parameterJson,
                            std::string &body, std::string &error,
                            bool retryAuthentication) {
    if (impl_->token.empty() && !login(error)) {
        return false;
    }

    json parameter;
    try {
        parameter = json::parse(parameterJson);
    } catch (const std::exception &exception) {
        error = std::string("invalid internal request JSON: ") +
                exception.what();
        return false;
    }

    const json requestBody =
        json::array({{{"cmd", command}, {"action", action},
                      {"param", parameter}}});
    std::ostringstream url;
    url << "http://" << impl_->host << ":" << impl_->httpPort
        << "/cgi-bin/api.cgi?cmd=" << command << "&token=" << impl_->token;

    if (!post(url.str(), requestBody.dump(), body, error)) {
        error = redact(error);
        return false;
    }

    std::string apiError;
    if (containsApiSuccess(body, apiError)) {
        return true;
    }

    if (retryAuthentication) {
        impl_->token.clear();
        if (login(error)) {
            return request(command, action, parameterJson, body, error, false);
        }
    }
    error = redact(apiError);
    return false;
}

bool ReolinkClient::parseCapabilitiesResponse(
    const std::string &body, ReolinkCapabilities &capabilities,
    std::string &error) {
    json result;
    if (!firstResult(body, result, error)) {
        return false;
    }
    const json &value = result["value"];
    capabilities.ptz = capabilityVersion(value, "ptzCtrl") > 0 ||
                       capabilityVersion(value, "ptzDirection") > 0;
    capabilities.ptzSpeed =
        capabilityVersion(value, "supportPtzSpeed") > 0 ||
        capabilityVersion(value, "ptzSpeed") > 0;
    capabilities.zoom = capabilityVersion(value, "supportZoom") > 0 ||
                        capabilityVersion(value, "zoomCfg") > 0 ||
                        capabilityVersion(value, "zoom") > 0;
    capabilities.focus = capabilityVersion(value, "supportFocus") > 0 ||
                         capabilityVersion(value, "focus") > 0;
    capabilities.autoFocus =
        capabilityVersion(value, "disableAutoFocus") > 0 ||
        capabilityVersion(value, "autoFocus") > 0;
    return true;
}

bool ReolinkClient::parseZoomFocusResponse(
    const std::string &body, ReolinkCapabilities &capabilities,
    std::string &error) {
    json result;
    if (!firstResult(body, result, error)) {
        return false;
    }
    const json *zoomFocus = findNamedObject(result["value"], "ZoomFocus");
    if (zoomFocus == NULL) {
        error = "GetZoomFocus response did not contain ZoomFocus";
        return false;
    }

    const json *zoom = findNamedObject(*zoomFocus, "zoom");
    const json *focus = findNamedObject(*zoomFocus, "focus");
    const json *rangeZoomFocus = NULL;
    if (result.contains("range")) {
        rangeZoomFocus = findNamedObject(result["range"], "ZoomFocus");
    }
    if (zoom != NULL) {
        capabilities.zoomPosition = integerValue(*zoom, "pos", 0);
        capabilities.zoomMin = integerValue(*zoom, "minPos", 0);
        capabilities.zoomMax = integerValue(*zoom, "maxPos", 0);
        if (rangeZoomFocus != NULL) {
            const json *rangeZoom =
                findNamedObject(*rangeZoomFocus, "zoom");
            const json *positionRange =
                rangeZoom != NULL ? findNamedObject(*rangeZoom, "pos") : NULL;
            if (positionRange != NULL) {
                capabilities.zoomMin =
                    integerValue(*positionRange, "min", capabilities.zoomMin);
                capabilities.zoomMax =
                    integerValue(*positionRange, "max", capabilities.zoomMax);
            }
        }
        capabilities.zoom = capabilities.zoomMax > capabilities.zoomMin;
    }
    if (focus != NULL) {
        capabilities.focusPosition = integerValue(*focus, "pos", 0);
        capabilities.focusMin = integerValue(*focus, "minPos", 0);
        capabilities.focusMax = integerValue(*focus, "maxPos", 0);
        if (rangeZoomFocus != NULL) {
            const json *rangeFocus =
                findNamedObject(*rangeZoomFocus, "focus");
            const json *positionRange =
                rangeFocus != NULL ? findNamedObject(*rangeFocus, "pos") : NULL;
            if (positionRange != NULL) {
                capabilities.focusMin = integerValue(
                    *positionRange, "min", capabilities.focusMin);
                capabilities.focusMax = integerValue(
                    *positionRange, "max", capabilities.focusMax);
            }
        }
        capabilities.focus = capabilities.focusMax > capabilities.focusMin;
    }
    if (!capabilities.zoom && !capabilities.focus) {
        error = "GetZoomFocus returned no usable zoom or focus ranges";
        return false;
    }
    return true;
}

bool ReolinkClient::parseAutoFocusResponse(const std::string &body,
                                            bool &enabled,
                                            std::string &error) {
    json result;
    if (!firstResult(body, result, error)) {
        return false;
    }
    const json *autoFocus = findNamedObject(result["value"], "AutoFocus");
    if (autoFocus == NULL) {
        error = "GetAutoFocus response did not contain AutoFocus";
        return false;
    }
    enabled = integerValue(*autoFocus, "disable", 1) == 0;
    return true;
}

bool ReolinkClient::getZoomFocus(ReolinkCapabilities &capabilities,
                                  std::string &error) {
    const json parameter = {{"channel", impl_->channel}};
    std::string response;
    if (!request("GetZoomFocus", 1, parameter.dump(), response, error)) {
        return false;
    }
    return parseZoomFocusResponse(response, capabilities, error);
}

bool ReolinkClient::getAutoFocus(bool &enabled, std::string &error) {
    const json parameter = {{"channel", impl_->channel}};
    std::string response;
    if (!request("GetAutoFocus", 0, parameter.dump(), response, error)) {
        return false;
    }
    return parseAutoFocusResponse(response, enabled, error);
}

bool ReolinkClient::discover(ReolinkCapabilities &capabilities,
                              std::string &error) {
    const json abilityParameter = {
        {"User", {{"userName", impl_->username}}}};
    std::string response;
    if (!request("GetAbility", 0, abilityParameter.dump(), response, error)) {
        return false;
    }
    if (!parseCapabilitiesResponse(response, capabilities, error)) {
        return false;
    }

    const json channelParameter = {{"channel", impl_->channel}};
    if (request("GetDevInfo", 0, channelParameter.dump(), response, error)) {
        json result;
        std::string parseError;
        if (firstResult(response, result, parseError)) {
            const json *device = findNamedObject(result["value"], "DevInfo");
            if (device != NULL) {
                capabilities.model = device->value("model", "");
                capabilities.firmware = device->value("firmVer", "");
            }
        }
    }

    std::string optionalError;
    if (!getZoomFocus(capabilities, optionalError)) {
        capabilities.zoom = false;
        capabilities.focus = false;
    }
    bool enabled = false;
    if (getAutoFocus(enabled, optionalError)) {
        capabilities.autoFocus = true;
        capabilities.autoFocusEnabled = enabled;
    } else {
        capabilities.autoFocus = false;
    }
    error.clear();
    return true;
}

bool ReolinkClient::ptz(const std::string &operation, int speed,
                         std::string &error) {
    json parameter = {{"channel", impl_->channel}, {"op", operation}};
    // Fixed-speed cameras such as the E1 Outdoor SE PoE acknowledge a
    // PtzCtrl request containing speed but do not move.  Their own web UI
    // omits the field.  A non-positive speed therefore means "not supported".
    if (speed > 0) {
        parameter["speed"] = speed;
    }
    std::string response;
    return request("PtzCtrl", 0, parameter.dump(), response, error);
}

bool ReolinkClient::setZoom(int position, std::string &error) {
    const json parameter = {
        {"ZoomFocus",
         {{"channel", impl_->channel}, {"pos", position}, {"op", "ZoomPos"}}}};
    std::string response;
    return request("StartZoomFocus", 0, parameter.dump(), response, error);
}

bool ReolinkClient::setFocus(int position, std::string &error) {
    const json parameter = {
        {"ZoomFocus",
         {{"channel", impl_->channel}, {"pos", position}, {"op", "FocusPos"}}}};
    std::string response;
    return request("StartZoomFocus", 0, parameter.dump(), response, error);
}

bool ReolinkClient::setAutoFocus(bool enabled, std::string &error) {
    const json parameter = {
        {"AutoFocus", {{"channel", impl_->channel},
                       {"disable", enabled ? 0 : 1}}}};
    std::string response;
    return request("SetAutoFocus", 0, parameter.dump(), response, error);
}
