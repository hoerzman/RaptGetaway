#pragma once

#include <Preferences.h>
#include <WString.h>
#include <cstddef>

bool initializeWebSecurity(Preferences& preferences);
const char* webServerCertificate();
size_t webServerCertificateLength();
const char* webServerPrivateKey();
size_t webServerPrivateKeyLength();
const char* webServerHostname();
const char* webServerCertificateFingerprint();

bool hasAdminPassword(Preferences& preferences);
bool setAdminPassword(Preferences& preferences, const String& password);
bool verifyAdminPassword(Preferences& preferences, const String& password);

String createAdminSession();
bool isValidAdminSession(const String& token);
void clearAdminSession();
