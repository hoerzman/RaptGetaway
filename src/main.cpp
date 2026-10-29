#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_https_server.h>
#include <esp_system.h>
#include <freertos/semphr.h>
#include <atomic>
#include <cstring>
#include <ctime>

#include "config.h"
#include "rapt_parser.h"
#include "web_security.h"

extern const uint8_t brewfatherCaBundleStart[]
    asm("_binary_certs_x509_crt_bundle_bin_start");

namespace
{
constexpr size_t MAX_PILLS = 12;
constexpr size_t MAX_DISCOVERED_PILLS = 20;
constexpr uint16_t DEFAULT_BREWFATHER_INTERVAL_MINUTES = 30;
constexpr uint16_t BREWFATHER_INTERVAL_OPTIONS[] = {15, 30, 60, 120, 240};
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000UL;
constexpr uint32_t BLE_SCAN_RETRY_INTERVAL_MS = 5000UL;
constexpr uint32_t SAMPLE_MAX_AGE_MS = 20UL * 60UL * 1000UL;
constexpr uint32_t DISCOVERY_MAX_AGE_MS = 60000UL;
constexpr time_t TLS_VALIDATION_MIN_EPOCH = 1735689600;

struct PillConfiguration
{
    char address[18];
    char name[48];
    char streamUrl[256];
    uint16_t intervalMinutes;
};

struct PillRuntime
{
    char address[18];
    RaptSample sample;
    uint32_t lastSeen;
    uint32_t sampleReceivedAt;
    uint32_t lastPostAttempt;
    uint32_t lastBrewfatherResultAt;
    uint32_t lastBrewfatherResultEpoch;
    int bleRssi;
    int lastBrewfatherStatus;
    bool hasSample;
    bool hasAttemptedPost;
    bool hasBleRssi;
    bool hasBrewfatherResult;
    bool lastBrewfatherSucceeded;
};

struct BrewfatherResult
{
    int status;
    String detail;
};

portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
PillConfiguration pillConfigurations[MAX_PILLS] = {};
PillRuntime discoveredPills[MAX_DISCOVERED_PILLS] = {};
size_t pillConfigurationCount = 0;
WebServer server(80);
httpd_handle_t httpsServer = nullptr;
httpd_req_t* activeHttpsRequest = nullptr;
SemaphoreHandle_t webRequestMutex = nullptr;
String activeHttpsBody;
bool activeHttpsBodyRead = false;
String pendingResponseCookie;
struct SecureResponseHeader
{
    String name;
    String value;
};
SecureResponseHeader secureResponseHeaders[4];
size_t secureResponseHeaderCount = 0;
DNSServer dnsServer;
Preferences preferences;
NimBLEScan* bleScan = nullptr;
uint32_t lastWifiAttempt = 0;
uint32_t lastBleScanAttempt = 0;
uint32_t lastSetupApAttempt = 0;
uint32_t lastMdnsAttempt = 0;
uint32_t setupApStartedAt = 0;
uint32_t setupApCloseNotBefore = 0;
bool setupApActive = false;
bool mdnsStarted = false;
bool httpsAvailable = false;
bool factoryResetHandled = false;
std::atomic<bool> factoryResetRequested{false};
uint32_t factoryResetPressedAt = 0;
uint8_t failedAdminLogins = 0;
uint32_t adminLoginBlockedUntil = 0;
bool tlsTimeSyncStarted = false;
uint32_t lastTlsTimeSyncLog = 0;

bool isSupportedBrewfatherInterval(int minutes)
{
    for (uint16_t option : BREWFATHER_INTERVAL_OPTIONS)
    {
        if (minutes == option)
        {
            return true;
        }
    }
    return false;
}

void copyString(char* destination, size_t capacity, const String& source)
{
    if (capacity == 0)
    {
        return;
    }

    source.substring(0, capacity - 1).toCharArray(destination, capacity);
}

bool startSetupAccessPoint(bool keepStationEnabled)
{
    const wifi_mode_t targetMode =
        keepStationEnabled ? WIFI_AP_STA : WIFI_AP;
    if (WiFi.getMode() != targetMode && !WiFi.mode(targetMode))
    {
        Serial.println("Setup-WLAN: AP+STA-Modus konnte nicht gestartet werden");
        return false;
    }

    const IPAddress setupIp(192, 168, 4, 1);
    const IPAddress setupSubnet(255, 255, 255, 0);
    if (!WiFi.softAPConfig(setupIp, setupIp, setupSubnet))
    {
        Serial.println("Setup-WLAN: IP-Konfiguration fehlgeschlagen");
        return false;
    }

    if (!WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD, 1, false, 4))
    {
        Serial.println("Setup-WLAN: Access Point konnte nicht gestartet werden");
        return false;
    }

    setupApActive = true;
    setupApStartedAt = millis();
    setupApCloseNotBefore = 0;
    dnsServer.start(53, "*", WiFi.softAPIP());
    Serial.printf("Setup-WLAN aktiviert: %s (%s)\n",
                  WiFi.softAPSSID().c_str(),
                  WiFi.softAPIP().toString().c_str());
    return true;
}

void stopSetupAccessPoint()
{
    if (!setupApActive)
    {
        return;
    }

    dnsServer.stop();
    if (!WiFi.mode(WIFI_STA))
    {
        Serial.println("Setup-WLAN: Wechsel in den WLAN-Client-Modus fehlgeschlagen");
        dnsServer.start(53, "*", WiFi.softAPIP());
        return;
    }
    setupApActive = false;

    Serial.println("Setup-WLAN ausgeschaltet; Webseite jetzt über das Heimnetz erreichbar");
}

