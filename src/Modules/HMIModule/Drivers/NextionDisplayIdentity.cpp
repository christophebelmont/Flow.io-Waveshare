/**
 * @file NextionDisplayIdentity.cpp
 * @brief Strict parsing and normalization of Nextion display identities.
 */

#include "Modules/HMIModule/Drivers/NextionDisplayIdentity.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace {

static constexpr const char* kArtifactPrefix = "FlowIO_Nextion_";
static constexpr const char* kArtifactExtension = ".tft";

bool appendChar_(char* out, size_t outLen, size_t& pos, char value)
{
    if (!out || pos + 1U >= outLen) return false;
    out[pos++] = value;
    out[pos] = '\0';
    return true;
}

bool appendUpper_(char* out, size_t outLen, size_t& pos, char value)
{
    return appendChar_(out, outLen, pos, (char)toupper((unsigned char)value));
}

bool isSeparator_(char value)
{
    return value == '_' || value == '-';
}

bool isNumericVersion_(const char* version)
{
    if (!version || !isdigit((unsigned char)version[0])) return false;
    uint8_t dots = 0U;
    bool digitInPart = false;
    for (size_t i = 0U; version[i] != '\0'; ++i) {
        if (isdigit((unsigned char)version[i])) {
            digitInPart = true;
            continue;
        }
        if (version[i] == '.' && digitInPart && dots < 2U) {
            ++dots;
            digitInPart = false;
            continue;
        }
        return false;
    }
    return dots == 2U && digitInPart;
}

} // namespace

bool parseNextionDisplayModel(const char* model, HmiDisplayIdentity& out)
{
    out = HmiDisplayIdentity{};
    if (!model) return false;

    const size_t len = strlen(model);
    if (len < 10U || len >= sizeof(out.model)) return false;
    if (toupper((unsigned char)model[0]) != 'N' ||
        toupper((unsigned char)model[1]) != 'X') {
        return false;
    }
    for (size_t i = 2U; i <= 5U; ++i) {
        if (!isdigit((unsigned char)model[i])) return false;
    }
    if (!isalpha((unsigned char)model[6])) return false;
    for (size_t i = 7U; i <= 9U; ++i) {
        if (!isdigit((unsigned char)model[i])) return false;
    }

    size_t outPos = 0U;
    for (size_t i = 0U; i < 10U; ++i) {
        if (!appendUpper_(out.compatibility, sizeof(out.compatibility), outPos, model[i])) return false;
    }

    size_t pos = 10U;
    if (pos < len) {
        if (!isSeparator_(model[pos++])) return false;
        if (!appendChar_(out.compatibility, sizeof(out.compatibility), outPos, '_')) return false;
        for (uint8_t i = 0U; i < 3U; ++i) {
            if (pos >= len || !isdigit((unsigned char)model[pos])) return false;
            if (!appendChar_(out.compatibility, sizeof(out.compatibility), outPos, model[pos++])) return false;
        }

        if (pos < len && !isSeparator_(model[pos])) {
            const char touch = (char)toupper((unsigned char)model[pos++]);
            if (touch == 'R') {
                out.touchType = HmiDisplayTouchType::Resistive;
            } else if (touch == 'C') {
                out.touchType = HmiDisplayTouchType::Capacitive;
            } else if (touch == 'N') {
                out.touchType = HmiDisplayTouchType::None;
                if (!appendChar_(out.compatibility, sizeof(out.compatibility), outPos, 'N')) return false;
            } else {
                return false;
            }
        }

        while (pos < len) {
            if (!isSeparator_(model[pos++])) return false;
            if (pos >= len || !isalnum((unsigned char)model[pos])) return false;
            if (!appendChar_(out.compatibility, sizeof(out.compatibility), outPos, '_')) return false;
            while (pos < len && !isSeparator_(model[pos])) {
                if (!isalnum((unsigned char)model[pos])) return false;
                if (!appendUpper_(out.compatibility, sizeof(out.compatibility), outPos, model[pos++])) return false;
            }
        }
    }

    for (size_t i = 0U; i < len; ++i) {
        if (!isalnum((unsigned char)model[i]) && !isSeparator_(model[i])) return false;
        out.model[i] = (char)toupper((unsigned char)model[i]);
    }
    out.model[len] = '\0';
    return true;
}

