/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/client/sdam/topology_manager.h"

#include <boost/optional/optional_io.hpp>

#include "mongo/client/sdam/sdam_test_base.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/system_clock_source.h"

namespace mongo {

namespace sdam {

class TopologyManagerTestFixture : public SdamTestFixture {
protected:
    void assertDefaultConfig(const TopologyDescription& topologyDescription);

    static inline const auto kSetName = std::string("mySetName");

    static inline const std::vector<HostAndPort> kOneServer{HostAndPort("foo:1234")};
    static inline const std::vector<HostAndPort> kThreeServers{
        HostAndPort("foo:1234"), HostAndPort("bar:1234"), HostAndPort("baz:1234")};

    static BSONObjBuilder okBuilder() {
        return std::move(BSONObjBuilder().append("ok", 1));
    }

    static inline const auto clockSource = SystemClockSource::get();

    static inline const auto kBsonOk = okBuilder().obj();
    static inline const auto kBsonTopologyVersionLow =
        okBuilder().append("topologyVersion", TopologyVersion(OID::max(), 0).toBSON()).obj();
    static inline const auto kBsonTopologyVersionHigh =
        okBuilder().append("topologyVersion", TopologyVersion(OID::max(), 1).toBSON()).obj();
    static inline const auto kBsonRsPrimary = okBuilder()
                                                  .append("ismaster", true)
                                                  .append("setName", kSetName)
                                                  .append("minWireVersion", 2)
                                                  .append("maxWireVersion", 10)
                                                  .appendArray("hosts",
                                                               BSON_ARRAY("foo:1234"
                                                                          << "bar:1234"
                                                                          << "baz:1234"))

