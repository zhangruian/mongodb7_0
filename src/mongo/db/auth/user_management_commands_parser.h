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

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/privilege_format.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace auth {

/**
 * Parses the privileges described in "privileges" into a vector of Privilege objects.
 * Returns Status::OK() upon successfully parsing all the elements of "privileges".
 */
Status parseAndValidatePrivilegeArray(const BSONArray& privileges,
                                      PrivilegeVector* parsedPrivileges);

/**
 * Takes a BSONArray of name,db pair documents, parses that array and returns (via the
 * output param parsedRoleNames) a list of the role names in the input array.
 * Performs syntactic validation of "rolesArray", only.
 */
Status parseRoleNamesFromBSONArray(const BSONArray& rolesArray,
                                   StringData dbname,
                                   std::vector<RoleName>* parsedRoleNames);

/**
 * Takes a BSONArray of name,db pair documents, parses that array and returns (via the
 * output param parsedUserNames) a list of the usernames in the input array.
 * Performs syntactic validation of "usersArray", only.
 */
Status parseUserNamesFromBSONArray(const BSONArray& usersArray,
                                   StringData dbname,
                                   std::vector<UserName>* parsedUserNames);

struct MergeAuthzCollectionsArgs {
    std::string usersCollName;
    std::string rolesCollName;
    std::string db;
    bool drop;

    MergeAuthzCollectionsArgs() : drop(false) {}
};

/**
 * Takes a command object describing an invocation of the "_mergeAuthzCollections" command and
 * parses out the name of the temporary collections to use for user and role data, whether or
 * not to drop the existing users/roles, the database if this is a for a db-specific restore.
 * Returns ErrorCodes::OutdatedClient if the "db" field is missing, as that likely indicates
 * the command was sent by an outdated (pre 2.6.4) version of mongorestore.
 * Returns other codes indicating missing or incorrectly typed fields.
 */
Status parseMergeAuthzCollectionsCommand(const BSONObj& cmdObj,
                                         MergeAuthzCollectionsArgs* parsedArgs);

}  // namespace auth
}  // namespace mongo