bool parseNextionConnectResponse(const char* response, HmiDisplayIdentity& out)
{
    out = HmiDisplayIdentity{};
    if (!response) return false;

    char copy[192]{};
    const size_t len = strlen(response);
    if (len == 0U || len >= sizeof(copy)) return false;
    memcpy(copy, response, len + 1U);

    char* fields[7]{};
    size_t fieldCount = 0U;
    char* cursor = copy;
    while (fieldCount < 7U) {
        fields[fieldCount++] = cursor;
        char* comma = strchr(cursor, ',');
        if (!comma) break;
        *comma = '\0';
        cursor = comma + 1;
    }
    if (fieldCount != 7U || strncmp(fields[0], "comok ", 6U) != 0) return false;

    HmiDisplayIdentity parsed{};
    if (!parseNextionDisplayModel(fields[2], parsed)) return false;

    char* end = nullptr;
    const unsigned long firmware = strtoul(fields[3], &end, 10);
    if (!end || *end != '\0' || firmware > 65535UL) return false;
    parsed.deviceFirmwareVersion = (uint16_t)firmware;
    out = parsed;
    return true;
}

bool parseNextionArtifactFilename(const char* filename,
                                  char* compatibilityOut,
                                  size_t compatibilityOutLen,
                                  char* versionOut,
                                  size_t versionOutLen)
{
    if (!filename || !compatibilityOut || compatibilityOutLen == 0U ||
        !versionOut || versionOutLen == 0U) {
        return false;
    }
    compatibilityOut[0] = '\0';
    versionOut[0] = '\0';

    const size_t filenameLen = strlen(filename);
    const size_t prefixLen = strlen(kArtifactPrefix);
    const size_t extensionLen = strlen(kArtifactExtension);
    if (filenameLen <= prefixLen + extensionLen ||
        strncmp(filename, kArtifactPrefix, prefixLen) != 0 ||
        strcmp(filename + filenameLen - extensionLen, kArtifactExtension) != 0) {
        return false;
    }

    const char* versionSeparator = nullptr;
    for (const char* cursor = filename + prefixLen;
         cursor < filename + filenameLen - extensionLen;
         ++cursor) {
        if (*cursor == '-' && cursor[1] != '\0' && isdigit((unsigned char)cursor[1])) {
            versionSeparator = cursor;
        }
    }
    if (!versionSeparator) return false;

    const size_t modelLen = (size_t)(versionSeparator - (filename + prefixLen));
    const size_t versionLen = (size_t)((filename + filenameLen - extensionLen) - (versionSeparator + 1));
    if (modelLen == 0U || modelLen >= HMI_DISPLAY_MODEL_TEXT_MAX ||
        versionLen == 0U || versionLen >= versionOutLen) {
        return false;
    }

    char model[HMI_DISPLAY_MODEL_TEXT_MAX]{};
    memcpy(model, filename + prefixLen, modelLen);
    HmiDisplayIdentity parsed{};
    if (!parseNextionDisplayModel(model, parsed) ||
        parsed.touchType == HmiDisplayTouchType::Resistive ||
        parsed.touchType == HmiDisplayTouchType::Capacitive) {
        return false;
    }

    memcpy(versionOut, versionSeparator + 1, versionLen);
    versionOut[versionLen] = '\0';
    if (!isNumericVersion_(versionOut)) {
        versionOut[0] = '\0';
        return false;
    }
    if (strlen(parsed.compatibility) + 1U > compatibilityOutLen) return false;
    strcpy(compatibilityOut, parsed.compatibility);
    return true;
}

int compareNextionVersions(const char* left, const char* right)
{
    if (!isNumericVersion_(left) || !isNumericVersion_(right)) return 0;
    const char* l = left;
    const char* r = right;
    for (uint8_t part = 0U; part < 3U; ++part) {
        char* lEnd = nullptr;
        char* rEnd = nullptr;
        const unsigned long lValue = strtoul(l, &lEnd, 10);
        const unsigned long rValue = strtoul(r, &rEnd, 10);
        if (lValue < rValue) return -1;
        if (lValue > rValue) return 1;
        l = (*lEnd == '.') ? lEnd + 1 : lEnd;
        r = (*rEnd == '.') ? rEnd + 1 : rEnd;
    }
    return 0;
}