                                                  .obj();
};

TEST_F(TopologyManagerTestFixture, ShouldUpdateTopologyVersionOnSuccess) {
    auto config = SdamConfiguration(kOneServer);
    TopologyManager topologyManager(config, clockSource);

    auto topologyDescription = topologyManager.getTopologyDescription();
    ASSERT_EQUALS(topologyDescription->getServers().size(), 1);
    auto serverDescription = topologyDescription->getServers()[0];
    ASSERT(serverDescription->getTopologyVersion() == boost::none);

    // If previous topologyVersion is boost::none, should update to new topologyVersion
    auto isMasterOutcome = HelloOutcome(serverDescription->getAddress(),
                                        kBsonTopologyVersionLow,
                                        duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    topologyManager.onServerDescription(isMasterOutcome);
    topologyDescription = topologyManager.getTopologyDescription();
    auto newServerDescription = topologyDescription->getServers()[0];
    ASSERT(newServerDescription->getTopologyVersion());
    ASSERT_BSONOBJ_EQ(newServerDescription->getTopologyVersion()->toBSON(),
                      kBsonTopologyVersionLow.getObjectField("topologyVersion"));

    // If previous topologyVersion is <= new topologyVersion, should update to new topologyVersion
    isMasterOutcome = HelloOutcome(serverDescription->getAddress(),
                                   kBsonTopologyVersionHigh,
                                   duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    topologyManager.onServerDescription(isMasterOutcome);
    topologyDescription = topologyManager.getTopologyDescription();
    newServerDescription = topologyDescription->getServers()[0];
    ASSERT(newServerDescription->getTopologyVersion());
    ASSERT_BSONOBJ_EQ(newServerDescription->getTopologyVersion()->toBSON(),
                      kBsonTopologyVersionHigh.getObjectField("topologyVersion"));
}

TEST_F(TopologyManagerTestFixture,
       ShouldUpdateServerDescriptionsTopologyDescriptionPtrWhenTopologyDescriptionIsInstalled) {
    auto checkServerTopologyDescriptionMatches = [](TopologyDescriptionPtr topologyDescription) {
        auto rawTopologyDescPtr = topologyDescription.get();
        for (auto server : topologyDescription->getServers()) {
            auto rawServerTopologyDescPtr = (*server->getTopologyDescription()).get();
            ASSERT(server->getTopologyDescription());
            ASSERT(rawServerTopologyDescPtr == rawTopologyDescPtr);
        }
    };

    auto config = SdamConfiguration(kThreeServers);
    TopologyManager topologyManager(config, clockSource);
    checkServerTopologyDescriptionMatches(topologyManager.getTopologyDescription());

    auto topologyDescription = topologyManager.getTopologyDescription();
    auto firstServer = *topologyDescription->getServers()[0];
    auto host = firstServer.getAddress();
    auto isMasterOutcome =
        HelloOutcome(host, kBsonRsPrimary, duration_cast<HelloRTT>(mongo::Milliseconds{40}));
    topologyManager.onServerDescription(isMasterOutcome);
    checkServerTopologyDescriptionMatches(topologyManager.getTopologyDescription());

    topologyManager.onServerRTTUpdated(host, Milliseconds{40});
    checkServerTopologyDescriptionMatches(topologyManager.getTopologyDescription());
}

TEST_F(TopologyManagerTestFixture, ShouldUpdateTopologyVersionOnErrorIfSent) {
    auto config = SdamConfiguration(kOneServer);
    TopologyManager topologyManager(config, clockSource);

    auto topologyDescription = topologyManager.getTopologyDescription();
    ASSERT_EQUALS(topologyDescription->getServers().size(), 1);
    auto serverDescription = topologyDescription->getServers()[0];
    ASSERT(serverDescription->getTopologyVersion() == boost::none);

    // If previous topologyVersion is boost::none, should update to new topologyVersion
    auto isMasterOutcome = HelloOutcome(serverDescription->getAddress(),
                                        kBsonTopologyVersionLow,
                                        duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    topologyManager.onServerDescription(isMasterOutcome);
    topologyDescription = topologyManager.getTopologyDescription();
    auto newServerDescription = topologyDescription->getServers()[0];
    ASSERT_BSONOBJ_EQ(newServerDescription->getTopologyVersion()->toBSON(),
                      kBsonTopologyVersionLow.getObjectField("topologyVersion"));

    // If isMasterOutcome is not successful, should preserve old topologyVersion
    isMasterOutcome =
        HelloOutcome(serverDescription->getAddress(), kBsonTopologyVersionLow, "an error occurred");
    topologyManager.onServerDescription(isMasterOutcome);
    topologyDescription = topologyManager.getTopologyDescription();
    newServerDescription = topologyDescription->getServers()[0];
    ASSERT_BSONOBJ_EQ(newServerDescription->getTopologyVersion()->toBSON(),
                      kBsonTopologyVersionLow.getObjectField("topologyVersion"));
}

TEST_F(TopologyManagerTestFixture, ShouldNotUpdateServerDescriptionIfNewTopologyVersionOlder) {
    auto config = SdamConfiguration(kOneServer);
    TopologyManager topologyManager(config, clockSource);

    auto topologyDescription = topologyManager.getTopologyDescription();
    ASSERT_EQUALS(topologyDescription->getServers().size(), 1);
    auto serverDescription = topologyDescription->getServers()[0];
    ASSERT(serverDescription->getTopologyVersion() == boost::none);

    // If previous topologyVersion is boost::none, should update to new topologyVersion
    auto isMasterOutcome = HelloOutcome(serverDescription->getAddress(),
                                        kBsonTopologyVersionHigh,
                                        duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    topologyManager.onServerDescription(isMasterOutcome);
    topologyDescription = topologyManager.getTopologyDescription();
    auto newServerDescription = topologyDescription->getServers()[0];
    ASSERT_BSONOBJ_EQ(newServerDescription->getTopologyVersion()->toBSON(),
                      kBsonTopologyVersionHigh.getObjectField("topologyVersion"));

    // If isMasterOutcome is not successful, should preserve old topologyVersion
    isMasterOutcome = HelloOutcome(serverDescription->getAddress(), kBsonTopologyVersionLow);
    topologyManager.onServerDescription(isMasterOutcome);
    topologyDescription = topologyManager.getTopologyDescription();
    newServerDescription = topologyDescription->getServers()[0];
    ASSERT_BSONOBJ_EQ(newServerDescription->getTopologyVersion()->toBSON(),
                      kBsonTopologyVersionHigh.getObjectField("topologyVersion"));
}
};  // namespace sdam
};  // namespace mongo
