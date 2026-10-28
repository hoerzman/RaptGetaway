#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cstring>

#include "config.h"
#include "rapt_parser.h"

namespace
{
constexpr size_t MAX_PILLS = 12;
constexpr size_t MAX_DISCOVERED_PILLS = 20;
constexpr uint32_t BREWFATHER_INTERVAL_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000UL;
constexpr uint32_t BLE_SCAN_RETRY_INTERVAL_MS = 5000UL;
constexpr uint32_t SAMPLE_MAX_AGE_MS = 20UL * 60UL * 1000UL;
constexpr uint32_t DISCOVERY_MAX_AGE_MS = 60000UL;

struct PillConfiguration
{
    char address[18];
    char name[48];
    char streamUrl[256];
};

struct PillRuntime
{
    char address[18];
    RaptSample sample;
    uint32_t lastSeen;
    uint32_t sampleReceivedAt;
    uint32_t lastPostAttempt;
    bool hasSample;
    bool hasAttemptedPost;
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
DNSServer dnsServer;
Preferences preferences;
NimBLEScan* bleScan = nullptr;
uint32_t lastWifiAttempt = 0;
uint32_t lastBleScanAttempt = 0;
uint32_t lastSetupApAttempt = 0;
uint32_t bootStartedAt = 0;
uint32_t setupApStartedAt = 0;
bool setupApActive = false;

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

        if (configuration.address[0] != '\0' &&
            configuration.name[0] != '\0' &&
            configuration.streamUrl[0] != '\0')
        {
            ++pillConfigurationCount;
        }
    }
}

