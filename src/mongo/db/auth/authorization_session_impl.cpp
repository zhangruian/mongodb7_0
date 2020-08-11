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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session_impl.h"

#include <string>
#include <vector>

#include "mongo/base/shim.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authz_session_external_state.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/restriction_environment.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

namespace dps = ::mongo::dotted_path_support;
using std::vector;

namespace {

std::unique_ptr<AuthorizationSession> authorizationSessionCreateImpl(
    AuthorizationManager* authzManager) {
    return std::make_unique<AuthorizationSessionImpl>(
        AuthzSessionExternalState::create(authzManager),
        AuthorizationSessionImpl::InstallMockForTestingOrAuthImpl{});
}

auto authorizationSessionCreateRegistration =
    MONGO_WEAK_FUNCTION_REGISTRATION(AuthorizationSession::create, authorizationSessionCreateImpl);

constexpr StringData ADMIN_DBNAME = "admin"_sd;

// Checks if this connection has the privileges necessary to create or modify the view 'viewNs'
// to be a view on 'viewOnNs' with pipeline 'viewPipeline'. Call this function after verifying
// that the user has the 'createCollection' or 'collMod' action, respectively.
Status checkAuthForCreateOrModifyView(AuthorizationSession* authzSession,
                                      const NamespaceString& viewNs,
                                      const NamespaceString& viewOnNs,
                                      const BSONArray& viewPipeline,
                                      bool isMongos) {
    // It's safe to allow a user to create or modify a view if they can't read it anyway.
    if (!authzSession->isAuthorizedForActionsOnNamespace(viewNs, ActionType::find)) {
        return Status::OK();
    }

    auto status = AggregationRequest::parseFromBSON(viewNs,
                                                    BSON("aggregate" << viewOnNs.coll()
                                                                     << "pipeline" << viewPipeline
                                                                     << "cursor" << BSONObj()));
    if (!status.isOK())
        return status.getStatus();

    auto statusWithPrivs =
        authzSession->getPrivilegesForAggregate(viewOnNs, status.getValue(), isMongos);
    PrivilegeVector privileges = uassertStatusOK(statusWithPrivs);
    if (!authzSession->isAuthorizedForPrivileges(privileges)) {
        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }
    return Status::OK();
}

}  // namespace

AuthorizationSessionImpl::AuthorizationSessionImpl(
    std::unique_ptr<AuthzSessionExternalState> externalState, InstallMockForTestingOrAuthImpl)
    : _externalState(std::move(externalState)), _impersonationFlag(false) {}

AuthorizationSessionImpl::~AuthorizationSessionImpl() {}

AuthorizationManager& AuthorizationSessionImpl::getAuthorizationManager() {
    return _externalState->getAuthorizationManager();
}

void AuthorizationSessionImpl::startRequest(OperationContext* opCtx) {
    _externalState->startRequest(opCtx);
    _refreshUserInfoAsNeeded(opCtx);
}

Status AuthorizationSessionImpl::addAndAuthorizeUser(OperationContext* opCtx,
                                                     const UserName& userName) {
    AuthorizationManager* authzManager = AuthorizationManager::get(opCtx->getServiceContext());
    auto swUser = authzManager->acquireUser(opCtx, userName);
    if (!swUser.isOK()) {
        return swUser.getStatus();
    }

    auto user = std::move(swUser.getValue());

    auto restrictionStatus = user->validateRestrictions(opCtx);
    if (!restrictionStatus.isOK()) {
        LOGV2(20240,
              "Failed to acquire user because of unmet authentication restrictions",
              "user"_attr = userName,
              "reason"_attr = restrictionStatus.reason());
        return AuthorizationManager::authenticationFailedStatus;
    }

    stdx::lock_guard<Client> lk(*opCtx->getClient());
    _authenticatedUsers.add(std::move(user));

    // If there are any users and roles in the impersonation data, clear it out.
    clearImpersonatedUserData();

    _buildAuthenticatedRolesVector();
    return Status::OK();
}

User* AuthorizationSessionImpl::lookupUser(const UserName& name) {
    auto user = _authenticatedUsers.lookup(name);
    return user ? user.get() : nullptr;
}

User* AuthorizationSessionImpl::getSingleUser() {
    UserName userName;

    auto userNameItr = getAuthenticatedUserNames();
    if (userNameItr.more()) {
        userName = userNameItr.next();
        if (userNameItr.more()) {
            uasserted(ErrorCodes::Unauthorized, "too many users are authenticated");
        }
    } else {
        uasserted(ErrorCodes::Unauthorized, "there are no users authenticated");
    }

    return lookupUser(userName);
}

void AuthorizationSessionImpl::logoutDatabase(OperationContext* opCtx, StringData dbname) {
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    _authenticatedUsers.removeByDBName(dbname);
    clearImpersonatedUserData();
    _buildAuthenticatedRolesVector();
}

