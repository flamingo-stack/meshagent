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

#if !defined(DUK_MODULE_DUKTAPE_H_INCLUDED)
#define DUK_MODULE_DUKTAPE_H_INCLUDED

#include "duktape.h"

/* Maximum length of CommonJS module identifier to resolve.  Length includes
 * both current module ID, requested (possibly relative) module ID, and a
 * slash in between.
 */
#define  DUK_COMMONJS_MODULE_ID_LIMIT  256

extern void duk_module_duktape_init(duk_context *ctx);

#endif  /* DUK_MODULE_DUKTAPE_H_INCLUDED */

