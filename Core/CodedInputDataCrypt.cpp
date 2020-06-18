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

#include "CodedInputDataCrypt.h"
#include "PBUtility.h"
#include <cerrno>
#include <stdexcept>

#ifdef MMKV_APPLE
#    if __has_feature(objc_arc)
#        error This file must be compiled with MRC. Use -fno-objc-arc flag.
#    endif
#endif // MMKV_APPLE

using namespace std;

namespace mmkv {

CodedInputDataCrypt::CodedInputDataCrypt(const void *oData, size_t length, AESCrypt &crypt)
    : m_ptr((uint8_t *) oData), m_size(length), m_position(0), m_decryptPosition(0), m_decrypter(crypt) {
    m_decryptBufferSize = AES_KEY_LEN * 2;
    m_decryptBufferPosition = 0;
    m_decryptBufferDiscardPosition = 0;
    m_decryptBufferDecryptPosition = 0;
    m_decryptBuffer = (uint8_t *) malloc(m_decryptBufferSize);
    if (!m_decryptBuffer) {
        throw runtime_error(strerror(errno));
    }
}

CodedInputDataCrypt::~CodedInputDataCrypt() {
    if (m_decryptBuffer) {
        free(m_decryptBuffer);
    }
}

void CodedInputDataCrypt::seek(size_t addedSize) {
    m_position += addedSize;

    if (m_position > m_size) {
        throw out_of_range("OutOfSpace");
    }
}

void CodedInputDataCrypt::consumeBytes(size_t length, bool discardPreData) {
    if (discardPreData) {
        m_decryptBufferDiscardPosition = m_decryptBufferPosition;
    }
    auto decryptedBytesLeft = m_decryptBufferDecryptPosition - m_decryptBufferPosition;
    if (decryptedBytesLeft >= length) {
        return;
    }
    length -= decryptedBytesLeft;

    length = ((length + AES_KEY_LEN - 1) / AES_KEY_LEN) * AES_KEY_LEN;
    auto bytesLeftInSrc = m_size - m_decryptPosition;
    length = min(bytesLeftInSrc, length);

    auto bytesLeftInBuffer = m_decryptBufferSize - m_decryptBufferDecryptPosition;
    // try move some space
    if (bytesLeftInBuffer < length && m_decryptBufferDiscardPosition > 0) {
        auto posToMove = (m_decryptBufferDiscardPosition / AES_KEY_LEN) * AES_KEY_LEN;
        if (posToMove) {
            auto sizeToMove = m_decryptBufferDecryptPosition - posToMove;
            memmove(m_decryptBuffer, m_decryptBuffer + posToMove, sizeToMove);
            m_decryptBufferPosition -= posToMove;
            m_decryptBufferDecryptPosition -= posToMove;
            m_decryptBufferDiscardPosition = 0;
            bytesLeftInBuffer = m_decryptBufferSize - m_decryptBufferDecryptPosition;
        }
    }
    // still not enough sapce, try realloc
    if (bytesLeftInBuffer < length) {
        auto newSize = m_decryptBufferSize + length;
        auto newBuffer = realloc(m_decryptBuffer, newSize);
        if (!newBuffer) {
            throw runtime_error(strerror(errno));
        }
        m_decryptBuffer = (uint8_t *) newBuffer;
        m_decryptBufferSize = newSize;
    }
    m_decrypter.decrypt(m_ptr + m_decryptPosition, m_decryptBuffer + m_decryptBufferDecryptPosition, length);
    m_decryptPosition += length;
    m_decryptBufferDecryptPosition += length;
}

void CodedInputDataCrypt::skipBytes(size_t length) {
    auto decryptedBytesLeft = m_decryptBufferDecryptPosition - m_decryptBufferPosition;
    if (decryptedBytesLeft >= length) {
        m_decryptBufferPosition += length;
        return;
    }
    length -= decryptedBytesLeft;

    for (size_t round = 0, total = (length + AES_KEY_LEN - 1) / AES_KEY_LEN; round < total; round++) {
        m_decrypter.decrypt(m_ptr + m_decryptPosition, m_decryptBuffer, AES_KEY_LEN);
        m_decryptPosition += AES_KEY_LEN;
    }
    m_decryptBufferPosition = length % AES_KEY_LEN;
    m_decryptBufferDecryptPosition = AES_KEY_LEN;
}

inline void CodedInputDataCrypt::statusBeforeDecrypt(size_t rollbackSize, AESCryptStatus &status) {
    rollbackSize += m_decryptBufferDecryptPosition - m_decryptBufferPosition;
    m_decrypter.statusBeforeDecrypt(m_ptr + m_decryptPosition, m_decryptBuffer + m_decryptBufferDecryptPosition,
                                    rollbackSize, status);
}

int8_t CodedInputDataCrypt::readRawByte() {
    if (m_position == m_size) {
        auto msg = "reach end, m_position: " + to_string(m_position) + ", m_size: " + to_string(m_size);
        throw out_of_range(msg);
    }
    m_position++;

    assert(m_decryptBufferPosition < m_decryptBufferSize);
    auto *bytes = (int8_t *) m_decryptBuffer;
    return bytes[m_decryptBufferPosition++];
}

int32_t CodedInputDataCrypt::readRawVarint32(bool discardPreData) {
    consumeBytes(10, discardPreData);

    int8_t tmp = this->readRawByte();
    if (tmp >= 0) {
        return tmp;
    }
    int32_t result = tmp & 0x7f;
    if ((tmp = this->readRawByte()) >= 0) {
        result |= tmp << 7;
    } else {
        result |= (tmp & 0x7f) << 7;
        if ((tmp = this->readRawByte()) >= 0) {
            result |= tmp << 14;
        } else {
            result |= (tmp & 0x7f) << 14;
            if ((tmp = this->readRawByte()) >= 0) {
                result |= tmp << 21;
            } else {
                result |= (tmp & 0x7f) << 21;
                result |= (tmp = this->readRawByte()) << 28;
                if (tmp < 0) {
                    // discard upper 32 bits
                    for (int i = 0; i < 5; i++) {
                        if (this->readRawByte() >= 0) {
                            return result;
                        }
                    }
                    throw invalid_argument("InvalidProtocolBuffer malformed varint32");
                }
            }
        }
    }
    return result;
}

int32_t CodedInputDataCrypt::readRawLittleEndian32() {
    consumeBytes(4);

    int8_t b1 = this->readRawByte();
    int8_t b2 = this->readRawByte();
    int8_t b3 = this->readRawByte();
    int8_t b4 = this->readRawByte();
    return (((int32_t) b1 & 0xff)) | (((int32_t) b2 & 0xff) << 8) | (((int32_t) b3 & 0xff) << 16) |
           (((int32_t) b4 & 0xff) << 24);
}

int64_t CodedInputDataCrypt::readRawLittleEndian64() {
    consumeBytes(8);

    int8_t b1 = this->readRawByte();
    int8_t b2 = this->readRawByte();
    int8_t b3 = this->readRawByte();
    int8_t b4 = this->readRawByte();
    int8_t b5 = this->readRawByte();
    int8_t b6 = this->readRawByte();
    int8_t b7 = this->readRawByte();
    int8_t b8 = this->readRawByte();
    return (((int64_t) b1 & 0xff)) | (((int64_t) b2 & 0xff) << 8) | (((int64_t) b3 & 0xff) << 16) |
           (((int64_t) b4 & 0xff) << 24) | (((int64_t) b5 & 0xff) << 32) | (((int64_t) b6 & 0xff) << 40) |
           (((int64_t) b7 & 0xff) << 48) | (((int64_t) b8 & 0xff) << 56);
}

double CodedInputDataCrypt::readDouble() {
    return Int64ToFloat64(this->readRawLittleEndian64());
}

float CodedInputDataCrypt::readFloat() {
    return Int32ToFloat32(this->readRawLittleEndian32());
}

int64_t CodedInputDataCrypt::readInt64() {
    consumeBytes(10);

    int32_t shift = 0;
    int64_t result = 0;
    while (shift < 64) {
        int8_t b = this->readRawByte();
        result |= (int64_t)(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
            return result;
        }
        shift += 7;
    }
    throw invalid_argument("InvalidProtocolBuffer malformedInt64");
}

uint64_t CodedInputDataCrypt::readUInt64() {
    return static_cast<uint64_t>(readInt64());
}

int32_t CodedInputDataCrypt::readInt32() {
    return this->readRawVarint32();
}

uint32_t CodedInputDataCrypt::readUInt32() {
    return static_cast<uint32_t>(readRawVarint32());
}

int32_t CodedInputDataCrypt::readFixed32() {
    return this->readRawLittleEndian32();
}

bool CodedInputDataCrypt::readBool() {
    return this->readRawVarint32() != 0;
}

#ifndef MMKV_APPLE

string CodedInputDataCrypt::readString() {
    int32_t size = readRawVarint32();
    if (size < 0) {
        throw length_error("InvalidProtocolBuffer negativeSize");
    }

    auto s_size = static_cast<size_t>(size);
    if (s_size <= m_size - m_position) {
        consumeBytes(s_size);

        string result((char *) (m_decryptBuffer + m_decryptBufferPosition), s_size);
        m_position += s_size;
        m_decryptBufferPosition += s_size;
        return result;
    } else {
        throw out_of_range("InvalidProtocolBuffer truncatedMessage");
    }
}

string CodedInputDataCrypt::readString(KeyValueHolderCrypt &kvHolder) {
    kvHolder.offset = static_cast<uint32_t>(m_position);

    int32_t size = this->readRawVarint32(true);
    if (size < 0) {
        throw length_error("InvalidProtocolBuffer negativeSize");
    }

    auto s_size = static_cast<size_t>(size);
    if (s_size <= m_size - m_position) {
        consumeBytes(s_size);

        kvHolder.keySize = static_cast<uint16_t>(s_size);

        string result((char *) (m_decryptBuffer + m_decryptBufferPosition), s_size);
        m_position += s_size;
        m_decryptBufferPosition += s_size;
        return result;
    } else {
        throw out_of_range("InvalidProtocolBuffer truncatedMessage");
    }
}

#endif

MMBuffer CodedInputDataCrypt::readData() {
    int32_t size = this->readRawVarint32();
    if (size < 0) {
        throw length_error("InvalidProtocolBuffer negativeSize");
    }

    auto s_size = static_cast<size_t>(size);
    if (s_size <= m_size - m_position) {
        consumeBytes(s_size);

        MMBuffer data(m_decryptBuffer + m_decryptBufferPosition, s_size);
        m_position += s_size;
        m_decryptBufferPosition += s_size;
        return data;
    } else {
        throw out_of_range("InvalidProtocolBuffer truncatedMessage");
    }
}

void CodedInputDataCrypt::readData(KeyValueHolderCrypt &kvHolder) {
    int32_t size = this->readRawVarint32();
    if (size < 0) {
        throw length_error("InvalidProtocolBuffer negativeSize");
    }

    auto s_size = static_cast<size_t>(size);
    if (s_size <= m_size - m_position) {
        if (s_size > sizeof(kvHolder) * 2) {
            kvHolder.type = KeyValueHolderType_Offset;
            kvHolder.valueSize = static_cast<uint32_t>(s_size);
            kvHolder.pbKeyValueSize =
                static_cast<uint8_t>(pbRawVarint32Size(kvHolder.valueSize) + pbRawVarint32Size(kvHolder.keySize));

            size_t rollbackSize = kvHolder.pbKeyValueSize + kvHolder.keySize;
            statusBeforeDecrypt(rollbackSize, *kvHolder.cryptStatus());

            skipBytes(s_size);
            m_position += s_size;
        } else {
            consumeBytes(s_size);

            kvHolder.type = KeyValueHolderType_Direct;
            kvHolder = KeyValueHolderCrypt(m_decryptBuffer + m_decryptBufferPosition, s_size);
            m_position += s_size;
            m_decryptBufferPosition += s_size;
        }
    } else {
        throw out_of_range("InvalidProtocolBuffer truncatedMessage");
    }
}

} // namespace mmkv