UserNameIterator AuthorizationSessionImpl::getAuthenticatedUserNames() {
    return _authenticatedUsers.getNames();
}

RoleNameIterator AuthorizationSessionImpl::getAuthenticatedRoleNames() {
    return makeRoleNameIterator(_authenticatedRoleNames.begin(), _authenticatedRoleNames.end());
}

void AuthorizationSessionImpl::grantInternalAuthorization(Client* client) {
    stdx::lock_guard<Client> lk(*client);
    _authenticatedUsers.add(internalSecurity.user);
    _buildAuthenticatedRolesVector();
}

/**
 * Overloaded function - takes in the opCtx of the current AuthSession
 * and calls the function above.
 */
void AuthorizationSessionImpl::grantInternalAuthorization(OperationContext* opCtx) {
    grantInternalAuthorization(opCtx->getClient());
}

PrivilegeVector AuthorizationSessionImpl::getDefaultPrivileges() {
    PrivilegeVector defaultPrivileges;

    // If localhost exception is active (and no users exist),
    // return a vector of the minimum privileges required to bootstrap
    // a system and add the first user.
    if (_externalState->shouldAllowLocalhost()) {
        ResourcePattern adminDBResource = ResourcePattern::forDatabaseName(ADMIN_DBNAME);
        ActionSet setupAdminUserActionSet;
        setupAdminUserActionSet.addAction(ActionType::createUser);
        setupAdminUserActionSet.addAction(ActionType::grantRole);
        Privilege setupAdminUserPrivilege = Privilege(adminDBResource, setupAdminUserActionSet);

        ResourcePattern externalDBResource = ResourcePattern::forDatabaseName("$external");
        Privilege setupExternalUserPrivilege =
            Privilege(externalDBResource, ActionType::createUser);

        ActionSet setupServerConfigActionSet;

        // If this server is an arbiter, add specific privileges meant to circumvent
        // the behavior of an arbiter in an authenticated replset. See SERVER-5479.
        if (_externalState->serverIsArbiter()) {
            setupServerConfigActionSet.addAction(ActionType::getCmdLineOpts);
            setupServerConfigActionSet.addAction(ActionType::getParameter);
            setupServerConfigActionSet.addAction(ActionType::serverStatus);
            setupServerConfigActionSet.addAction(ActionType::shutdown);
        }

        setupServerConfigActionSet.addAction(ActionType::addShard);
        setupServerConfigActionSet.addAction(ActionType::replSetConfigure);
        setupServerConfigActionSet.addAction(ActionType::replSetGetStatus);
        Privilege setupServerConfigPrivilege =
            Privilege(ResourcePattern::forClusterResource(), setupServerConfigActionSet);

        Privilege::addPrivilegeToPrivilegeVector(&defaultPrivileges, setupAdminUserPrivilege);
        Privilege::addPrivilegeToPrivilegeVector(&defaultPrivileges, setupExternalUserPrivilege);
        Privilege::addPrivilegeToPrivilegeVector(&defaultPrivileges, setupServerConfigPrivilege);
        return defaultPrivileges;
    }

    return defaultPrivileges;
}

StatusWith<PrivilegeVector> AuthorizationSessionImpl::getPrivilegesForAggregate(
    const NamespaceString& nss, const AggregationRequest& request, bool isMongos) {
    if (!nss.isValid()) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "Invalid input namespace, " << nss.ns());
    }

    PrivilegeVector privileges;

    // If this connection does not need to be authenticated (for instance, if auth is disabled),
    // returns an empty requirements set.
    if (_externalState->shouldIgnoreAuthChecks()) {
        return privileges;
    }

    const auto& pipeline = request.getPipeline();

    // If the aggregation pipeline is empty, confirm the user is authorized for find on 'nss'.
    if (pipeline.empty()) {
        Privilege currentPriv =
            Privilege(ResourcePattern::forExactNamespace(nss), ActionType::find);
        Privilege::addPrivilegeToPrivilegeVector(&privileges, currentPriv);
        return privileges;
    }

    // If the first stage of the pipeline is not an initial source, the pipeline is implicitly
    // reading documents from the underlying collection. The client must be authorized to do so.
    auto liteParsedDocSource = LiteParsedDocumentSource::parse(nss, pipeline[0]);
    if (!liteParsedDocSource->isInitialSource()) {
        Privilege currentPriv =
            Privilege(ResourcePattern::forExactNamespace(nss), ActionType::find);
        Privilege::addPrivilegeToPrivilegeVector(&privileges, currentPriv);
    }

    // Confirm privileges for the pipeline.
    for (auto&& pipelineStage : pipeline) {
        liteParsedDocSource = LiteParsedDocumentSource::parse(nss, pipelineStage);
        PrivilegeVector currentPrivs = liteParsedDocSource->requiredPrivileges(
            isMongos, request.shouldBypassDocumentValidation());
        Privilege::addPrivilegesToPrivilegeVector(&privileges, currentPrivs);
    }
    return privileges;
}

