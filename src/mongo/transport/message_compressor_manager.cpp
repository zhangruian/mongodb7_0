/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/transport/message_compressor_manager.h"

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/transport/session.h"

namespace mongo {
namespace {

// TODO(JBR): This should be changed so it 's closer to the MSGHEADER View/ConstView classes
// than this little struct.
struct CompressionHeader {
    int32_t originalOpCode;
    int32_t uncompressedSize;
    uint8_t compressorId;

    void serialize(DataRangeCursor* cursor) {
        cursor->writeAndAdvance<LittleEndian<int32_t>>(originalOpCode);
        cursor->writeAndAdvance<LittleEndian<int32_t>>(uncompressedSize);
        cursor->writeAndAdvance<LittleEndian<uint8_t>>(compressorId);
    }

    CompressionHeader(int32_t _opcode, int32_t _size, uint8_t _id)
        : originalOpCode{_opcode}, uncompressedSize{_size}, compressorId{_id} {}

    CompressionHeader(ConstDataRangeCursor* cursor) {
        originalOpCode = cursor->readAndAdvance<LittleEndian<std::int32_t>>();
        uncompressedSize = cursor->readAndAdvance<LittleEndian<std::int32_t>>();
        compressorId = cursor->readAndAdvance<LittleEndian<uint8_t>>();
    }