int findConfiguredPill(const char* address)
{
    for (size_t i = 0; i < pillConfigurationCount; ++i)
    {
        if (strcmp(pillConfigurations[i].address, address) == 0)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int findDiscoveredPill(const char* address)
{
    for (size_t i = 0; i < MAX_DISCOVERED_PILLS; ++i)
    {
        if (discoveredPills[i].address[0] != '\0' &&
            strcmp(discoveredPills[i].address, address) == 0)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool savePillConfigurations()
{
    JsonDocument document;
    JsonArray pills = document.to<JsonArray>();
    for (size_t i = 0; i < pillConfigurationCount; ++i)
    {
        JsonObject pill = pills.add<JsonObject>();
        pill["address"] = pillConfigurations[i].address;
        pill["name"] = pillConfigurations[i].name;
        pill["url"] = pillConfigurations[i].streamUrl;
        pill["intervalMinutes"] = pillConfigurations[i].intervalMinutes;
    }

    String serialized;
    serializeJson(document, serialized);
    return preferences.putString("pills", serialized) != 0;
}

void loadSettings()
{
    preferences.begin("raptgateway", false);

    const String serialized = preferences.isKey("pills")
                                  ? preferences.getString("pills")
                                  : "[]";
    JsonDocument document;
    const DeserializationError error = deserializeJson(document, serialized);
    if (error)
    {
        Serial.printf("Pill-Konfigurationen konnten nicht gelesen werden: %s\n",
                      error.c_str());
        return;
    }

    for (JsonObjectConst pill : document.as<JsonArrayConst>())
    {
        if (pillConfigurationCount >= MAX_PILLS)
        {
            Serial.println("Maximale Anzahl gespeicherter Pills erreicht");
            break;
        }

        PillConfiguration& configuration =
            pillConfigurations[pillConfigurationCount];
        copyString(configuration.address, sizeof(configuration.address),
                   String(pill["address"] | ""));
        copyString(configuration.name, sizeof(configuration.name),
                   String(pill["name"] | ""));
        copyString(configuration.streamUrl, sizeof(configuration.streamUrl),
                   String(pill["url"] | ""));
        const int intervalMinutes =
            pill["intervalMinutes"] | DEFAULT_BREWFATHER_INTERVAL_MINUTES;
        if (!isSupportedBrewfatherInterval(intervalMinutes))
        {
            Serial.printf("Ungültiges Brewfather-Intervall für Pill %s; "
                          "verwende Standardwert %u Minuten\n",
                          configuration.address,
                          DEFAULT_BREWFATHER_INTERVAL_MINUTES);
            configuration.intervalMinutes =
                DEFAULT_BREWFATHER_INTERVAL_MINUTES;
        }
        else
        {
            configuration.intervalMinutes =
                static_cast<uint16_t>(intervalMinutes);
        }

        if (configuration.address[0] != '\0' &&
            configuration.name[0] != '\0' &&
            configuration.streamUrl[0] != '\0')
        {
            ++pillConfigurationCount;
        }
    }
}

bool readSavedWifiCredentials(String& ssid,
                              String& password,
                              bool* adminPasswordConfigured = nullptr)
{
    const bool locked =
        webRequestMutex != nullptr &&
        xSemaphoreTake(webRequestMutex, portMAX_DELAY) == pdTRUE;
    if (webRequestMutex != nullptr && !locked)
    {
        return false;
    }

    ssid = preferences.isKey("wifiSsid")
               ? preferences.getString("wifiSsid")
               : "";
    password = preferences.isKey("wifiPass")
                   ? preferences.getString("wifiPass")
                   : "";
    if (adminPasswordConfigured != nullptr)
    {
        *adminPasswordConfigured = hasAdminPassword(preferences);
    }
    if (locked)
    {
        xSemaphoreGive(webRequestMutex);
    }
    return !ssid.isEmpty();
}

BrewfatherResult sendToBrewfather(const PillConfiguration& configuration,
                                  const PillRuntime& runtime)
{
    if (time(nullptr) < TLS_VALIDATION_MIN_EPOCH)
    {
        Serial.println("Brewfather-Senden wartet auf eine gültige Systemzeit");
        return {-1, "Systemzeit noch nicht synchronisiert"};
    }

    JsonDocument json;
    json["name"] = configuration.name;
    json["temp"] = runtime.sample.temperature;
    json["temp_unit"] = "C";
    json["gravity"] = runtime.sample.gravity;
    json["gravity_unit"] = "G";
    json["battery"] = runtime.sample.batteryVolts;
    json["device_source"] = "RAPT Pill";
    json["report_source"] = "RaptGetaway";

    String body;
    serializeJson(json, body);

    WiFiClientSecure client;
    client.setCACertBundle(brewfatherCaBundleStart);

    HTTPClient http;
    if (!http.begin(client, configuration.streamUrl))
    {
        Serial.printf("%s: HTTPS-Verbindung konnte nicht gestartet werden\n",
                      configuration.name);
        return {-1, "HTTPS-Verbindung konnte nicht gestartet werden"};
    }

    http.addHeader("Content-Type", "application/json");
    const int status = http.POST(body);
    String detail;
    if (status >= 200 && status < 300)
    {
        Serial.printf("%s: Brewfather-Messwerte gesendet (HTTP %d)\n",
                      configuration.name, status);
    }
    else if (status > 0)
    {
        detail = http.getString();
        Serial.printf("%s: Brewfather HTTP-Fehler %d: %s\n",
                      configuration.name, status, detail.c_str());
    }
    else
    {
        detail = http.errorToString(status);
        Serial.printf("%s: Brewfather-Senden fehlgeschlagen: %s\n",
                      configuration.name, detail.c_str());
    }
    http.end();
    return {status, detail};
}

void recordBrewfatherResult(const char* address, const BrewfatherResult& result)
{
    const time_t resultEpoch = time(nullptr);
    const uint32_t resultTimestamp =
        resultEpoch >= TLS_VALIDATION_MIN_EPOCH
            ? static_cast<uint32_t>(resultEpoch)
            : 0;
    portENTER_CRITICAL(&stateMux);
    const int runtimeIndex = findDiscoveredPill(address);
    if (runtimeIndex >= 0)
    {
        PillRuntime& runtime = discoveredPills[runtimeIndex];
        runtime.hasBrewfatherResult = true;
        runtime.lastBrewfatherSucceeded =
            result.status >= 200 && result.status < 300;
        runtime.lastBrewfatherStatus = result.status;
        runtime.lastBrewfatherResultAt = millis();
        runtime.lastBrewfatherResultEpoch = resultTimestamp;
    }
    portEXIT_CRITICAL(&stateMux);
}

String decodeQueryValue(const char* input)
{
    String output;
    for (size_t i = 0; input[i] != '\0'; ++i)
    {
        if (input[i] == '+' )
        {
            output += ' ';
        }
        else if (input[i] == '%' && input[i + 1] != '\0' &&
                 input[i + 2] != '\0')
        {
            char hex[3] = {input[i + 1], input[i + 2], '\0'};
            output += static_cast<char>(strtoul(hex, nullptr, 16));
            i += 2;
        }
        else
        {
            output += input[i];
        }
    }
    return output;
}

String requestArg(const String& name)
{
    if (activeHttpsRequest == nullptr)
    {
        return server.arg(name);
    }
    if (name == "plain")
    {
        if (!activeHttpsBodyRead)
        {
            activeHttpsBodyRead = true;
            const size_t contentLength = activeHttpsRequest->content_len;
            if (contentLength > 2048)
            {
                return "";
            }
            char* body = static_cast<char*>(malloc(contentLength + 1));
            if (body == nullptr)
            {
                return "";
            }
            size_t received = 0;
            while (received < contentLength)
            {
                const int count = httpd_req_recv(
                    activeHttpsRequest, body + received,
                    contentLength - received);
                if (count <= 0)
                {
                    free(body);
                    return "";
                }
                received += static_cast<size_t>(count);
            }
            body[contentLength] = '\0';
            activeHttpsBody = body;
            free(body);
        }
        return activeHttpsBody;
    }

    char query[HTTPD_MAX_URI_LEN + 1] = {};
    char value[HTTPD_MAX_URI_LEN + 1] = {};
    if (httpd_req_get_url_query_str(
            activeHttpsRequest, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(
            query, name.c_str(), value, sizeof(value)) != ESP_OK)
    {
        return "";
    }
    return decodeQueryValue(value);
}

String requestHeader(const String& name)
{
    if (activeHttpsRequest == nullptr)
    {
        if (name == "Host")
        {
            return server.hostHeader();
        }
        return server.header(name);
    }
    const size_t length =
        httpd_req_get_hdr_value_len(activeHttpsRequest, name.c_str());
    if (length == 0 || length >= HTTPD_MAX_REQ_HDR_LEN)
    {
        return "";
    }
    char value[HTTPD_MAX_REQ_HDR_LEN + 1] = {};
    if (httpd_req_get_hdr_value_str(
            activeHttpsRequest, name.c_str(), value, sizeof(value)) != ESP_OK)
    {
        return "";
    }
    return value;
}

bool isSetupApClient()
{
    if (!setupApActive || activeHttpsRequest != nullptr)
    {
        return false;
    }
    const WiFiClient client = server.client();
    const IPAddress localAddress = client.localIP();
    const IPAddress remoteAddress = client.remoteIP();
    return localAddress == WiFi.softAPIP() &&
           remoteAddress[0] == 192 && remoteAddress[1] == 168 &&
           remoteAddress[2] == 4;
}

bool isHttpsRequest()
{
    return activeHttpsRequest != nullptr;
}

void setResponseHeader(const String& name, const String& value)
{
    if (activeHttpsRequest != nullptr)
    {
        size_t headerIndex = 0;
        while (headerIndex < secureResponseHeaderCount &&
               secureResponseHeaders[headerIndex].name != name)
        {
            ++headerIndex;
        }
        if (headerIndex == secureResponseHeaderCount)
        {
            if (secureResponseHeaderCount >=
                sizeof(secureResponseHeaders) /
                    sizeof(secureResponseHeaders[0]))
            {
                Serial.println("HTTPS response header limit exceeded");
                return;
            }
            ++secureResponseHeaderCount;
        }
        secureResponseHeaders[headerIndex].name = name;
        secureResponseHeaders[headerIndex].value = value;
        if (httpd_resp_set_hdr(
                activeHttpsRequest,
                secureResponseHeaders[headerIndex].name.c_str(),
                secureResponseHeaders[headerIndex].value.c_str()) != ESP_OK)
        {
            Serial.printf("HTTPS response header could not be set: %s\n",
                          name.c_str());
        }
    }
    else
    {
        server.sendHeader(name, value);
    }
}

const char* responseStatus(int statusCode)
{
    switch (statusCode)
    {
    case 200: return "200 OK";
    case 302: return "302 Found";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 429: return "429 Too Many Requests";
    case 500: return "500 Internal Server Error";
    case 502: return "502 Bad Gateway";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
    }
}

void sendResponse(int statusCode, const char* contentType, const String& body)
{
    if (activeHttpsRequest == nullptr)
    {
        setResponseHeader("Cache-Control", "no-store");
        setResponseHeader("X-Content-Type-Options", "nosniff");
        setResponseHeader("Referrer-Policy", "no-referrer");
        if (!pendingResponseCookie.isEmpty())
        {
            setResponseHeader("Set-Cookie", pendingResponseCookie);
        }
        pendingResponseCookie = "";
        server.send(statusCode, contentType, body);
        return;
    }
    httpd_resp_set_status(activeHttpsRequest, responseStatus(statusCode));
    httpd_resp_set_type(activeHttpsRequest, contentType);
    setResponseHeader("Cache-Control", "no-store");
    setResponseHeader("X-Content-Type-Options", "nosniff");
    setResponseHeader("Referrer-Policy", "no-referrer");
    if (!pendingResponseCookie.isEmpty())
    {
        setResponseHeader("Set-Cookie", pendingResponseCookie);
    }
    pendingResponseCookie = "";
    httpd_resp_send(
        activeHttpsRequest, body.c_str(), static_cast<ssize_t>(body.length()));
}

String requestCookie(const char* cookieName)
{
    const String cookies = requestHeader("Cookie");
    const String prefix = String(cookieName) + "=";
    int start = cookies.indexOf(prefix);
    while (start >= 0)
    {
        if (start == 0 || cookies[start - 1] == ';' ||
            cookies[start - 1] == ' ')
        {
            const int valueStart = start + prefix.length();
            int valueEnd = cookies.indexOf(';', valueStart);
            if (valueEnd < 0)
            {
                valueEnd = cookies.length();
            }
            return cookies.substring(valueStart, valueEnd);
        }
        start = cookies.indexOf(prefix, start + 1);
    }
    return "";
}

bool currentRequestAuthenticated()
{
    return isValidAdminSession(requestCookie("rg_session"));
}

bool requestOriginIsSame()
{
    String host = requestHeader("Host");
    const String origin = requestHeader("Origin");
    if (host.isEmpty() || origin.isEmpty())
    {
        return false;
    }
    if (isHttpsRequest() && host.endsWith(":443"))
    {
        host.remove(host.length() - 4);
    }
    else if (!isHttpsRequest() && host.endsWith(":80"))
    {
        host.remove(host.length() - 3);
    }
    const String expected =
        String(isHttpsRequest() ? "https://" : "http://") + host;
    return origin == expected;
}

void setAdminSessionCookie(const String& token)
{
    const String cookie = String("rg_session=") + token +
        "; Path=/; HttpOnly; SameSite=Strict; Max-Age=1800" +
        (isHttpsRequest() ? "; Secure" : "");
    if (isHttpsRequest())
    {
        pendingResponseCookie = cookie;
    }
    else
    {
        setResponseHeader("Set-Cookie", cookie);
    }
}

void clearAdminSessionCookie()
{
    const String cookie =
        String("rg_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0") +
        (isHttpsRequest() ? "; Secure" : "");
    if (isHttpsRequest())
    {
        pendingResponseCookie = cookie;
    }
    else
    {
        setResponseHeader("Set-Cookie", cookie);
    }
}

void sendJson(int statusCode, JsonDocument& document)
{
    String body;
    serializeJson(document, body);
    sendResponse(statusCode, "application/json; charset=utf-8", body);
}

void handleAuthStatus()
{
    JsonDocument document;
    document["adminConfigured"] = hasAdminPassword(preferences);
    document["authenticated"] = currentRequestAuthenticated();
    document["setupAccess"] = isSetupApClient() && !isHttpsRequest();
    document["wifiConnected"] = WiFi.status() == WL_CONNECTED;
    document["certificateFingerprint"] = webServerCertificateFingerprint();
    document["hostname"] = webServerHostname();
    sendJson(200, document);
}

void handleAdminPasswordSetup()
{
    if (!isSetupApClient() || isHttpsRequest() ||
        hasAdminPassword(preferences))
    {
        sendResponse(403, "application/json",
                     "{\"error\":\"Admin password setup is only available once from the setup access point\"}");
        return;
    }

    JsonDocument document;
    if (deserializeJson(document, requestArg("plain")) ||
        !document["password"].is<const char*>())
    {
        sendResponse(400, "application/json",
                     "{\"error\":\"Admin password is required\"}");
        return;
    }
    const String password = document["password"].as<String>();
    if (password.length() < 12 || password.length() > 64)
    {
        sendResponse(400, "application/json",
                     "{\"error\":\"Admin password must be 12 to 64 characters\"}");
        return;
    }
    if (!setAdminPassword(preferences, password))
    {
        sendResponse(500, "application/json",
                     "{\"error\":\"Admin password could not be saved\"}");
        return;
    }
    if (WiFi.status() == WL_CONNECTED)
    {
        setupApCloseNotBefore = millis() + 20000UL;
    }

    const String token = createAdminSession();
    setAdminSessionCookie(token);
    JsonDocument response;
    response["configured"] = true;
    response["setupApWillClose"] =
        setupApActive && WiFi.status() == WL_CONNECTED;
    response["hostname"] = webServerHostname();
    sendJson(200, response);
}

void handleAdminLogin()
{
    if (!hasAdminPassword(preferences))
    {
        sendResponse(403, "application/json",
                     "{\"error\":\"Set the admin password through the setup access point first\"}");
        return;
    }
    const uint32_t now = millis();
    if (static_cast<int32_t>(adminLoginBlockedUntil - now) > 0)
    {
        sendResponse(429, "application/json",
                     "{\"error\":\"Too many failed logins; wait five minutes and try again\"}");
        return;
    }

    JsonDocument document;
    if (deserializeJson(document, requestArg("plain")) ||
        !document["password"].is<const char*>())
    {
        sendResponse(400, "application/json",
                     "{\"error\":\"Admin password is required\"}");
        return;
    }
    if (!verifyAdminPassword(preferences, document["password"].as<String>()))
    {
        if (++failedAdminLogins >= 5)
        {
            failedAdminLogins = 0;
            adminLoginBlockedUntil = now + 5UL * 60UL * 1000UL;
        }
        sendResponse(401, "application/json",
                     "{\"error\":\"Admin password is incorrect\"}");
        return;
    }

    failedAdminLogins = 0;
    adminLoginBlockedUntil = 0;
    const String token = createAdminSession();
    setAdminSessionCookie(token);
    JsonDocument response;
    response["authenticated"] = true;
    sendJson(200, response);
}

void handleAdminLogout()
{
    clearAdminSession();
    clearAdminSessionCookie();
    JsonDocument response;
    response["authenticated"] = false;
    sendJson(200, response);
}

void handleFactoryReset()
{
    if (!preferences.clear())
    {
        sendResponse(500, "application/json",
                     "{\"error\":\"Factory reset failed: settings could not be cleared\"}");
        return;
    }

    clearAdminSession();
    clearAdminSessionCookie();
    factoryResetRequested.store(true, std::memory_order_release);
    sendResponse(200, "application/json", "{\"resetting\":true}");
}

void handleStatus()
{
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    JsonDocument document;
    document["wifiConnected"] = wifiConnected;
    document["wifiSsid"] = wifiConnected ? WiFi.SSID() : "";
    document["wifiRssi"] = wifiConnected ? WiFi.RSSI() : 0;
    document["ip"] = wifiConnected
                         ? WiFi.localIP().toString()
                         : WiFi.softAPIP().toString();
    document["setupSsid"] = SETUP_AP_SSID;
    document["setupApActive"] = setupApActive;
    document["setupApActiveForSeconds"] =
        setupApActive ? (millis() - setupApStartedAt) / 1000UL : 0;
    document["wifiConfigured"] = preferences.isKey("wifiSsid") &&
                                 !preferences.getString("wifiSsid").isEmpty();
    document["pillCount"] = pillConfigurationCount;
    sendJson(200, document);
}

void handleWifiScan()
{
    const wifi_mode_t originalMode = WiFi.getMode();
    const bool temporarilyEnabledStation =
        originalMode == WIFI_AP && WiFi.mode(WIFI_AP_STA);
    if (originalMode == WIFI_AP && !temporarilyEnabledStation)
    {
        sendResponse(500, "application/json",
                    "{\"error\":\"WLAN-Suche konnte nicht gestartet werden\"}");
        return;
    }

    const int networkCount = WiFi.scanNetworks(false, true);
    if (networkCount < 0)
    {
        Serial.printf("WLAN-Suche fehlgeschlagen (Code %d)\n", networkCount);
        WiFi.scanDelete();
        if (temporarilyEnabledStation)
        {
            WiFi.mode(originalMode);
        }
        sendResponse(500, "application/json",
                    String("{\"error\":\"WLAN-Suche fehlgeschlagen (Code ") +
                        networkCount + ")\"}");
        return;
    }

    JsonDocument document;
    JsonArray networks = document.to<JsonArray>();
    for (int i = 0; i < networkCount; ++i)
    {
        const String ssid = WiFi.SSID(i);
        if (ssid.isEmpty())
        {
            continue;
        }

        JsonObject matchingNetwork;
        for (JsonObject network : networks)
        {
            if (network["ssid"].as<String>() == ssid)
            {
                matchingNetwork = network;
                break;
            }
        }

        const int rssi = WiFi.RSSI(i);
        if (matchingNetwork.isNull())
        {
            matchingNetwork = networks.add<JsonObject>();
            matchingNetwork["ssid"] = ssid;
            matchingNetwork["rssi"] = rssi;
            matchingNetwork["secure"] =
                WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        }
        else if (rssi > matchingNetwork["rssi"].as<int>())
        {
            matchingNetwork["rssi"] = rssi;
            matchingNetwork["secure"] =
                WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        }
    }
    WiFi.scanDelete();
    if (temporarilyEnabledStation && !WiFi.mode(originalMode))
    {
        Serial.println("WLAN-Suche: vorheriger WLAN-Modus konnte nicht wiederhergestellt werden");
    }
    sendJson(200, document);
}

void handleWifiSave()
{
    JsonDocument document;
    if (deserializeJson(document, requestArg("plain")) ||
        !document["ssid"].is<const char*>() ||
        !document["password"].is<const char*>())
    {
        sendResponse(400, "application/json",
                    "{\"error\":\"SSID oder Passwort fehlt\"}");
        return;
    }

    const String ssid = document["ssid"].as<String>();
    const String password = document["password"].as<String>();
    if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63)
    {
        sendResponse(400, "application/json",
                    "{\"error\":\"SSID oder Passwort hat eine ungültige Länge\"}");
        return;
    }

    if (preferences.putString("wifiSsid", ssid) == 0 ||
        preferences.putString("wifiPass", password) == 0)
    {
        sendResponse(500, "application/json",
                    "{\"error\":\"WLAN-Zugangsdaten konnten nicht gespeichert werden\"}");
        return;
    }
    setupApCloseNotBefore = millis() + 20000UL;
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    JsonDocument response;
    response["saved"] = true;
    response["message"] =
        "WLAN-Zugangsdaten gespeichert; Verbindung wird aufgebaut. "
        "Das Setup-WLAN wird nach erfolgreicher Verbindung ausgeschaltet.";
    sendJson(200, response);
    Serial.printf("WLAN-Verbindung zu '%s' wird aufgebaut\n", ssid.c_str());
}

void handlePillsGet()
{
    JsonDocument document;
    JsonArray pills = document.to<JsonArray>();
    const uint32_t now = millis();
    PillConfiguration configurations[MAX_PILLS] = {};
    PillRuntime runtimes[MAX_DISCOVERED_PILLS] = {};

    portENTER_CRITICAL(&stateMux);
    memcpy(configurations, pillConfigurations, sizeof(configurations));
    memcpy(runtimes, discoveredPills, sizeof(runtimes));
    portEXIT_CRITICAL(&stateMux);

    for (size_t i = 0; i < pillConfigurationCount; ++i)
    {
        const PillConfiguration& configuration = configurations[i];
        int runtimeIndex = -1;
        for (size_t j = 0; j < MAX_DISCOVERED_PILLS; ++j)
        {
            if (runtimes[j].address[0] != '\0' &&
                strcmp(runtimes[j].address, configuration.address) == 0)
            {
                runtimeIndex = static_cast<int>(j);
                break;
            }
        }
        const PillRuntime runtime =
            runtimeIndex >= 0 ? runtimes[runtimeIndex] : PillRuntime{};

        JsonObject pill = pills.add<JsonObject>();
        pill["address"] = configuration.address;
        pill["name"] = configuration.name;
        pill["configured"] = true;
        pill["urlConfigured"] = configuration.streamUrl[0] != '\0';
        pill["intervalMinutes"] = configuration.intervalMinutes;
        const bool hasRecentBleRssi =
            runtime.hasBleRssi && runtime.address[0] != '\0' &&
            static_cast<uint32_t>(now - runtime.lastSeen) <=
                DISCOVERY_MAX_AGE_MS;
        pill["hasBleRssi"] = hasRecentBleRssi;
        if (hasRecentBleRssi)
        {
            pill["bleRssi"] = runtime.bleRssi;
        }
        pill["online"] = runtime.address[0] != '\0' &&
                         static_cast<uint32_t>(now - runtime.lastSeen) <=
                             DISCOVERY_MAX_AGE_MS;
        if (runtime.address[0] != '\0')
        {
            pill["lastSeenAgeSeconds"] =
                static_cast<uint32_t>(now - runtime.lastSeen) / 1000UL;
        }
        pill["hasBrewfatherResult"] = runtime.hasBrewfatherResult;
        if (runtime.hasBrewfatherResult)
        {
            pill["lastBrewfatherSucceeded"] = runtime.lastBrewfatherSucceeded;
            pill["lastBrewfatherStatus"] = runtime.lastBrewfatherStatus;
            pill["lastBrewfatherResultAgeSeconds"] =
                static_cast<uint32_t>(now - runtime.lastBrewfatherResultAt) /
                1000UL;
            if (runtime.lastBrewfatherResultEpoch != 0)
            {
                pill["lastBrewfatherResultEpoch"] =
                    runtime.lastBrewfatherResultEpoch;
            }
        }
        if (runtime.hasSample)
        {
            pill["lastMeasurementAgeSeconds"] =
                static_cast<uint32_t>(now - runtime.sampleReceivedAt) /
                1000UL;
            pill["temperature"] = runtime.sample.temperature;
            pill["gravity"] = runtime.sample.gravity;
            pill["batteryPercent"] = runtime.sample.batteryPercent;
            pill["batteryVolts"] = runtime.sample.batteryVolts;
        }
    }

    for (size_t i = 0; i < MAX_DISCOVERED_PILLS; ++i)
    {
        const PillRuntime& runtime = discoveredPills[i];
        if (runtime.address[0] == '\0' ||
            findConfiguredPill(runtime.address) >= 0 ||
            static_cast<uint32_t>(now - runtime.lastSeen) > DISCOVERY_MAX_AGE_MS)
        {
            continue;
        }

        JsonObject pill = pills.add<JsonObject>();
        pill["address"] = runtime.address;
        pill["configured"] = false;
        pill["online"] = true;
        const bool hasRecentBleRssi =
            runtime.hasBleRssi &&
            static_cast<uint32_t>(now - runtime.lastSeen) <=
                DISCOVERY_MAX_AGE_MS;
        pill["hasBleRssi"] = hasRecentBleRssi;
        if (hasRecentBleRssi)
        {
            pill["bleRssi"] = runtime.bleRssi;
        }
        if (runtime.hasSample)
        {
            pill["lastMeasurementAgeSeconds"] =
                static_cast<uint32_t>(now - runtime.sampleReceivedAt) /
                1000UL;
            pill["temperature"] = runtime.sample.temperature;
            pill["gravity"] = runtime.sample.gravity;
            pill["batteryPercent"] = runtime.sample.batteryPercent;
            pill["batteryVolts"] = runtime.sample.batteryVolts;
        }
    }
    sendJson(200, document);
}

void handlePillSave()
{
    JsonDocument document;
    if (deserializeJson(document, requestArg("plain")) ||
        !document["address"].is<const char*>() ||
        !document["name"].is<const char*>())
    {
        sendResponse(400, "application/json",
                    "{\"error\":\"Pillen-Adresse und Name sind erforderlich\"}");
        return;
    }

    const String address = document["address"].as<String>();
    const String name = document["name"].as<String>();
    const String url = document["url"].is<const char*>()
                           ? document["url"].as<String>()
                           : "";
    int configurationIndex = findConfiguredPill(address.c_str());
    const bool isNewConfiguration = configurationIndex < 0;
    uint16_t intervalMinutes = isNewConfiguration
                                   ? DEFAULT_BREWFATHER_INTERVAL_MINUTES
                                   : pillConfigurations[configurationIndex]
                                         .intervalMinutes;
    const JsonVariantConst intervalValue = document["intervalMinutes"];
    if (!intervalValue.isUnbound())
    {
        if (!intervalValue.is<int>())
        {
            sendResponse(400, "application/json",
                        "{\"error\":\"Ungültiges Brewfather-Sendeintervall\"}");
            return;
        }
        const int requestedInterval = intervalValue.as<int>();
        if (!isSupportedBrewfatherInterval(requestedInterval))
        {
            sendResponse(400, "application/json",
                        "{\"error\":\"Brewfather-Sendeintervall muss 15, 30, 60, 120 oder 240 Minuten betragen\"}");
            return;
        }
        intervalMinutes = static_cast<uint16_t>(requestedInterval);
    }
    const bool usesHttpBrewfatherUrl =
        url.startsWith("http://log.brewfather.net/stream?id=");
    if (usesHttpBrewfatherUrl)
    {
        sendResponse(400, "application/json",
                    "{\"error\":\"Die Brewfather-URL beginnt mit http://. Bitte den Anfang zu https:// ändern, bevor du speicherst.\"}");
        return;
    }

    const bool invalidUrl =
        (url.isEmpty() && isNewConfiguration) ||
        (!url.isEmpty() &&
         (!url.startsWith("https://log.brewfather.net/stream?id=") ||
          url.length() >= 256));
    if (address.length() != 17 || name.isEmpty() || name.length() >= 48)
    {
        sendResponse(400, "application/json",
                    "{\"error\":\"Bitte eine gültige Pillen-Adresse und einen Namen eingeben\"}");
        return;
    }
    if (invalidUrl)
    {
        sendResponse(400, "application/json",
                    "{\"error\":\"Ungültige Brewfather-URL. Bitte die vollständige Custom-Stream-URL mit https://log.brewfather.net/stream?id= eingeben.\"}");
        return;
    }

    for (size_t i = 0; i < pillConfigurationCount; ++i)
    {
        if (static_cast<int>(i) != configurationIndex &&
            name.equals(pillConfigurations[i].name))
        {
            sendResponse(409, "application/json",
                        "{\"error\":\"Der Brewfather-Gerätename wird bereits für eine andere Pill verwendet\"}");
            return;
        }
    }
    if (isNewConfiguration)
    {
        if (pillConfigurationCount >= MAX_PILLS)
        {
            sendResponse(409, "application/json",
                        "{\"error\":\"Maximal 12 Pills können konfiguriert werden\"}");
            return;
        }
        configurationIndex = static_cast<int>(pillConfigurationCount++);
    }

    PillConfiguration& configuration =
        pillConfigurations[configurationIndex];
    const PillConfiguration previousConfiguration = configuration;
    copyString(configuration.address, sizeof(configuration.address), address);
    copyString(configuration.name, sizeof(configuration.name), name);
    configuration.intervalMinutes = intervalMinutes;
    if (!url.isEmpty())
    {
        copyString(configuration.streamUrl, sizeof(configuration.streamUrl), url);
    }
    if (!savePillConfigurations())
    {
        configuration = previousConfiguration;
        if (isNewConfiguration)
        {
            --pillConfigurationCount;
        }
        sendResponse(500, "application/json",
                    "{\"error\":\"Pill-Konfiguration konnte nicht gespeichert werden\"}");
        return;
    }

    JsonDocument response;
    response["saved"] = true;
    response["urlConfigured"] = configuration.streamUrl[0] != '\0';
    sendJson(200, response);
    Serial.printf("Pill %s als '%s' gespeichert\n",
                  configuration.address, configuration.name);
}

void handlePillTest()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        sendResponse(503, "application/json",
                    "{\"error\":\"WLAN-Verbindung erforderlich\"}");
        return;
    }

    const String address = requestArg("address");
    const int configurationIndex = findConfiguredPill(address.c_str());
    if (address.length() != 17 || configurationIndex < 0)
    {
        sendResponse(404, "application/json",
                    "{\"error\":\"Pill-Konfiguration nicht gefunden\"}");
        return;
    }

    const PillConfiguration configuration =
        pillConfigurations[configurationIndex];
    PillRuntime runtime = {};
    portENTER_CRITICAL(&stateMux);
    const int runtimeIndex = findDiscoveredPill(configuration.address);
    if (runtimeIndex >= 0)
    {
        runtime = discoveredPills[runtimeIndex];
    }
    portEXIT_CRITICAL(&stateMux);

    const uint32_t now = millis();
    if (!runtime.hasSample ||
        static_cast<uint32_t>(now - runtime.sampleReceivedAt) >
            SAMPLE_MAX_AGE_MS)
    {
        sendResponse(409, "application/json",
                    "{\"error\":\"Kein aktueller Messwert verfügbar\"}");
        return;
    }

    portENTER_CRITICAL(&stateMux);
    const int currentRuntimeIndex = findDiscoveredPill(configuration.address);
    if (currentRuntimeIndex >= 0)
    {
        discoveredPills[currentRuntimeIndex].lastPostAttempt = now;
        discoveredPills[currentRuntimeIndex].hasAttemptedPost = true;
    }
    portEXIT_CRITICAL(&stateMux);

    const BrewfatherResult result = sendToBrewfather(configuration, runtime);
    recordBrewfatherResult(configuration.address, result);
    JsonDocument response;
    response["httpStatus"] = result.status;
    if (result.status >= 200 && result.status < 300)
    {
        response["sent"] = true;
        sendJson(200, response);
        return;
    }

    response["sent"] = false;
    response["error"] = result.status > 0
                            ? String("Brewfather HTTP-Fehler ") +
                                  result.status +
                                  (result.detail.isEmpty()
                                       ? ""
                                       : String(": ") + result.detail)
                            : result.detail;
    sendJson(502, response);
}

void handlePillDelete()
{
    const String address = requestArg("address");
    const int configurationIndex = findConfiguredPill(address.c_str());
    if (configurationIndex < 0)
    {
        sendResponse(404, "application/json",
                    "{\"error\":\"Pill-Konfiguration nicht gefunden\"}");
        return;
    }

    const PillConfiguration deletedConfiguration =
        pillConfigurations[configurationIndex];
    for (size_t i = static_cast<size_t>(configurationIndex);
         i + 1 < pillConfigurationCount; ++i)
    {
        pillConfigurations[i] = pillConfigurations[i + 1];
    }
    --pillConfigurationCount;
    pillConfigurations[pillConfigurationCount] = {};
    if (!savePillConfigurations())
    {
        for (size_t i = pillConfigurationCount;
             i > static_cast<size_t>(configurationIndex); --i)
        {
            pillConfigurations[i] = pillConfigurations[i - 1];
        }
        pillConfigurations[configurationIndex] = deletedConfiguration;
        ++pillConfigurationCount;
        sendResponse(500, "application/json",
                    "{\"error\":\"Pill-Konfiguration konnte nicht gespeichert werden\"}");
        return;
    }
    sendResponse(200, "application/json", "{\"deleted\":true}");
}

using RouteHandler = void (*)();

struct SecureRoute
{
    const char* uri;
    httpd_method_t method;
    RouteHandler handler;
    bool requiresAuthentication;
};

void handleRoot()
{
    File file = LittleFS.open("/index.html", "r");
    if (!file)
    {
        sendResponse(500, "text/plain", "Web interface is unavailable.");
        return;
    }

    String body;
    body.reserve(file.size());
    char buffer[1024];
    while (file.available())
    {
        const size_t count = file.read(
            reinterpret_cast<uint8_t*>(buffer), sizeof(buffer));
        if (count == 0)
        {
            file.close();
            sendResponse(500, "text/plain", "Web interface could not be read.");
            return;
        }
        body.concat(buffer, count);
    }
    file.close();
    sendResponse(200, "text/html; charset=utf-8", body);
}

esp_err_t handleSecureRequest(httpd_req_t* request)
{
    if (webRequestMutex == nullptr ||
        xSemaphoreTake(webRequestMutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_FAIL;
    }
    activeHttpsRequest = request;
    activeHttpsBody = "";
    activeHttpsBodyRead = false;
    secureResponseHeaderCount = 0;
    const SecureRoute* route =
        static_cast<const SecureRoute*>(request->user_ctx);

    if (request->content_len > 2048)
    {
        sendResponse(413, "application/json", "{\"error\":\"Request too large\"}");
    }
    else if (route->requiresAuthentication &&
             !currentRequestAuthenticated())
    {
        sendResponse(401, "application/json",
                     "{\"error\":\"Please sign in to administer this gateway\"}");
    }
    else if ((request->method == HTTP_POST ||
              request->method == HTTP_DELETE) &&
             !requestOriginIsSame())
    {
        sendResponse(403, "application/json",
                     "{\"error\":\"Request origin was not accepted\"}");
    }
    else
    {
        if (route->requiresAuthentication)
        {
            setAdminSessionCookie(requestCookie("rg_session"));
        }
        route->handler();
    }

    activeHttpsRequest = nullptr;
    activeHttpsBody = "";
    activeHttpsBodyRead = false;
    xSemaphoreGive(webRequestMutex);
    return ESP_OK;
}

void dispatchApRoute(RouteHandler handler,
                     bool requiresAuthentication,
                     bool setupOnly = false)
{
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-Content-Type-Options", "nosniff");
    server.sendHeader("Referrer-Policy", "no-referrer");
    if (!isSetupApClient())
    {
        if (WiFi.status() == WL_CONNECTED && httpsAvailable)
        {
            server.sendHeader(
                "Location",
                String("https://") + webServerHostname() + "/",
                true);
            server.send(302, "text/plain", "");
        }
        else if (WiFi.status() == WL_CONNECTED)
        {
            server.send(503, "text/plain",
                        "HTTPS administration is unavailable.");
        }
        else
        {
            server.send(403, "text/plain",
                        "Connect to the RaptGateway setup network.");
        }
        return;
    }

    if (setupOnly && (isHttpsRequest() || hasAdminPassword(preferences)))
    {
        server.send(403, "application/json",
                    "{\"error\":\"Admin password setup is only available once from the setup access point\"}");
        return;
    }
    if (requiresAuthentication && !currentRequestAuthenticated())
    {
        server.send(401, "application/json",
                    "{\"error\":\"Please sign in to administer this gateway\"}");
        return;
    }
    const HTTPMethod method = server.method();
    if ((method == HTTP_POST || method == HTTP_DELETE) &&
        !requestOriginIsSame())
    {
        server.send(403, "application/json",
                    "{\"error\":\"Request origin was not accepted\"}");
        return;
    }
    if (requiresAuthentication)
    {
        setAdminSessionCookie(requestCookie("rg_session"));
    }
    handler();
}

bool startHttpsServer()
{
    httpd_ssl_config_t configuration = HTTPD_SSL_CONFIG_DEFAULT();
    configuration.cacert_pem =
        reinterpret_cast<const uint8_t*>(webServerCertificate());
    configuration.cacert_len = webServerCertificateLength();
    configuration.prvtkey_pem =
        reinterpret_cast<const uint8_t*>(webServerPrivateKey());
    configuration.prvtkey_len = webServerPrivateKeyLength();
    configuration.httpd.stack_size = 12288;
    configuration.httpd.max_open_sockets = 2;
    configuration.httpd.max_uri_handlers = 16;
    configuration.httpd.lru_purge_enable = true;
    configuration.session_tickets = false;

    static SecureRoute routes[] = {
        {"/", HTTP_GET, handleRoot, false},
        {"/api/auth/status", HTTP_GET, handleAuthStatus, false},
        {"/api/auth/setup", HTTP_POST, handleAdminPasswordSetup, false},
        {"/api/auth/login", HTTP_POST, handleAdminLogin, false},
        {"/api/auth/logout", HTTP_POST, handleAdminLogout, true},
        {"/api/factory-reset", HTTP_POST, handleFactoryReset, true},
        {"/api/status", HTTP_GET, handleStatus, true},
        {"/api/wifi/scan", HTTP_GET, handleWifiScan, true},
        {"/api/wifi", HTTP_POST, handleWifiSave, true},
        {"/api/pills", HTTP_GET, handlePillsGet, true},
        {"/api/pills", HTTP_POST, handlePillSave, true},
        {"/api/pills/test", HTTP_POST, handlePillTest, true},
        {"/api/pills", HTTP_DELETE, handlePillDelete, true}
    };

    if (httpd_ssl_start(&httpsServer, &configuration) != ESP_OK)
    {
        Serial.println("HTTPS server failed to start");
        return false;
    }
    for (SecureRoute& route : routes)
    {
        httpd_uri_t uri = {};
        uri.uri = route.uri;
        uri.method = route.method;
        uri.handler = handleSecureRequest;
        uri.user_ctx = &route;
        if (httpd_register_uri_handler(httpsServer, &uri) != ESP_OK)
        {
            Serial.printf("HTTPS route registration failed: %s\n", route.uri);
            httpd_ssl_stop(httpsServer);
            httpsServer = nullptr;
            return false;
        }
    }
    Serial.println("HTTPS administration available on port 443");
    return true;
}

void startWebServer()
{
    server.on("/", HTTP_GET, []() {
        if (!isSetupApClient())
        {
            dispatchApRoute(handleRoot, false);
            return;
        }
        File file = LittleFS.open("/index.html", "r");
        if (!file)
        {
            server.send(500, "text/plain", "Weboberfläche fehlt. LittleFS hochladen.");
            return;
        }
        server.sendHeader("Cache-Control", "no-store");
        server.sendHeader("X-Content-Type-Options", "nosniff");
        server.sendHeader("Referrer-Policy", "no-referrer");
        server.streamFile(file, "text/html; charset=utf-8");
        file.close();
    });
    const char* requestHeaders[] = {"Cookie", "Origin"};
    server.collectHeaders(requestHeaders, 2);
    server.on("/api/auth/status", HTTP_GET, []() {
        dispatchApRoute(handleAuthStatus, false);
    });
    server.on("/api/auth/setup", HTTP_POST, []() {
        dispatchApRoute(handleAdminPasswordSetup, false, true);
    });
    server.on("/api/auth/login", HTTP_POST, []() {
        dispatchApRoute(handleAdminLogin, false);
    });
    server.on("/api/auth/logout", HTTP_POST, []() {
        dispatchApRoute(handleAdminLogout, true);
    });
    server.on("/api/status", HTTP_GET, []() {
        dispatchApRoute(handleStatus, true);
    });
    server.on("/api/wifi/scan", HTTP_GET, []() {
        dispatchApRoute(handleWifiScan, true);
    });
    server.on("/api/wifi", HTTP_POST, []() {
        dispatchApRoute(handleWifiSave, true);
    });
    server.on("/api/pills", HTTP_GET, []() {
        dispatchApRoute(handlePillsGet, true);
    });
    server.on("/api/pills", HTTP_POST, []() {
        dispatchApRoute(handlePillSave, true);
    });
    server.on("/api/pills/test", HTTP_POST, []() {
        dispatchApRoute(handlePillTest, true);
    });
    server.on("/api/pills", HTTP_DELETE, []() {
        dispatchApRoute(handlePillDelete, true);
    });
    const char* captivePortalProbes[] = {
        "/generate_204",
        "/gen_204",
        "/hotspot-detect.html",
        "/library/test/success.html",
        "/connecttest.txt",
        "/ncsi.txt",
        "/fwlink"
    };
    for (const char* probe : captivePortalProbes)
    {
        server.on(probe, HTTP_GET, []() {
            dispatchApRoute([]() {
                server.sendHeader("Location", "http://192.168.4.1/", true);
                server.send(302, "text/plain", "");
            }, false);
        });
    }
    server.onNotFound([]() {
        if (!isSetupApClient() && WiFi.status() == WL_CONNECTED &&
            httpsAvailable)
        {
            server.sendHeader(
                "Location",
                String("https://") + webServerHostname() + "/",
                true);
            server.send(302, "text/plain", "");
        }
        else if (!isSetupApClient() && WiFi.status() == WL_CONNECTED)
        {
            server.send(503, "text/plain",
                        "HTTPS administration is unavailable.");
        }
        else if (server.uri().startsWith("/api/"))
        {
            server.send(404, "application/json", "{\"error\":\"API-Endpunkt nicht gefunden\"}");
        }
        else
        {
            server.sendHeader("Location", "/", true);
            server.send(302, "text/plain", "");
        }
    });
    server.begin();
}

class ScanCallbacks : public NimBLEScanCallbacks
{
    void onResult(const NimBLEAdvertisedDevice* device) override
    {
        if (!device->haveManufacturerData())
        {
            return;
        }

        const uint8_t manufacturerDataCount = device->getManufacturerDataCount();
        RaptSample sample = {};
        std::string raptManufacturerData;
        std::string parsedManufacturerData;
        bool recognizedRapt = false;
        bool parsed = false;
        for (uint8_t index = 0; index < manufacturerDataCount; ++index)
        {
            const std::string manufacturerData = device->getManufacturerData(index);
            if (manufacturerData.size() >= 2 &&
                static_cast<uint8_t>(manufacturerData[0]) == 0x52 &&
                static_cast<uint8_t>(manufacturerData[1]) == 0x41 &&
                manufacturerData.size() >= 4 &&
                manufacturerData[2] == 'P' &&
                manufacturerData[3] == 'T')
            {
                recognizedRapt = true;
                if (raptManufacturerData.empty())
                {
                    raptManufacturerData = manufacturerData;
                }
            }

            if (parseRaptAdvertisement(
                    reinterpret_cast<const uint8_t*>(manufacturerData.data()),
                    manufacturerData.size(),
                    sample))
            {
                parsed = true;
                parsedManufacturerData = manufacturerData;
                break;
            }
        }

        if (!recognizedRapt)
        {
            return;
        }

        const String address = device->getAddress().toString().c_str();
        portENTER_CRITICAL(&stateMux);
        int index = findDiscoveredPill(address.c_str());
        if (index < 0)
        {
            const uint32_t now = millis();
            for (size_t i = 0; i < MAX_DISCOVERED_PILLS; ++i)
            {
                if (discoveredPills[i].address[0] == '\0' ||
                    static_cast<uint32_t>(now - discoveredPills[i].lastSeen) >
                        DISCOVERY_MAX_AGE_MS)
                {
                    index = static_cast<int>(i);
                    discoveredPills[i] = {};
                    strncpy(discoveredPills[i].address, address.c_str(),
                            sizeof(discoveredPills[i].address) - 1);
                    discoveredPills[i].address[
                        sizeof(discoveredPills[i].address) - 1] = '\0';
                    break;
                }
            }
        }

        if (index >= 0)
        {
            discoveredPills[index].lastSeen = millis();
            discoveredPills[index].bleRssi = device->getRSSI();
            discoveredPills[index].hasBleRssi = true;
            if (parsed)
            {
                discoveredPills[index].sample = sample;
                discoveredPills[index].sampleReceivedAt = millis();
                discoveredPills[index].hasSample = true;
            }
        }
        portEXIT_CRITICAL(&stateMux);

        if (!parsed)
        {
            static uint32_t lastPayloadDiagnostic = 0;
            const uint32_t now = millis();
            if (static_cast<uint32_t>(now - lastPayloadDiagnostic) >= 10000UL)
            {
                Serial.printf("RAPT Payload nicht dekodiert von %s (%s), %u Herstellerdaten: %u Bytes: ",
                              address.c_str(),
                              device->getName().c_str(),
                              static_cast<unsigned>(manufacturerDataCount),
                              static_cast<unsigned>(raptManufacturerData.size()));
                for (unsigned char byte : raptManufacturerData)
                {
                    Serial.printf("%02X ", byte);
                }
                Serial.println();
                lastPayloadDiagnostic = now;
            }
            return;
        }

        Serial.printf("RAPT %s: %.2f C, Gravity %.4f, Batterie %.0f%% / %.2f V",
                      address.c_str(), sample.temperature, sample.gravity,
                      sample.batteryPercent, sample.batteryVolts);
        static uint32_t lastRawDiagnostic = 0;
        const uint32_t now = millis();
        if (static_cast<uint32_t>(now - lastRawDiagnostic) >= 10000UL)
        {
            Serial.print(" | Payload:");
            for (unsigned char byte : parsedManufacturerData)
            {
                Serial.printf(" %02X", byte);
            }
            lastRawDiagnostic = now;
        }
        Serial.println();
    }
};
} // namespace

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("Starte RaptGetaway");

    loadSettings();
    pinMode(4, INPUT_PULLUP);
    webRequestMutex = xSemaphoreCreateMutex();
    if (webRequestMutex == nullptr)
    {
        Serial.println("Web request mutex could not be created");
    }

    httpsAvailable = initializeWebSecurity(preferences);
    if (!httpsAvailable)
    {
        Serial.println("HTTPS administration is unavailable; HTTP remains setup-AP-only");
    }

    if (!LittleFS.begin(true))
    {
        Serial.println("LittleFS konnte nicht gestartet werden; Webseite ist nicht verfügbar");
    }

    String savedSsid;
    String savedPassword;
    const bool hasSavedWifi =
        readSavedWifiCredentials(savedSsid, savedPassword);
    WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
        if (event == ARDUINO_EVENT_WIFI_AP_START)
        {
            Serial.println("WLAN-Diagnose: Access Point gestartet");
        }
        else if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED)
        {
            const wifi_event_ap_staconnected_t& station =
                info.wifi_ap_staconnected;
            Serial.printf("WLAN-Diagnose: Client verbunden %02X:%02X:%02X:%02X:%02X:%02X (AID %u)\n",
                          station.mac[0], station.mac[1], station.mac[2],
                          station.mac[3], station.mac[4], station.mac[5],
                          station.aid);
        }
        else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED)
        {
            const wifi_event_ap_stadisconnected_t& station =
                info.wifi_ap_stadisconnected;
            Serial.printf("WLAN-Diagnose: Client getrennt %02X:%02X:%02X:%02X:%02X:%02X (AID %u)\n",
                          station.mac[0], station.mac[1], station.mac[2],
                          station.mac[3], station.mac[4], station.mac[5],
                          station.aid);
        }
        else if (event == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED)
        {
            Serial.printf("WLAN-Diagnose: DHCP-IP vergeben: " IPSTR "\n",
                          IP2STR(&info.wifi_ap_staipassigned.ip));
        }
        else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
        {
            Serial.printf("WLAN verbunden; IP-Adresse: " IPSTR "\n",
                          IP2STR(&info.got_ip.ip_info.ip));
        }
        else if (event == ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED)
        {
            static uint32_t lastProbeLog = 0;
            const uint32_t now = millis();
            if (static_cast<uint32_t>(now - lastProbeLog) >= 5000UL)
            {
                Serial.printf("WLAN-Diagnose: WLAN-Suche empfangen (RSSI %d dBm)\n",
                              info.wifi_ap_probereqrecved.rssi);
                lastProbeLog = now;
            }
        }
    });
    if (!WiFi.mode(hasSavedWifi ? WIFI_AP_STA : WIFI_AP))
    {
        Serial.println("WLAN-Diagnose: WLAN-Modus konnte nicht gestartet werden");
    }
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    if (!WiFi.setSleep(true))
    {
        Serial.println("WLAN-Modemschlaf konnte nicht aktiviert werden");
    }
    Serial.printf("WLAN-Diagnose: Starte im Modus %s\n",
                  hasSavedWifi ? "AP+STA" : "AP-only");

    if (startSetupAccessPoint(hasSavedWifi))
    {
        Serial.printf("Setup-Webseite: http://%s\n",
                      WiFi.softAPIP().toString().c_str());
    }

    if (hasSavedWifi)
    {
        WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
        Serial.printf("Verbinde mit gespeichertem WLAN '%s'\n", savedSsid.c_str());
    }

    startWebServer();
    if (webRequestMutex == nullptr)
    {
        httpsAvailable = false;
    }
    else if (httpsAvailable)
    {
        httpsAvailable = startHttpsServer();
    }

    NimBLEDevice::init("");
    bleScan = NimBLEDevice::getScan();
    bleScan->setScanCallbacks(new ScanCallbacks(), true);
    bleScan->setActiveScan(false);
    bleScan->setDuplicateFilter(0);
    Serial.println("BLE-Scan pausiert, bis eine WLAN-Client-Verbindung besteht");
}

