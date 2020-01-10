/*
* Tencent is pleased to support the open source community by making
* MMKV available.
*
* Copyright (C) 2020 THL A29 Limited, a Tencent company.
* All rights reserved.
*
* Licensed under the BSD 3-Clause License (the "License"); you may not use
* this file except in compliance with the License. You may obtain a copy of
* the License at
*
*       https://opensource.org/licenses/BSD-3-Clause
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "Checksum.h"

#ifdef __aarch64__

__attribute__((target("crc"))) static inline uint32_t __crc32b(uint32_t a, uint8_t b) {
    return __builtin_arm_crc32b(a, b);
}

__attribute__((target("crc"))) static inline uint32_t __crc32h(uint32_t a, uint16_t b) {
    return __builtin_arm_crc32h(a, b);
}

__attribute__((target("crc"))) static inline uint32_t __crc32w(uint32_t a, uint32_t b) {
    return __builtin_arm_crc32w(a, b);
}

__attribute__((target("crc"))) static inline uint32_t __crc32d(uint32_t a, uint64_t b) {
    return __builtin_arm_crc32d(a, b);
}

namespace mmkv {

__attribute__((target("crc"))) static inline uint32_t
armv8_crc32_small(uint32_t crc, const unsigned char *buf, size_t len) {
    if (len >= sizeof(uint32_t)) {
        crc = __crc32w(crc, *(const uint32_t *) buf);
        buf += sizeof(uint32_t);
        len -= sizeof(uint32_t);
    }
    if (len >= sizeof(uint16_t)) {
        crc = __crc32h(crc, *(const uint16_t *) buf);
        buf += sizeof(uint16_t);
        len -= sizeof(uint16_t);
    }
    if (len >= sizeof(uint8_t)) {
        crc = __crc32b(crc, *(const uint8_t *) buf);
    }

    return crc;
}

__attribute__((target("crc"))) uint32_t armv8_crc32(uint32_t crc, const unsigned char *buf, size_t len) {

    crc = crc ^ 0xffffffffUL;

    // roundup to 8 byte pointer
    auto offset = std::min(len, (uintptr_t) buf & 7);
    if (offset) {
        crc = armv8_crc32_small(crc, buf, offset);
        buf += offset;
        len -= offset;
    }
    if (!len) {
        return crc ^ 0xffffffffUL;
    }

    // unroll to 8 * 8 byte per loop
    auto ptr64 = (const uint64_t *) buf;
    for (constexpr auto step = 8 * sizeof(uint64_t); len >= step; len -= step) {
        crc = __crc32d(crc, *ptr64++);
        crc = __crc32d(crc, *ptr64++);
        crc = __crc32d(crc, *ptr64++);
        crc = __crc32d(crc, *ptr64++);
        crc = __crc32d(crc, *ptr64++);
        crc = __crc32d(crc, *ptr64++);
        crc = __crc32d(crc, *ptr64++);
        crc = __crc32d(crc, *ptr64++);
    }

    for (constexpr auto step = sizeof(uint64_t); len >= step; len -= step) {
        crc = __crc32d(crc, *ptr64++);
    }

    if (len) {
        crc = armv8_crc32_small(crc, (const unsigned char *) ptr64, len);
    }

    return crc ^ 0xffffffffUL;
}

} // namespace mmkv

#endif // __aarch64__