BrewfatherResult sendToBrewfather(const PillConfiguration& configuration,
                                  const PillRuntime& runtime)
{
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
    client.setInsecure();

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

void sendJson(int statusCode, JsonDocument& document)
{
    String body;
    serializeJson(document, body);
    server.send(statusCode, "application/json; charset=utf-8", body);
}

void handleStatus()
{
    JsonDocument document;
    document["wifiConnected"] = WiFi.status() == WL_CONNECTED;
    document["wifiSsid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
    document["ip"] = WiFi.status() == WL_CONNECTED
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
        server.send(500, "application/json",
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
        server.send(500, "application/json",
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
    if (deserializeJson(document, server.arg("plain")) ||
        !document["ssid"].is<const char*>() ||
        !document["password"].is<const char*>())
    {
        server.send(400, "application/json",
                    "{\"error\":\"SSID oder Passwort fehlt\"}");
        return;
    }

    const String ssid = document["ssid"].as<String>();
    const String password = document["password"].as<String>();
    if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63)
    {
        server.send(400, "application/json",
                    "{\"error\":\"SSID oder Passwort hat eine ungültige Länge\"}");
        return;
    }

    if (preferences.putString("wifiSsid", ssid) == 0 ||
        preferences.putString("wifiPass", password) == 0)
    {
        server.send(500, "application/json",
                    "{\"error\":\"WLAN-Zugangsdaten konnten nicht gespeichert werden\"}");
        return;
    }
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
        pill["url"] = configuration.streamUrl;
        pill["configured"] = true;
        pill["online"] = runtime.address[0] != '\0' &&
                         static_cast<uint32_t>(now - runtime.lastSeen) <=
                             DISCOVERY_MAX_AGE_MS;
        if (runtime.hasSample)
        {
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
        if (runtime.hasSample)
        {
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
    if (deserializeJson(document, server.arg("plain")) ||
        !document["address"].is<const char*>() ||
        !document["name"].is<const char*>() ||
        !document["url"].is<const char*>())
    {
        server.send(400, "application/json",
                    "{\"error\":\"Pillen-Adresse, Name und Brewfather-URL sind erforderlich\"}");
        return;
    }

    const String address = document["address"].as<String>();
    const String name = document["name"].as<String>();
    const String url = document["url"].as<String>();
    if (address.length() != 17 || name.isEmpty() || name.length() >= 48 ||
        !url.startsWith("https://log.brewfather.net/stream?id=") ||
        url.length() >= 256)
    {
        server.send(400, "application/json",
                    "{\"error\":\"Bitte eine gültige Adresse, einen Namen und die vollständige Brewfather-URL eingeben\"}");
        return;
    }

    int configurationIndex = findConfiguredPill(address.c_str());
    for (size_t i = 0; i < pillConfigurationCount; ++i)
    {
        if (static_cast<int>(i) != configurationIndex &&
            name.equals(pillConfigurations[i].name))
        {
            server.send(409, "application/json",
                        "{\"error\":\"Der Brewfather-Gerätename wird bereits für eine andere Pill verwendet\"}");
            return;
        }
    }
    const bool isNewConfiguration = configurationIndex < 0;
    if (isNewConfiguration)
    {
        if (pillConfigurationCount >= MAX_PILLS)
        {
            server.send(409, "application/json",
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
    copyString(configuration.streamUrl, sizeof(configuration.streamUrl), url);
    if (!savePillConfigurations())
    {
        configuration = previousConfiguration;
        if (isNewConfiguration)
        {
            --pillConfigurationCount;
        }
        server.send(500, "application/json",
                    "{\"error\":\"Pill-Konfiguration konnte nicht gespeichert werden\"}");
        return;
    }

    JsonDocument response;
    response["saved"] = true;
    sendJson(200, response);
    Serial.printf("Pill %s als '%s' gespeichert\n",
                  configuration.address, configuration.name);
}

void handlePillTest()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        server.send(503, "application/json",
                    "{\"error\":\"WLAN-Verbindung erforderlich\"}");
        return;
    }

    const String address = server.arg("address");
    const int configurationIndex = findConfiguredPill(address.c_str());
    if (address.length() != 17 || configurationIndex < 0)
    {
        server.send(404, "application/json",
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
        server.send(409, "application/json",
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
    const String address = server.arg("address");
    const int configurationIndex = findConfiguredPill(address.c_str());
    if (configurationIndex < 0)
    {
        server.send(404, "application/json",
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
        server.send(500, "application/json",
                    "{\"error\":\"Pill-Konfiguration konnte nicht gespeichert werden\"}");
        return;
    }
    server.send(200, "application/json", "{\"deleted\":true}");
}

void startWebServer()
{
    server.on("/", HTTP_GET, []() {
        File file = LittleFS.open("/index.html", "r");
        if (!file)
        {
            server.send(500, "text/plain", "Weboberfläche fehlt. LittleFS hochladen.");
            return;
        }
        server.streamFile(file, "text/html; charset=utf-8");
        file.close();
    });
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
    server.on("/api/wifi", HTTP_POST, handleWifiSave);
    server.on("/api/pills", HTTP_GET, handlePillsGet);
    server.on("/api/pills", HTTP_POST, handlePillSave);
    server.on("/api/pills/test", HTTP_POST, handlePillTest);
    server.on("/api/pills", HTTP_DELETE, handlePillDelete);
    server.onNotFound([]() {
        if (server.uri().startsWith("/api/"))
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
    Serial.println("HTTPS-Zertifikatspruefung ist derzeit deaktiviert");

    loadSettings();
    bootStartedAt = millis();

    if (!LittleFS.begin(true))
    {
        Serial.println("LittleFS konnte nicht gestartet werden; Webseite ist nicht verfügbar");
    }

    const String savedSsid = preferences.isKey("wifiSsid")
                                 ? preferences.getString("wifiSsid")
                                 : "";
    const bool hasSavedWifi = !savedSsid.isEmpty();
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

    const String savedPassword = preferences.isKey("wifiPass")
                                     ? preferences.getString("wifiPass")
                                     : "";
    if (hasSavedWifi)
    {
        WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
        Serial.printf("Verbinde mit gespeichertem WLAN '%s'\n", savedSsid.c_str());
    }

    startWebServer();

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
    server.handleClient();

    const uint32_t now = millis();
    const bool wifiConnected = WiFi.status() == WL_CONNECTED;
    if (wifiConnected && setupApActive)
    {
        stopSetupAccessPoint();
    }
    else if (!wifiConnected && !setupApActive &&
             preferences.isKey("wifiSsid") &&
             !preferences.getString("wifiSsid").isEmpty() &&
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
        const String savedSsid = preferences.isKey("wifiSsid")
                                     ? preferences.getString("wifiSsid")
                                     : "";
        const String savedPassword = preferences.isKey("wifiPass")
                                         ? preferences.getString("wifiPass")
                                         : "";
        if (preferences.isKey("wifiSsid") && !savedSsid.isEmpty())
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
            bool shouldPost = false;

            portENTER_CRITICAL(&stateMux);
            const int runtimeIndex = findDiscoveredPill(configuration.address);
            if (runtimeIndex >= 0)
            {
                runtime = discoveredPills[runtimeIndex];
                shouldPost = runtime.hasSample &&
                             static_cast<uint32_t>(now - runtime.sampleReceivedAt) <=
                                 SAMPLE_MAX_AGE_MS &&
                             (static_cast<uint32_t>(now - bootStartedAt) >=
                                  BREWFATHER_INTERVAL_MS ||
                              runtime.hasAttemptedPost) &&
                             (!runtime.hasAttemptedPost ||
                              static_cast<uint32_t>(now - runtime.lastPostAttempt) >=
                                  BREWFATHER_INTERVAL_MS);
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
                sendToBrewfather(configuration, runtime);
            }
        }
    }

    delay(10);
}
