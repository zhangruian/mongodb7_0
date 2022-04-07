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

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/unittest/temp_dir.h"

namespace mongo {

/**
 * Test fixture class for tests that use the "ephemeralForTest" storage engine.
 */
class ServiceContextMongoDTest : public virtual ServiceContextTest {
public:
    constexpr static StorageEngineInitFlags kDefaultStorageEngineInitFlags =
        StorageEngineInitFlags::kAllowNoLockFile | StorageEngineInitFlags::kSkipMetadataFile;

protected:
    enum class RepairAction { kNoRepair, kRepair };

    class Options {
    public:
        Options(){};

        Options& engine(std::string engine) {
            _engine = std::move(engine);
            return *this;
        }
        Options& repair(RepairAction repair) {
            _repair = repair;
            return *this;
        }
        Options& initFlags(StorageEngineInitFlags initFlags) {
            _initFlags = initFlags;
            return *this;
        }
        Options& useReplSettings(bool useReplSettings) {
            _useReplSettings = useReplSettings;
            return *this;
        }
        Options& useMockClock(bool useMockClock) {
            _useMockClock = useMockClock;
            return *this;
        }

    private:
        std::string _engine = "wiredTiger";
        RepairAction _repair = RepairAction::kNoRepair;
        StorageEngineInitFlags _initFlags = kDefaultStorageEngineInitFlags;
        bool _useReplSettings = false;
        bool _useMockClock = false;

        friend class ServiceContextMongoDTest;
    };

    explicit ServiceContextMongoDTest(Options options = {});

    virtual ~ServiceContextMongoDTest();

    void tearDown() override;

private:
    struct {
        std::string engine;
        bool engineSetByUser;
        bool repair;
    } _stashedStorageParams;

    struct {
        bool enableMajorityReadConcern;
    } _stashedServerParams;

    unittest::TempDir _tempDir;
};

}  // namespace mongo
