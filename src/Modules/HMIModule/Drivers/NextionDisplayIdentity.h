#pragma once
/**
 * @file NextionDisplayIdentity.h
 * @brief Strict parsing and normalization of Nextion display identities.
 */

#include "Core/Services/IHmi.h"

#include <stddef.h>

/**
 * Parse a Nextion model identifier and build its firmware compatibility key.
 *
 * The touch marker R/C is removed from the compatibility key because both
 * variants use the same TFT image. N (no touch) remains part of the key.
 * Separators are canonicalized to underscores.
 */
bool parseNextionDisplayModel(const char* model, HmiDisplayIdentity& out);

/** Parse a complete `comok` response returned by the Nextion `connect` command. */
bool parseNextionConnectResponse(const char* response, HmiDisplayIdentity& out);

/**
 * Parse the canonical Flow.io Nextion artifact filename.
 *
 * Expected form: FlowIO_Nextion_<compatibility>-<version>.tft
 */
bool parseNextionArtifactFilename(const char* filename,
                                  char* compatibilityOut,
                                  size_t compatibilityOutLen,
                                  char* versionOut,
                                  size_t versionOutLen);

/** Compare dotted numeric HMI versions. Returns -1, 0 or 1. */
int compareNextionVersions(const char* left, const char* right);
