/*
 *    Copyright (C) 2012 10gen Inc.
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
 */

#include "mongo/db/auth/mongo_authentication_session.h"

namespace mongo {

    MongoAuthenticationSession::MongoAuthenticationSession(nonce64 nonce) :
        AuthenticationSession(AuthenticationSession::SESSION_TYPE_MONGO),
        _nonce(nonce) {
    }

    MongoAuthenticationSession::~MongoAuthenticationSession() {}

}  // namespace mongo
