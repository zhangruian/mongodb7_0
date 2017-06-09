/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/rpc/protocol.h"

namespace mongo {
class BSONObj;
class BSONObjBuilder;
class Message;

namespace rpc {

/**
 * Constructs an RPC Reply.
 */
class ReplyBuilderInterface {
    MONGO_DISALLOW_COPYING(ReplyBuilderInterface);

public:
    virtual ~ReplyBuilderInterface() = default;

    /**
     * Sets the raw command reply. This should probably not be used in favor of the
     * variants that accept a Status or StatusWith.
     */
    virtual ReplyBuilderInterface& setRawCommandReply(const BSONObj& reply) = 0;

    /**
     * Returns a BSONObjBuilder for building a command reply in place.
     */
    virtual BSONObjBuilder getInPlaceReplyBuilder(std::size_t reserveBytes) = 0;

    virtual ReplyBuilderInterface& setMetadata(const BSONObj& metadata) = 0;

    /**
     * Sets the reply for this command. If an engaged StatusWith<BSONObj> is passed, the command
     * reply will be set to the contained BSONObj, augmented with the element {ok, 1.0} if it
     * does not already have an "ok" field. If a disengaged StatusWith<BSONObj> is passed, the
     * command reply will be set to {ok: 0.0, code: <code of status>,
     *                               codeName: <name of status code>,
     *                               errmsg: <reason of status>}
     */
    ReplyBuilderInterface& setCommandReply(StatusWith<BSONObj> commandReply);

    /**
     * Sets the reply for this command. The status parameter must be non-OK. The reply for
     * this command will be set to an object containing all the fields in extraErrorInfo,
     * augmented with {ok: 0.0} , {code: <code of status>}, {codeName: <name of status code>},
     * and {errmsg: <reason of status>}.
     * If any of the fields "ok", "code", or "errmsg" already exist in extraErrorInfo, they
     * will be left as-is in the command reply. This use of this form is intended for
     * interfacing with legacy code that adds additional data to a failed command reply and
     * its use is discouraged in new code.
     */
    virtual ReplyBuilderInterface& setCommandReply(Status nonOKStatus, BSONObj extraErrorInfo);

    /**
     * Gets the protocol used to serialize this reply. This should be used for validity checks
     * only - runtime behavior changes should be implemented with polymorphism.
     */
    virtual Protocol getProtocol() const = 0;

    /**
     * Resets the state of the builder to kMetadata and clears any data that was previously
     * written.
     */
    virtual void reset() = 0;

    /**
     * Writes data then transfers ownership of the message to the caller. The behavior of
     * calling any methods on the builder is subsequently undefined.
     */
    virtual Message done() = 0;

protected:
    ReplyBuilderInterface() = default;
};

}  // namespace rpc
}  // namespace mongo