void loop()
{
    if (setupApActive)
    {
        dnsServer.processNextRequest();
    }
    if (webRequestMutex != nullptr &&
        xSemaphoreTake(webRequestMutex, portMAX_DELAY) == pdTRUE)
    {
        server.handleClient();
        xSemaphoreGive(webRequestMutex);
    }

    if (factoryResetRequested.exchange(false, std::memory_order_acq_rel))
    {
        Serial.println("Factory reset requested from web interface; restarting");
        delay(500);
        ESP.restart();
    }

    if (digitalRead(4) == LOW)
    {
        if (factoryResetPressedAt == 0)
        {
            factoryResetPressedAt = millis();
        }
        else if (!factoryResetHandled &&
                 static_cast<uint32_t>(millis() - factoryResetPressedAt) >=
                     5000UL)
        {
            factoryResetHandled = true;
            Serial.println("Factory reset requested on GPIO4; clearing settings");
            const bool locked =
                webRequestMutex != nullptr &&
                xSemaphoreTake(webRequestMutex, portMAX_DELAY) == pdTRUE;
            const bool cleared =
                (webRequestMutex == nullptr || locked) && preferences.clear();
            if (cleared)
            {
                clearAdminSession();
            }
            if (locked)
            {
                xSemaphoreGive(webRequestMutex);
            }
            if (!cleared)
            {
                Serial.println("Factory reset failed: NVS settings could not be cleared");
                factoryResetHandled = false;
                factoryResetPressedAt = 0;
            }
            else
            {
                Serial.println("Settings cleared; restarting");
                delay(250);
                ESP.restart();
            }
        }
    }
    else
    {
        factoryResetPressedAt = 0;
        factoryResetHandled = false;
    }

    const uint32_t now = millis();
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    if (wifiConnected && !tlsTimeSyncStarted)
    {
        configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
        tlsTimeSyncStarted = true;
        lastTlsTimeSyncLog = now;
        Serial.println("SNTP-Zeitsynchronisierung gestartet");
    }
    const bool tlsClockSynchronized =
        wifiConnected && time(nullptr) >= TLS_VALIDATION_MIN_EPOCH;
    if (wifiConnected && !tlsClockSynchronized &&
        static_cast<uint32_t>(now - lastTlsTimeSyncLog) >= 30000UL)
    {
        Serial.println("Warte auf SNTP-Zeit; Brewfather-TLS bleibt bis dahin gesperrt");
        lastTlsTimeSyncLog = now;
    }
    String savedSsid;
    String savedPassword;
    bool adminPasswordConfigured = false;
    const bool hasSavedWifi =
        readSavedWifiCredentials(
            savedSsid, savedPassword, &adminPasswordConfigured);
    if (wifiConnected && httpsAvailable && !mdnsStarted &&
        static_cast<uint32_t>(now - lastMdnsAttempt) >= WIFI_RETRY_INTERVAL_MS)
    {
        lastMdnsAttempt = now;
        String hostname = webServerHostname();
        hostname.replace(".local", "");
        if (MDNS.begin(hostname.c_str()))
        {
            if (!MDNS.addService("https", "tcp", 443))
            {
                Serial.println("HTTPS service could not be advertised with mDNS");
            }
            mdnsStarted = true;
            Serial.printf("HTTPS administration: https://%s.local/\n",
                          hostname.c_str());
        }
        else
        {
            Serial.println("mDNS hostname could not be started");
        }
    }
    else if (!wifiConnected && mdnsStarted)
    {
        MDNS.end();
        mdnsStarted = false;
    }
    if (wifiConnected && setupApActive && adminPasswordConfigured &&
        static_cast<int32_t>(now - setupApCloseNotBefore) >= 0)
    {
        stopSetupAccessPoint();
    }
    else if (!wifiConnected && !setupApActive &&
             hasSavedWifi &&
             static_cast<uint32_t>(now - lastSetupApAttempt) >=
                 WIFI_RETRY_INTERVAL_MS)
    {
        startSetupAccessPoint(true);
        lastSetupApAttempt = now;
    }

    static uint32_t lastApStatusLog = 0;
    if (static_cast<uint32_t>(now - lastApStatusLog) >= 5000UL)
    {
        Serial.printf("WLAN-Diagnose: verbundene Setup-Clients: %u\n",
                      WiFi.softAPgetStationNum());
        lastApStatusLog = now;
    }

    if (WiFi.status() != WL_CONNECTED &&
        static_cast<uint32_t>(now - lastWifiAttempt) >= WIFI_RETRY_INTERVAL_MS)
    {
        if (hasSavedWifi)
        {
            if (WiFi.getMode() != WIFI_AP_STA)
            {
                WiFi.mode(WIFI_AP_STA);
            }
            WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
        }
        lastWifiAttempt = now;
    }

    if (bleScan != nullptr &&
        static_cast<uint32_t>(now - lastBleScanAttempt) >=
            BLE_SCAN_RETRY_INTERVAL_MS)
    {
        if (wifiConnected && !bleScan->isScanning())
        {
            if (bleScan->start(0, false, false))
            {
                Serial.println("WLAN-Client verbunden; BLE-Scan gestartet");
            }
            else
            {
                Serial.println("WLAN-Client verbunden, aber BLE-Scan konnte nicht gestartet werden");
            }
            lastBleScanAttempt = now;
        }
        else if (!wifiConnected && bleScan->isScanning())
        {
            if (bleScan->stop())
            {
                Serial.println("WLAN-Client getrennt; BLE-Scan gestoppt");
            }
            else
            {
                Serial.println("WLAN-Client getrennt, aber BLE-Scan konnte nicht gestoppt werden");
            }
            lastBleScanAttempt = now;
        }
    }

    if (wifiConnected)
    {
        for (size_t i = 0; i < pillConfigurationCount; ++i)
        {
            PillRuntime runtime = {};
            PillConfiguration configuration = pillConfigurations[i];
            const uint32_t uploadIntervalMs =
                static_cast<uint32_t>(configuration.intervalMinutes) *
                60UL * 1000UL;
            bool shouldPost = false;

            portENTER_CRITICAL(&stateMux);
            const int runtimeIndex = findDiscoveredPill(configuration.address);
            if (runtimeIndex >= 0)
            {
                runtime = discoveredPills[runtimeIndex];
                shouldPost = runtime.hasSample &&
                             static_cast<uint32_t>(now - runtime.sampleReceivedAt) <=
                                 SAMPLE_MAX_AGE_MS &&
                             tlsClockSynchronized &&
                             (!runtime.hasAttemptedPost ||
                              static_cast<uint32_t>(now - runtime.lastPostAttempt) >=
                                  uploadIntervalMs);
                if (shouldPost)
                {
                    discoveredPills[runtimeIndex].lastPostAttempt = now;
                    discoveredPills[runtimeIndex].hasAttemptedPost = true;
                }
            }
            portEXIT_CRITICAL(&stateMux);

            if (shouldPost)
            {
                runtime.lastPostAttempt = now;
                runtime.hasAttemptedPost = true;
                const BrewfatherResult result =
                    sendToBrewfather(configuration, runtime);
                recordBrewfatherResult(configuration.address, result);
            }
        }
    }

    delay(10);
}