Status AuthorizationSessionImpl::checkAuthForFind(const NamespaceString& ns, bool hasTerm) {
    if (MONGO_unlikely(ns.isCommand())) {
        return Status(ErrorCodes::InternalError,
                      str::stream() << "Checking query auth on command namespace " << ns.ns());
    }
    if (!isAuthorizedForActionsOnNamespace(ns, ActionType::find)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "not authorized for query on " << ns.ns());
    }

    // Only internal clients (such as other nodes in a replica set) are allowed to use
    // the 'term' field in a find operation. Use of this field could trigger changes
    // in the receiving server's replication state and should be protected.
    if (hasTerm &&
        !isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                          ActionType::internal)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "not authorized for query with term on " << ns.ns());
    }

    return Status::OK();
}

Status AuthorizationSessionImpl::checkAuthForGetMore(const NamespaceString& ns,
                                                     long long cursorID,
                                                     bool hasTerm) {
    // Since users can only getMore their own cursors, we verify that a user either is authenticated
    // or does not need to be.
    if (!_externalState->shouldIgnoreAuthChecks() && !isAuthenticated()) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "not authorized for getMore on " << ns.db());
    }

    // Only internal clients (such as other nodes in a replica set) are allowed to use
    // the 'term' field in a getMore operation. Use of this field could trigger changes
    // in the receiving server's replication state and should be protected.
    if (hasTerm &&
        !isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                          ActionType::internal)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "not authorized for getMore with term on " << ns.ns());
    }

    return Status::OK();
}

Status AuthorizationSessionImpl::checkAuthForInsert(OperationContext* opCtx,
                                                    const NamespaceString& ns) {
    ActionSet required{ActionType::insert};
    if (documentValidationDisabled(opCtx)) {
        required.addAction(ActionType::bypassDocumentValidation);
    }
    if (!isAuthorizedForActionsOnNamespace(ns, required)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "not authorized for insert on " << ns.ns());
    }

    return Status::OK();
}

Status AuthorizationSessionImpl::checkAuthForUpdate(OperationContext* opCtx,
                                                    const NamespaceString& ns,
                                                    const BSONObj& query,
                                                    const write_ops::UpdateModification& update,
                                                    bool upsert) {
    ActionSet required{ActionType::update};
    StringData operationType = "update"_sd;

    if (upsert) {
        required.addAction(ActionType::insert);
        operationType = "upsert"_sd;
    }

    if (documentValidationDisabled(opCtx)) {
        required.addAction(ActionType::bypassDocumentValidation);
    }

    if (!isAuthorizedForActionsOnNamespace(ns, required)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "not authorized for " << operationType << " on " << ns.ns());
    }

    return Status::OK();
}

Status AuthorizationSessionImpl::checkAuthForDelete(OperationContext* opCtx,
                                                    const NamespaceString& ns,
                                                    const BSONObj& query) {
    if (!isAuthorizedForActionsOnNamespace(ns, ActionType::remove)) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "not authorized to remove from " << ns.ns());
    }
    return Status::OK();
}

Status AuthorizationSessionImpl::checkAuthForKillCursors(const NamespaceString& ns,
                                                         UserNameIterator cursorOwner) {
    if (isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                         ActionType::killAnyCursor)) {
        return Status::OK();
    }

    if (isCoauthorizedWith(cursorOwner)) {
        return Status::OK();
    }

    ResourcePattern target;
    if (ns.isListCollectionsCursorNS()) {
        target = ResourcePattern::forDatabaseName(ns.db());
    } else {
        target = ResourcePattern::forExactNamespace(ns);
    }

    if (isAuthorizedForActionsOnResource(target, ActionType::killAnyCursor)) {
        return Status::OK();
    }

    return Status(ErrorCodes::Unauthorized,
                  str::stream() << "not authorized to kill cursor on " << ns.ns());
}