    static size_t size() {
        return sizeof(originalOpCode) + sizeof(uncompressedSize) + sizeof(compressorId);
    }
};

const transport::Session::Decoration<MessageCompressorManager> getForSession =
    transport::Session::declareDecoration<MessageCompressorManager>();
}  // namespace

MessageCompressorManager::MessageCompressorManager()
    : MessageCompressorManager(&MessageCompressorRegistry::get()) {}

MessageCompressorManager::MessageCompressorManager(MessageCompressorRegistry* factory)
    : _registry{factory} {}

StatusWith<Message> MessageCompressorManager::compressMessage(
    const Message& msg, const MessageCompressorId* compressorId) {

    MessageCompressorBase* compressor = nullptr;
    if (compressorId) {
        compressor = _registry->getCompressor(*compressorId);
        invariant(compressor);
    } else if (!_negotiated.empty()) {
        compressor = _negotiated[0];
    } else {
        return {msg};
    }

    LOGV2_DEBUG(22925,
                3,
                "Compressing message with {compressor}",
                "Compressing message",
                "compressor"_attr = compressor->getName());

    auto inputHeader = msg.header();
    size_t bufferSize = compressor->getMaxCompressedSize(msg.dataSize()) +
        CompressionHeader::size() + MsgData::MsgDataHeaderSize;

    CompressionHeader compressionHeader(
        inputHeader.getNetworkOp(), inputHeader.dataLen(), compressor->getId());

    if (bufferSize > MaxMessageSizeBytes) {
        LOGV2_DEBUG(22926,
                    3,
                    "Compressed message would be larger than {MaxMessageSizeBytes}, returning "
                    "original uncompressed message",
                    "Compressed message would be larger than maximum allowed, returning original "
                    "uncompressed message",
                    "MaxMessageSizeBytes"_attr = MaxMessageSizeBytes);
        return {msg};
    }

    auto outputMessageBuffer = SharedBuffer::allocate(bufferSize);

    MsgData::View outMessage(outputMessageBuffer.get());
    outMessage.setId(inputHeader.getId());
    outMessage.setResponseToMsgId(inputHeader.getResponseToMsgId());
    outMessage.setOperation(dbCompressed);
    outMessage.setLen(bufferSize);

    DataRangeCursor output(outMessage.data(), outMessage.data() + outMessage.dataLen());
    compressionHeader.serialize(&output);
    ConstDataRange input(inputHeader.data(), inputHeader.data() + inputHeader.dataLen());

    auto sws = compressor->compressData(input, output);

    if (!sws.isOK())
        return sws.getStatus();

    auto realCompressedSize = sws.getValue();
    outMessage.setLen(realCompressedSize + CompressionHeader::size() + MsgData::MsgDataHeaderSize);

    return {Message(outputMessageBuffer)};
}

StatusWith<Message> MessageCompressorManager::decompressMessage(const Message& msg,
                                                                MessageCompressorId* compressorId) {
    auto inputHeader = msg.header();
    ConstDataRangeCursor input(inputHeader.data(), inputHeader.data() + inputHeader.dataLen());
    if (input.length() < CompressionHeader::size()) {
        return {ErrorCodes::BadValue, "Invalid compressed message header"};
    }
    CompressionHeader compressionHeader(&input);

    auto compressor = _registry->getCompressor(compressionHeader.compressorId);
    if (!compressor) {
        return {ErrorCodes::InternalError,
                "Compression algorithm specified in message is not available"};
    }

    if (compressorId) {
        *compressorId = compressor->getId();
    }

    LOGV2_DEBUG(22927,
                3,
                "Decompressing message with {compressor}",
                "Decompressing message",
                "compressor"_attr = compressor->getName());

    if (compressionHeader.uncompressedSize < 0) {
        return {ErrorCodes::BadValue, "Decompressed message would be negative in size"};
    }

    // Explicitly promote `uncompressedSize` to a 64-bit integer before addition in order to
    // avoid potential overflow.
    size_t bufferSize =
        static_cast<size_t>(compressionHeader.uncompressedSize) + MsgData::MsgDataHeaderSize;
    if (bufferSize > MaxMessageSizeBytes) {
        return {ErrorCodes::BadValue,
                "Decompressed message would be larger than maximum message size"};
    }

    auto outputMessageBuffer = SharedBuffer::allocate(bufferSize);
    MsgData::View outMessage(outputMessageBuffer.get());
    outMessage.setId(inputHeader.getId());
    outMessage.setResponseToMsgId(inputHeader.getResponseToMsgId());
    outMessage.setOperation(compressionHeader.originalOpCode);
    outMessage.setLen(bufferSize);

    DataRangeCursor output(outMessage.data(), outMessage.data() + outMessage.dataLen());

    auto sws = compressor->decompressData(input, output);

    if (!sws.isOK())
        return sws.getStatus();

    if (sws.getValue() != static_cast<std::size_t>(compressionHeader.uncompressedSize)) {
        return {ErrorCodes::BadValue, "Decompressing message returned less data than expected"};
    }

    outMessage.setLen(sws.getValue() + MsgData::MsgDataHeaderSize);

    return {Message(outputMessageBuffer)};
}

void MessageCompressorManager::clientBegin(BSONObjBuilder* output) {
    LOGV2_DEBUG(22928, 3, "Starting client-side compression negotiation");

    // We're about to update the compressor list with the negotiation result from the server.
    _negotiated.clear();

    auto& compressorList = _registry->getCompressorNames();
    if (compressorList.size() == 0)
        return;

    BSONArrayBuilder sub(output->subarrayStart("compression"));
    for (const auto& e : _registry->getCompressorNames()) {
        LOGV2_DEBUG(22929,
                    3,
                    "Offering {compressor} compressor to server",
                    "Offering compressor to server",
                    "compressor"_attr = e);
        sub.append(e);
    }
    sub.doneFast();
}

void MessageCompressorManager::clientFinish(const BSONObj& input) {
    auto elem = input.getField("compression");
    LOGV2_DEBUG(22930, 3, "Finishing client-side compression negotiation");

    // We've just called clientBegin, so the list of compressors should be empty.
    invariant(_negotiated.empty());

    // If the server didn't send back a "compression" array, then we assume compression is not
    // supported by this server and just return. We've already disabled compression by clearing
    // out the _negotiated array above.
    if (elem.eoo()) {
        LOGV2_DEBUG(22931,
                    3,
                    "No compression algorithms were sent from the server. This connection will be "
                    "uncompressed");
        return;
    }

    LOGV2_DEBUG(22932, 3, "Received message compressors from server");
    for (const auto& e : elem.Obj()) {
        auto algoName = e.checkAndGetStringData();
        auto ret = _registry->getCompressor(algoName);
        LOGV2_DEBUG(22933,
                    3,
                    "Adding compressor {compressor}",
                    "Adding compressor",
                    "compressor"_attr = ret->getName());
        _negotiated.push_back(ret);
    }
}

void MessageCompressorManager::serverNegotiate(const BSONObj& input, BSONObjBuilder* output) {
    LOGV2_DEBUG(22934, 3, "Starting server-side compression negotiation");

    auto elem = input.getField("compression");
    // If the "compression" field is missing, then this isMaster request is requesting information
    // rather than doing a negotiation
    if (elem.eoo()) {
        // If we haven't negotiated any compressors yet, then don't append anything to the
        // output - this makes this compatible with older versions of MongoDB that don't
        // support compression.
        if (_negotiated.size() > 0) {
            BSONArrayBuilder sub(output->subarrayStart("compression"));
            for (const auto& algo : _negotiated) {
                sub.append(algo->getName());
            }
            sub.doneFast();
        } else {
            LOGV2_DEBUG(22935, 3, "Compression negotiation not requested by client");
        }
        return;
    }

    // If compression has already been negotiated, then this is a renegotiation, so we should
    // reset the state of the manager.
    _negotiated.clear();

    // First we go through all the compressor names that the client has requested support for
    BSONObj theirObj = elem.Obj();

    if (!theirObj.nFields()) {
        LOGV2_DEBUG(22936, 3, "No compressors provided");
        return;
    }

    for (const auto& elem : theirObj) {
        MessageCompressorBase* cur;
        auto curName = elem.checkAndGetStringData();
        // If the MessageCompressorRegistry knows about a compressor with that name, then it is
        // valid and we add it to our list of negotiated compressors.
        if ((cur = _registry->getCompressor(curName))) {
            LOGV2_DEBUG(22937,
                        3,
                        "{compressor} is supported",
                        "supported compressor",
                        "compressor"_attr = cur->getName());
            _negotiated.push_back(cur);
        } else {  // Otherwise the compressor is not supported and we skip over it.
            LOGV2_DEBUG(22938,
                        3,
                        "{compressor} is not supported",
                        "compressor is not supported",
                        "compressor"_attr = curName);
        }
    }

    // If the number of compressors that were eventually negotiated is greater than 0, then
    // we should send that back to the client.
    if (_negotiated.size() > 0) {
        BSONArrayBuilder sub(output->subarrayStart("compression"));
        for (auto algo : _negotiated) {
            sub.append(algo->getName());
        }
        sub.doneFast();
    } else {
        LOGV2_DEBUG(22939, 3, "Could not agree on compressor to use");
    }
}

MessageCompressorManager& MessageCompressorManager::forSession(
    const transport::SessionHandle& session) {
    return getForSession(session.get());
}

}  // namespace mongo
