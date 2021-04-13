/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_critical_section.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/sharding_state_lock.h"

namespace mongo {

ReshardingCriticalSection::ReshardingCriticalSection(ServiceContext* serviceContext,
                                                     NamespaceString nss)
    : _nss(std::move(nss)) {
    _client = serviceContext->makeClient("ReshardingCriticalSection");

    {
        stdx::lock_guard<Client> lk(*_client.get());
        _client->setSystemOperationKillableByStepdown(lk);
    }

    AlternativeClientRegion acr(_client);
    _opCtx = cc().makeOperationContext();

    auto rawOpCtx = _opCtx.get();

    AutoGetCollection coll(rawOpCtx, _nss, MODE_S);
    const auto csr = CollectionShardingRuntime::get(rawOpCtx, _nss);
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(rawOpCtx, csr);

    csr->enterCriticalSectionCatchUpPhase(csrLock);
}

ReshardingCriticalSection::~ReshardingCriticalSection() {
    AlternativeClientRegion acr(_client);
    auto rawOpCtx = _opCtx.get();

    UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
    AutoGetCollection autoColl(rawOpCtx, _nss, MODE_IX);

    auto* const csr = CollectionShardingRuntime::get(rawOpCtx, _nss);
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(rawOpCtx, csr);
    csr->exitCriticalSection(csrLock);
}

}  // namespace mongo