Status AuthorizationSessionImpl::checkAuthForCreate(const NamespaceString& ns,
                                                    const BSONObj& cmdObj,
                                                    bool isMongos) {
    if (cmdObj["capped"].trueValue() &&
        !isAuthorizedForActionsOnNamespace(ns, ActionType::convertToCapped)) {
        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }

    const bool hasCreateCollectionAction =
        isAuthorizedForActionsOnNamespace(ns, ActionType::createCollection);

    // If attempting to create a view, check for additional required privileges.
    if (cmdObj["viewOn"]) {
        // You need the createCollection action on this namespace; the insert action is not
        // sufficient.
        if (!hasCreateCollectionAction) {
            return Status(ErrorCodes::Unauthorized, "unauthorized");
        }

        // Parse the viewOn namespace and the pipeline. If no pipeline was specified, use the empty
        // pipeline.
        NamespaceString viewOnNs(ns.db(), cmdObj["viewOn"].checkAndGetStringData());
        auto pipeline =
            cmdObj.hasField("pipeline") ? BSONArray(cmdObj["pipeline"].Obj()) : BSONArray();
        return checkAuthForCreateOrModifyView(this, ns, viewOnNs, pipeline, isMongos);
    }

    // To create a regular collection, ActionType::createCollection or ActionType::insert are
    // both acceptable.
    if (hasCreateCollectionAction || isAuthorizedForActionsOnNamespace(ns, ActionType::insert)) {
        return Status::OK();
    }

    return Status(ErrorCodes::Unauthorized, "unauthorized");
}

Status AuthorizationSessionImpl::checkAuthForCollMod(const NamespaceString& ns,
                                                     const BSONObj& cmdObj,
                                                     bool isMongos) {
    if (!isAuthorizedForActionsOnNamespace(ns, ActionType::collMod)) {
        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }

    // Check for additional required privileges if attempting to modify a view. When auth is
    // enabled, users must specify both "viewOn" and "pipeline" together. This prevents a user from
    // exposing more information in the original underlying namespace by only changing "pipeline",
    // or looking up more information via the original pipeline by only changing "viewOn".
    const bool hasViewOn = cmdObj.hasField("viewOn");
    const bool hasPipeline = cmdObj.hasField("pipeline");
    if (hasViewOn != hasPipeline) {
        return Status(
            ErrorCodes::InvalidOptions,
            "Must specify both 'viewOn' and 'pipeline' when modifying a view and auth is enabled");
    }
    if (hasViewOn) {
        NamespaceString viewOnNs(ns.db(), cmdObj["viewOn"].checkAndGetStringData());
        auto viewPipeline = BSONArray(cmdObj["pipeline"].Obj());
        return checkAuthForCreateOrModifyView(this, ns, viewOnNs, viewPipeline, isMongos);
    }

    return Status::OK();
}

Status AuthorizationSessionImpl::checkAuthorizedToGrantPrivilege(const Privilege& privilege) {
    const ResourcePattern& resource = privilege.getResourcePattern();
    if (resource.isDatabasePattern() || resource.isExactNamespacePattern()) {
        if (!isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(resource.databaseToMatch()),
                ActionType::grantRole)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "Not authorized to grant privileges on the "
                                        << resource.databaseToMatch() << "database");
        }
    } else if (!isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName("admin"),
                                                 ActionType::grantRole)) {
        return Status(ErrorCodes::Unauthorized,
                      "To grant privileges affecting multiple databases or the cluster,"
                      " must be authorized to grant roles from the admin database");
    }
    return Status::OK();
}


Status AuthorizationSessionImpl::checkAuthorizedToRevokePrivilege(const Privilege& privilege) {
    const ResourcePattern& resource = privilege.getResourcePattern();
    if (resource.isDatabasePattern() || resource.isExactNamespacePattern()) {
        if (!isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(resource.databaseToMatch()),
                ActionType::revokeRole)) {
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "Not authorized to revoke privileges on the "
                                        << resource.databaseToMatch() << "database");
        }
    } else if (!isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName("admin"),
                                                 ActionType::revokeRole)) {
        return Status(ErrorCodes::Unauthorized,
                      "To revoke privileges affecting multiple databases or the cluster,"
                      " must be authorized to revoke roles from the admin database");
    }
    return Status::OK();
}

bool AuthorizationSessionImpl::isAuthorizedToParseNamespaceElement(const BSONElement& element) {
    const bool isUUID = element.type() == BinData && element.binDataType() == BinDataType::newUUID;

    uassert(ErrorCodes::InvalidNamespace,
            "Failed to parse namespace element",
            element.type() == String || isUUID);

    if (isUUID) {
        return isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                ActionType::useUUID);
    }

    return true;
}

bool AuthorizationSessionImpl::isAuthorizedToCreateRole(const RoleName& roleName) {
    // A user is allowed to create a role under either of two conditions.

    // The user may create a role if the authorization system says they are allowed to.
    if (isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(roleName.getDB()),
                                         ActionType::createRole)) {
        return true;
    }

    // The user may create a role if the localhost exception is enabled, and they already own the
    // role. This implies they have obtained the role through an external authorization mechanism.
    if (_externalState->shouldAllowLocalhost()) {
        for (const auto& user : _authenticatedUsers) {
            if (user->hasRole(roleName)) {
                return true;
            }
        }
        LOGV2(20241,
              "Not authorized to create the first role in the system using the "
              "localhost exception. The user needs to acquire the role through "
              "external authentication first.",
              "role"_attr = roleName);
    }

    return false;
}

