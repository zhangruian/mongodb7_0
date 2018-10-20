
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

#pragma once

#include "mongo/db/repl/optime.h"

namespace mongo {

/**
 * This class allows for the storageEngine to alert the rest of the system about journaled write
 * progress.
 *
 * It has two methods. The first, getToken(), returns a token representing the current progress
 * applied to the node. It should be called just prior to making writes durable (usually, syncing a
 * journal entry to disk).
 *
 * The second method, onDurable(), takes this token as an argument and relays to the rest of the
 * system that writes through that point have been journaled. All implementations must be prepared
 * to receive default-constructed Tokens generated by NoOpJournalListener, in case they are
 * activated while a journal commit is in progress.
 */
class JournalListener {
public:
    using Token = repl::OpTime;
    virtual ~JournalListener() = default;
    virtual Token getToken() = 0;
    virtual void onDurable(const Token& token) = 0;
};

/**
 * The NoOpJournalListener is a trivial implementation of a JournalListener, that does nothing.
 * NoOpJournalListener::instance exists simply as a default implementation for storage engines to
 * use until they are passed a JournalListener with greater functionality, allowing us to avoid
 * checking for JournalListener-nullness.
 */
class NoOpJournalListener : public JournalListener {
public:
    virtual ~NoOpJournalListener() = default;
    virtual JournalListener::Token getToken() {
        return JournalListener::Token();
    }
    virtual void onDurable(const Token& token) {}
    // As this has no state, it is de facto const and can be safely shared freely.
    static NoOpJournalListener instance;
};
}
