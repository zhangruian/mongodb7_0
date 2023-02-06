/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/move_primary/move_primary_database_cloner.h"

#include "mongo/base/string_data.h"
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/cloner_utils.h"
#include "mongo/db/repl/database_cloner_gen.h"
#include "mongo/db/s/move_primary/move_primary_collection_cloner.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/basic.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kMovePrimary

namespace mongo {

MovePrimaryDatabaseCloner::MovePrimaryDatabaseCloner(const std::string& dbName,
                                                     MovePrimarySharedData* sharedData,
                                                     const HostAndPort& source,
                                                     DBClientConnection* client,
                                                     repl::StorageInterface* storageInterface,
                                                     ThreadPool* dbPool)
    : MovePrimaryBaseCloner(
          "MovePrimaryDatabaseCloner"_sd, sharedData, source, client, storageInterface, dbPool),
      _listCollectionsStage(
          "listCollections", this, &MovePrimaryDatabaseCloner::listCollectionsStage),
      _listExistingCollectionsStage("listExistingCollections",
                                    this,
                                    &MovePrimaryDatabaseCloner::listExistingCollectionsStage) {
    invariant(!dbName.empty());
}

repl::BaseCloner::ClonerStages MovePrimaryDatabaseCloner::getStages() {
    return {&_listCollectionsStage, &_listExistingCollectionsStage};
}

void MovePrimaryDatabaseCloner::preStage() {}

repl::BaseCloner::AfterStageBehavior MovePrimaryDatabaseCloner::listCollectionsStage() {
    return kContinueNormally;
}

repl::BaseCloner::AfterStageBehavior MovePrimaryDatabaseCloner::listExistingCollectionsStage() {
    return kContinueNormally;
}

void MovePrimaryDatabaseCloner::postStage() {}

}  // namespace mongo
