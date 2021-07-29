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

#include <vector>

#include "mongo/bson/mutable/element.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/resource_pattern.h"

namespace mongo {

class Privilege;
typedef std::vector<Privilege> PrivilegeVector;

/**
 * A representation of the permission to perform a set of actions on a resource.
 */
class Privilege {
public:
    /**
     * Adds "privilegeToAdd" to "privileges", de-duping "privilegeToAdd" if the vector already
     * contains a privilege on the same resource.
     *
     * This method is the preferred way to add privileges to  privilege vectors.
     */
    static void addPrivilegeToPrivilegeVector(PrivilegeVector* privileges,
                                              const Privilege& privilegeToAdd);

    static void addPrivilegesToPrivilegeVector(PrivilegeVector* privileges,
                                               const PrivilegeVector& privilegesToAdd);

    /**
     * Takes a vector of privileges and fills the output param "resultArray" with a BSON array
     * representation of the privileges.
     */
    static Status getBSONForPrivileges(const PrivilegeVector& privileges,
                                       mutablebson::Element resultArray);


    Privilege() = default;
    ~Privilege() = default;

    Privilege(const Privilege&) = default;
    Privilege& operator=(const Privilege&) = default;

    Privilege(Privilege&&) = default;
    Privilege& operator=(Privilege&&) = default;

    Privilege(const ResourcePattern& resource, ActionType action);
    Privilege(const ResourcePattern& resource, const ActionSet& actions);

    const ResourcePattern& getResourcePattern() const {
        return _resource;
    }

    const ActionSet& getActions() const {
        return _actions;
    }

    void addActions(const ActionSet& actionsToAdd);
    void removeActions(const ActionSet& actionsToRemove);

    // Checks if the given action is present in the Privilege.
    bool includesAction(ActionType action) const;
    // Checks if the given actions are present in the Privilege.
    bool includesActions(const ActionSet& actions) const;

    static Privilege fromBSON(BSONElement obj);
    static Privilege fromBSON(BSONObj obj);
    BSONObj toBSON() const;

private:
    ResourcePattern _resource;
    ActionSet _actions;  // bitmask of actions this privilege grants
};

}  // namespace mongo