bool AuthorizationSessionImpl::isAuthorizedToGrantRole(const RoleName& role) {
    return isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(role.getDB()),
                                            ActionType::grantRole);
}

bool AuthorizationSessionImpl::isAuthorizedToRevokeRole(const RoleName& role) {
    return isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(role.getDB()),
                                            ActionType::revokeRole);
}

bool AuthorizationSessionImpl::isAuthorizedForPrivilege(const Privilege& privilege) {
    if (_externalState->shouldIgnoreAuthChecks())
        return true;

    return _isAuthorizedForPrivilege(privilege);
}

bool AuthorizationSessionImpl::isAuthorizedForPrivileges(const vector<Privilege>& privileges) {
    if (_externalState->shouldIgnoreAuthChecks())
        return true;

    for (size_t i = 0; i < privileges.size(); ++i) {
        if (!_isAuthorizedForPrivilege(privileges[i]))
            return false;
    }

    return true;
}

bool AuthorizationSessionImpl::isAuthorizedForActionsOnResource(const ResourcePattern& resource,
                                                                ActionType action) {
    return isAuthorizedForPrivilege(Privilege(resource, action));
}

bool AuthorizationSessionImpl::isAuthorizedForActionsOnResource(const ResourcePattern& resource,
                                                                const ActionSet& actions) {
    return isAuthorizedForPrivilege(Privilege(resource, actions));
}

bool AuthorizationSessionImpl::isAuthorizedForActionsOnNamespace(const NamespaceString& ns,
                                                                 ActionType action) {
    return isAuthorizedForPrivilege(Privilege(ResourcePattern::forExactNamespace(ns), action));
}

bool AuthorizationSessionImpl::isAuthorizedForActionsOnNamespace(const NamespaceString& ns,
                                                                 const ActionSet& actions) {
    return isAuthorizedForPrivilege(Privilege(ResourcePattern::forExactNamespace(ns), actions));
}

static const int resourceSearchListCapacity = 5;
/**
 * Builds from "target" an exhaustive list of all ResourcePatterns that match "target".
 *
 * Some resources are considered to be "normal resources", and are matched by the
 * forAnyNormalResource pattern. Collections which are not prefixed with "system.",
 * and which do not belong inside of the "local" or "config" databases are "normal".
 * Database other than "local" and "config" are normal.
 *
 * Most collections are matched by their database's resource. Collections prefixed with "system."
 * are not. Neither are collections on the "local" database, whose name are prefixed with "replset."
 *
 *
 * Stores the resulting list into resourceSearchList, and returns the length.
 *
 * The seach lists are as follows, depending on the type of "target":
 *
 * target is ResourcePattern::forAnyResource():
 *   searchList = { ResourcePattern::forAnyResource(), ResourcePattern::forAnyResource() }
 * target is the ResourcePattern::forClusterResource():
 *   searchList = { ResourcePattern::forAnyResource(), ResourcePattern::forClusterResource() }
 * target is a database, db:
 *   searchList = { ResourcePattern::forAnyResource(),
 *                  ResourcePattern::forAnyNormalResource(),
 *                  db }
 * target is a non-system collection, db.coll:
 *   searchList = { ResourcePattern::forAnyResource(),
 *                  ResourcePattern::forAnyNormalResource(),
 *                  db,
 *                  coll,
 *                  db.coll }
 * target is a system collection, db.system.coll:
 *   searchList = { ResourcePattern::forAnyResource(),
 *                  system.coll,
 *                  db.system.coll }
 */
static int buildResourceSearchList(const ResourcePattern& target,
                                   ResourcePattern resourceSearchList[resourceSearchListCapacity]) {
    int size = 0;
    resourceSearchList[size++] = ResourcePattern::forAnyResource();
    if (target.isExactNamespacePattern()) {
        // Normal collections can be matched by anyNormalResource, or their database's resource.
        if (target.ns().isNormalCollection()) {
            // But even normal collections in non-normal databases should not be matchable with
            // ResourcePattern::forAnyNormalResource. 'local' and 'config' are
            // used to store special system collections, which user level
            // administrators should not be able to manipulate.
            if (target.ns().db() != "local" && target.ns().db() != "config") {
                resourceSearchList[size++] = ResourcePattern::forAnyNormalResource();
            }
            resourceSearchList[size++] = ResourcePattern::forDatabaseName(target.ns().db());
        }

        // All collections can be matched by a collection resource for their name
        resourceSearchList[size++] = ResourcePattern::forCollectionName(target.ns().coll());
    } else if (target.isDatabasePattern()) {
        if (target.ns().db() != "local" && target.ns().db() != "config") {
            resourceSearchList[size++] = ResourcePattern::forAnyNormalResource();
        }
    }
    resourceSearchList[size++] = target;
    dassert(size <= resourceSearchListCapacity);
    return size;
}

