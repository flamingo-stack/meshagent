/*
 * Copyright (C) 2024 Intel Corporation
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
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MAC_LOGGING_UTILS_H
#define MAC_LOGGING_UTILS_H

/**
 * Shared logging utility for macOS components
 *
 * Logs messages to both stderr and a log file for troubleshooting
 * installation and upgrade issues.
 */

#define MESH_LOG_FILE "/tmp/meshagent-install-ui.log"

/**
 * Log a message to both stderr and the log file
 *
 * @param format printf-style format string
 * @param ... variable arguments for format string
 */
void mesh_log_message(const char* format, ...);

#endif // MAC_LOGGING_UTILS_H

