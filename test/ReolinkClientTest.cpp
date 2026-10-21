#include "ReolinkClient.h"

#include <epicsUnitTest.h>
#include <testMain.h>

#include <string>

MAIN(ReolinkClientTest) {
    testPlan(17);

    std::string token;
    std::string error;
    testOk(ReolinkClient::parseLoginResponse(
               "[{\"cmd\":\"Login\",\"code\":0,\"value\":{\"Token\":"
               "{\"name\":\"abc123\",\"leaseTime\":3600}}}]",
               token, error),
           "parse a valid login response");
    testOk(token == "abc123", "extract the login token");

    token.clear();
    testOk(!ReolinkClient::parseLoginResponse(
               "[{\"cmd\":\"Login\",\"code\":1,\"error\":"
               "{\"rspCode\":-8}}]",
               token, error),
           "reject a failed login response");
    testOk(error.find("code 1") != std::string::npos,
           "report the API error code");

    const std::string ability =
        "[{\"cmd\":\"GetAbility\",\"code\":0,\"value\":{\"Ability\":"
        "{\"abilityChn\":[{\"ptzCtrl\":{\"ver\":2},"
        "\"supportPtzSpeed\":{\"ver\":1},"
        "\"supportZoom\":{\"ver\":1},"
        "\"supportFocus\":{\"ver\":1},"
        "\"disableAutoFocus\":{\"ver\":1}}]}}}]";
    ReolinkCapabilities capabilities;
    testOk(ReolinkClient::parseCapabilitiesResponse(ability, capabilities,
                                                     error),
           "parse capabilities");
    testOk(capabilities.ptz, "detect PTZ support");
    testOk(capabilities.ptzSpeed, "detect PTZ speed support");
    testOk(capabilities.zoom && capabilities.focus,
           "detect zoom and focus support");
    testOk(capabilities.autoFocus, "detect autofocus support");

    const std::string zoomFocus =
        "[{\"cmd\":\"GetZoomFocus\",\"code\":0,\"value\":"
        "{\"ZoomFocus\":{\"channel\":0,\"zoom\":{\"pos\":26},"
        "\"focus\":{\"pos\":999}}},\"range\":{\"ZoomFocus\":"
        "{\"channel\":0,\"zoom\":{\"pos\":{\"min\":0,"
        "\"max\":41}},\"focus\":{\"pos\":{\"min\":0,"
        "\"max\":1104}}}}}]";
    testOk(ReolinkClient::parseZoomFocusResponse(zoomFocus, capabilities,
                                                 error),
           "parse zoom/focus status");
    testOk(capabilities.zoomPosition == 26 && capabilities.zoomMax == 41,
           "extract zoom position and range");
    testOk(capabilities.focusPosition == 999 &&
               capabilities.focusMax == 1104,
           "extract focus position and range");

    bool autoFocus = false;
    testOk(ReolinkClient::parseAutoFocusResponse(
               "[{\"cmd\":\"GetAutoFocus\",\"code\":0,\"value\":"
               "{\"AutoFocus\":{\"channel\":0,\"disable\":0}}}]",
               autoFocus, error),
           "parse autofocus status");
    testOk(autoFocus, "invert disable=0 to enabled");
    testOk(ReolinkClient::parseAutoFocusResponse(
               "[{\"cmd\":\"GetAutoFocus\",\"code\":0,\"value\":"
               "{\"AutoFocus\":{\"channel\":0,\"disable\":1}}}]",
               autoFocus, error) &&
               !autoFocus,
           "invert disable=1 to disabled");

    const std::string redacted = ReolinkClient::redact(
        "open rtsp://admin:password@camera/live?token=secret&cmd=x");
    testOk(redacted.find("password") == std::string::npos,
           "redact URL credentials");
    testOk(redacted.find("secret") == std::string::npos,
           "redact API tokens");

    return testDone();
}