bool AuthorizationSessionImpl::isAuthorizedToChangeAsUser(const UserName& userName,
                                                          ActionType actionType) {
    User* user = lookupUser(userName);
    if (!user) {
        return false;
    }
    ResourcePattern resourceSearchList[resourceSearchListCapacity];
    const int resourceSearchListLength = buildResourceSearchList(
        ResourcePattern::forDatabaseName(userName.getDB()), resourceSearchList);

    ActionSet actions;
    for (int i = 0; i < resourceSearchListLength; ++i) {
        actions.addAllActionsFromSet(user->getActionsForResource(resourceSearchList[i]));
    }
    return actions.contains(actionType);
}

bool AuthorizationSessionImpl::isAuthorizedToChangeOwnPasswordAsUser(const UserName& userName) {
    return AuthorizationSessionImpl::isAuthorizedToChangeAsUser(userName,
                                                                ActionType::changeOwnPassword);
}

bool AuthorizationSessionImpl::isAuthorizedToChangeOwnCustomDataAsUser(const UserName& userName) {
    return AuthorizationSessionImpl::isAuthorizedToChangeAsUser(userName,
                                                                ActionType::changeOwnCustomData);
}

StatusWith<PrivilegeVector> AuthorizationSessionImpl::checkAuthorizedToListCollections(
    StringData dbname, const BSONObj& cmdObj) {
    if (cmdObj["authorizedCollections"].trueValue() && cmdObj["nameOnly"].trueValue() &&
        AuthorizationSessionImpl::isAuthorizedForAnyActionOnAnyResourceInDB(dbname)) {
        return PrivilegeVector();
    }

    // Check for the listCollections ActionType on the database.
    PrivilegeVector privileges = {
        Privilege(ResourcePattern::forDatabaseName(dbname), ActionType::listCollections)};
    if (AuthorizationSessionImpl::isAuthorizedForPrivileges(privileges)) {
        return privileges;
    }
    return Status(ErrorCodes::Unauthorized, "unauthorized");
}

bool AuthorizationSessionImpl::isAuthenticatedAsUserWithRole(const RoleName& roleName) {
    for (UserSet::iterator it = _authenticatedUsers.begin(); it != _authenticatedUsers.end();
         ++it) {
        if ((*it)->hasRole(roleName)) {
            return true;
        }
    }
    return false;
}

bool AuthorizationSessionImpl::isAuthenticated() {
    return _authenticatedUsers.begin() != _authenticatedUsers.end();
}

void AuthorizationSessionImpl::_refreshUserInfoAsNeeded(OperationContext* opCtx) {
    AuthorizationManager& authMan = getAuthorizationManager();
    UserSet::iterator it = _authenticatedUsers.begin();

    while (it != _authenticatedUsers.end()) {
        auto& user = *it;
        if (!user.isValid()) {
            // Make a good faith effort to acquire an up-to-date user object, since the one
            // we've cached is marked "out-of-date."
            UserName name = user->getName();
            UserHandle updatedUser;

            auto swUser = authMan.acquireUserForSessionRefresh(opCtx, name, user->getID());
            auto& status = swUser.getStatus();

            // Take out a lock on the client here to ensure that no one reads while
            // _authenticatedUsers is being modified.
            stdx::lock_guard<Client> lk(*opCtx->getClient());

            // The user is invalid, so make sure that we erase it from _authenticateUsers at the
            // end of this block.
            auto removeGuard = makeGuard([&] { _authenticatedUsers.removeAt(it++); });

            switch (status.code()) {
                case ErrorCodes::OK: {
                    updatedUser = std::move(swUser.getValue());
                    try {
                        auto restrictionStatus = updatedUser->validateRestrictions(opCtx);
                        if (!restrictionStatus.isOK()) {
                            LOGV2(20242,
                                  "Removed user with unmet authentication restrictions from "
                                  "session cache of user information. Restriction failed",
                                  "user"_attr = name,
                                  "reason"_attr = restrictionStatus.reason());
                            // If we remove from the UserSet, we cannot increment the iterator.
                            continue;
                        }
                    } catch (...) {
                        LOGV2(20243,
                              "Evaluating authentication restrictions for user resulted in an "
                              "unknown exception. Removing user from the session cache",
                              "user"_attr = name);
                        continue;
                    }

                    // Success! Replace the old User object with the updated one.
                    removeGuard.dismiss();
                    _authenticatedUsers.replaceAt(it, std::move(updatedUser));
                    LOGV2_DEBUG(20244,
                                1,
                                "Updated session cache of user information for user",
                                "user"_attr = name);
                    break;
                }
                case ErrorCodes::UserNotFound: {
                    // User does not exist anymore; remove it from _authenticatedUsers.
                    LOGV2(20245,
                          "Removed deleted user from session cache of user information",
                          "user"_attr = name);
                    continue;  // No need to advance "it" in this case.
                }
                case ErrorCodes::UnsupportedFormat: {
                    // An auth subsystem has explicitly indicated a failure.
                    LOGV2(20246,
                          "Removed user from session cache of user information because of "
                          "refresh failure",
                          "user"_attr = name,
                          "error"_attr = status);
                    continue;  // No need to advance "it" in this case.
                }
                default:
                    // Unrecognized error; assume that it's transient, and continue working with the
                    // out-of-date privilege data.
                    LOGV2_WARNING(20247,
                                  "Could not fetch updated user privilege information for {user}; "
                                  "continuing to use old information. Reason is {error}",
                                  "Could not fetch updated user privilege information, continuing "
                                  "to use old information",
                                  "user"_attr = name,
                                  "error"_attr = redact(status));
                    removeGuard.dismiss();
                    break;
            }
        }
        ++it;
    }
    _buildAuthenticatedRolesVector();
}

