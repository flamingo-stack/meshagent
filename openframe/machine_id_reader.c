/*
 * Copyright (c) 2024 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "machine_id_reader.h"

#define MAX_MACHINE_ID_LEN 128

#ifdef _WIN32
#include <windows.h>

char* read_machine_id() {
    // Read from C:\ProgramData\OpenFrame\machine_id
    char path[MAX_PATH];
    char* programData = getenv("ProgramData");
    if (!programData) {
        return NULL;
    }

    snprintf(path, sizeof(path), "%s\\OpenFrame\\machine_id", programData);

    FILE* file = fopen(path, "r");
    if (!file) {
        return NULL;
    }

    char* machineId = malloc(MAX_MACHINE_ID_LEN);
    if (!machineId) {
        fclose(file);
        return NULL;
    }

    if (fgets(machineId, MAX_MACHINE_ID_LEN, file) == NULL) {
        free(machineId);
        fclose(file);
        return NULL;
    }

    fclose(file);

    // Trim newline
    size_t len = strlen(machineId);
    while (len > 0 && (machineId[len-1] == '\n' || machineId[len-1] == '\r')) {
        machineId[--len] = '\0';
    }

    if (len == 0) {
        free(machineId);
        return NULL;
    }

    return machineId;
}

#else
// macOS / Linux

char* read_machine_id() {
#ifdef __APPLE__
    const char* path = "/Library/Application Support/OpenFrame/machine_id";
#else
    const char* path = "/var/lib/openframe/machine_id";
#endif

    FILE* file = fopen(path, "r");
    if (!file) {
        return NULL;
    }

    char* machineId = malloc(MAX_MACHINE_ID_LEN);
    if (!machineId) {
        fclose(file);
        return NULL;
    }

    if (fgets(machineId, MAX_MACHINE_ID_LEN, file) == NULL) {
        free(machineId);
        fclose(file);
        return NULL;
    }

    fclose(file);

    // Trim newline
    size_t len = strlen(machineId);
    while (len > 0 && (machineId[len-1] == '\n' || machineId[len-1] == '\r')) {
        machineId[--len] = '\0';
    }

    if (len == 0) {
        free(machineId);
        return NULL;
    }

    return machineId;
}

#endif