void AuthorizationSessionImpl::_buildAuthenticatedRolesVector() {
    _authenticatedRoleNames.clear();
    for (UserSet::iterator it = _authenticatedUsers.begin(); it != _authenticatedUsers.end();
         ++it) {
        RoleNameIterator roles = (*it)->getIndirectRoles();
        while (roles.more()) {
            RoleName roleName = roles.next();
            _authenticatedRoleNames.push_back(RoleName(roleName.getRole(), roleName.getDB()));
        }
    }
}

bool AuthorizationSessionImpl::isAuthorizedForAnyActionOnAnyResourceInDB(StringData db) {
    if (_externalState->shouldIgnoreAuthChecks()) {
        return true;
    }

    for (const auto& user : _authenticatedUsers) {
        // First lookup any Privileges on this database specifying Database resources
        if (user->hasActionsForResource(ResourcePattern::forDatabaseName(db))) {
            return true;
        }

        // Any resource will match any collection in the database
        if (user->hasActionsForResource(ResourcePattern::forAnyResource())) {
            return true;
        }

        // If the user is authorized for anyNormalResource, then they implicitly have access
        // to most databases.
        if (db != "local" && db != "config" &&
            user->hasActionsForResource(ResourcePattern::forAnyNormalResource())) {
            return true;
        }

        // We've checked all the resource types that can be directly expressed. Now we must
        // iterate all privileges, until we see something that could reside in the target database.
        User::ResourcePrivilegeMap map = user->getPrivileges();
        for (const auto& privilege : map) {
            // If the user has a Collection privilege, then they're authorized for this resource
            // on all databases.
            if (privilege.first.isCollectionPattern()) {
                return true;
            }

            // If the user has an exact namespace privilege on a collection in this database, they
            // have access to a resource in this database.
            if (privilege.first.isExactNamespacePattern() &&
                privilege.first.databaseToMatch() == db) {
                return true;
            }
        }
    }

    return false;
}

bool AuthorizationSessionImpl::isAuthorizedForAnyActionOnResource(const ResourcePattern& resource) {
    if (_externalState->shouldIgnoreAuthChecks()) {
        return true;
    }

    std::array<ResourcePattern, resourceSearchListCapacity> resourceSearchList;
    const int resourceSearchListLength =
        buildResourceSearchList(resource, resourceSearchList.data());

    for (int i = 0; i < resourceSearchListLength; ++i) {
        for (const auto& user : _authenticatedUsers) {
            if (user->hasActionsForResource(resourceSearchList[i])) {
                return true;
            }
        }
    }

    return false;
}


bool AuthorizationSessionImpl::_isAuthorizedForPrivilege(const Privilege& privilege) {
    const ResourcePattern& target(privilege.getResourcePattern());

    ResourcePattern resourceSearchList[resourceSearchListCapacity];
    const int resourceSearchListLength = buildResourceSearchList(target, resourceSearchList);

    ActionSet unmetRequirements = privilege.getActions();

    PrivilegeVector defaultPrivileges = getDefaultPrivileges();
    for (PrivilegeVector::iterator it = defaultPrivileges.begin(); it != defaultPrivileges.end();
         ++it) {
        for (int i = 0; i < resourceSearchListLength; ++i) {
            if (!(it->getResourcePattern() == resourceSearchList[i]))
                continue;

            ActionSet userActions = it->getActions();
            unmetRequirements.removeAllActionsFromSet(userActions);

            if (unmetRequirements.empty())
                return true;
        }
    }

    for (const auto& user : _authenticatedUsers) {
        for (int i = 0; i < resourceSearchListLength; ++i) {
            ActionSet userActions = user->getActionsForResource(resourceSearchList[i]);
            unmetRequirements.removeAllActionsFromSet(userActions);

            if (unmetRequirements.empty()) {
                return true;
            }
        }
    }

    return false;
}

void AuthorizationSessionImpl::setImpersonatedUserData(std::vector<UserName> usernames,
                                                       std::vector<RoleName> roles) {
    _impersonatedUserNames = usernames;
    _impersonatedRoleNames = roles;
    _impersonationFlag = true;
}

bool AuthorizationSessionImpl::isCoauthorizedWithClient(Client* opClient, WithLock opClientLock) {
    auto getUserNames = [](AuthorizationSession* authSession) {
        if (authSession->isImpersonating()) {
            return authSession->getImpersonatedUserNames();
        } else {
            return authSession->getAuthenticatedUserNames();
        }
    };

    UserNameIterator it = getUserNames(this);
    while (it.more()) {
        UserNameIterator opIt = getUserNames(AuthorizationSession::get(opClient));
        while (opIt.more()) {
            if (it.get() == opIt.get()) {
                return true;
            }
            opIt.next();
        }
        it.next();
    }

    return false;
}

bool AuthorizationSessionImpl::isCoauthorizedWith(UserNameIterator userNameIter) {
    if (!getAuthorizationManager().isAuthEnabled()) {
        return true;
    }

    if (!userNameIter.more() && !isAuthenticated()) {
        return true;
    }

    for (; userNameIter.more(); userNameIter.next()) {
        for (UserNameIterator thisUserNameIter = getAuthenticatedUserNames();
             thisUserNameIter.more();
             thisUserNameIter.next()) {
            if (*userNameIter == *thisUserNameIter) {
                return true;
            }
        }
    }

    return false;
}

UserNameIterator AuthorizationSessionImpl::getImpersonatedUserNames() {
    return makeUserNameIterator(_impersonatedUserNames.begin(), _impersonatedUserNames.end());
}

RoleNameIterator AuthorizationSessionImpl::getImpersonatedRoleNames() {
    return makeRoleNameIterator(_impersonatedRoleNames.begin(), _impersonatedRoleNames.end());
}

bool AuthorizationSessionImpl::isUsingLocalhostBypass() {
    return getAuthorizationManager().isAuthEnabled() && _externalState->shouldAllowLocalhost();
}

// Clear the vectors of impersonated usernames and roles.
void AuthorizationSessionImpl::clearImpersonatedUserData() {
    _impersonatedUserNames.clear();
    _impersonatedRoleNames.clear();
    _impersonationFlag = false;
}


bool AuthorizationSessionImpl::isImpersonating() const {
    return _impersonationFlag;
}

auto AuthorizationSessionImpl::checkCursorSessionPrivilege(
    OperationContext* const opCtx, const boost::optional<LogicalSessionId> cursorSessionId)
    -> Status {
    auto nobodyIsLoggedIn = [authSession = this] { return !authSession->isAuthenticated(); };

    auto authHasImpersonatePrivilege = [authSession = this] {
        return authSession->isAuthorizedForPrivilege(
            Privilege(ResourcePattern::forClusterResource(), ActionType::impersonate));
    };

    auto authIsOn = [authSession = this] {
        return authSession->getAuthorizationManager().isAuthEnabled();
    };

    auto sessionIdToStringOrNone =
        [](const boost::optional<LogicalSessionId>& sessionId) -> std::string {
        if (sessionId) {
            return str::stream() << *sessionId;
        }
        return "none";
    };

    // If the cursor has a session then one of the following must be true:
    // 1: context session id must match cursor session id.
    // 2: user must be magic special (__system, or background task, etc).

    // We do not check the user's ID against the cursor's notion of a user ID, since higher level
    // auth checks will check that for us anyhow.
    if (authIsOn() &&  // If the authorization is not on, then we permit anybody to do anything.
        cursorSessionId != opCtx->getLogicalSessionId() &&  // If the cursor's session doesn't match
                                                            // the Operation Context's session, then
                                                            // we should forbid the operation even
                                                            // when the cursor has no session.
        !nobodyIsLoggedIn() &&          // Unless, for some reason a user isn't actually using this
                                        // Operation Context (which implies a background job
        !authHasImpersonatePrivilege()  // Or if the user has an impersonation privilege, in which
                                        // case, the user gets to sidestep certain checks.
    ) {
        return Status{ErrorCodes::Unauthorized,
                      str::stream()
                          << "Cursor session id (" << sessionIdToStringOrNone(cursorSessionId)
                          << ") is not the same as the operation context's session id ("
                          << sessionIdToStringOrNone(opCtx->getLogicalSessionId()) << ")"};
    }

    return Status::OK();
}

}  // namespace mongo
