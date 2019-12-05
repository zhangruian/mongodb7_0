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
#include "mongo/client/sdam/topology_description.h"

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork
#include "mongo/client/sdam/server_description.h"
#include "mongo/db/wire_version.h"
#include "mongo/util/log.h"

namespace mongo::sdam {
////////////////////////
// TopologyDescription
////////////////////////
TopologyDescription::TopologyDescription(SdamConfiguration config)
    : _type(config.getInitialType()), _setName(config.getSetName()) {
    if (auto seeds = config.getSeedList()) {
        _servers.clear();
        for (auto address : *seeds) {
            _servers.push_back(std::make_shared<ServerDescription>(address));
        }
    }
}

const UUID& TopologyDescription::getId() const {
    return _id;
}

TopologyType TopologyDescription::getType() const {
    return _type;
}

const boost::optional<std::string>& TopologyDescription::getSetName() const {
    return _setName;
}

const boost::optional<int>& TopologyDescription::getMaxSetVersion() const {
    return _maxSetVersion;
}

const boost::optional<OID>& TopologyDescription::getMaxElectionId() const {
    return _maxElectionId;
}

const std::vector<ServerDescriptionPtr>& TopologyDescription::getServers() const {
    return _servers;
}

bool TopologyDescription::isWireVersionCompatible() const {
    return _compatible;
}

const boost::optional<std::string>& TopologyDescription::getWireVersionCompatibleError() const {
    return _compatibleError;
}

const boost::optional<int>& TopologyDescription::getLogicalSessionTimeoutMinutes() const {
    return _logicalSessionTimeoutMinutes;
}

void TopologyDescription::setType(TopologyType type) {
    _type = type;
}

bool TopologyDescription::containsServerAddress(const ServerAddress& address) const {
    return findServerByAddress(address) != boost::none;
}

std::vector<ServerDescriptionPtr> TopologyDescription::findServers(
    std::function<bool(const ServerDescriptionPtr&)> predicate) const {
    std::vector<ServerDescriptionPtr> result;
    std::copy_if(_servers.begin(), _servers.end(), std::back_inserter(result), predicate);
    return result;
}

const boost::optional<ServerDescriptionPtr> TopologyDescription::findServerByAddress(
    ServerAddress address) const {
    auto results = findServers([address](const ServerDescriptionPtr& serverDescription) {
        return serverDescription->getAddress() == address;
    });
    return (results.size() > 0) ? boost::make_optional(results.front()) : boost::none;
}

boost::optional<ServerDescriptionPtr> TopologyDescription::installServerDescription(
    const ServerDescriptionPtr& newServerDescription) {
    boost::optional<ServerDescriptionPtr> previousDescription;
    if (getType() == TopologyType::kSingle) {
        // For Single, there is always one ServerDescription in TopologyDescription.servers;
        // the ServerDescription in TopologyDescription.servers MUST be replaced with the new
        // ServerDescription.
        invariant(_servers.size() == 1);
        previousDescription = _servers[0];
        _servers[0] = std::shared_ptr<ServerDescription>(newServerDescription);
    } else {
        for (auto it = _servers.begin(); it != _servers.end(); ++it) {
            const auto& currentDescription = *it;
            if (currentDescription->getAddress() == newServerDescription->getAddress()) {
                previousDescription = *it;
                *it = std::shared_ptr<ServerDescription>(newServerDescription);
                break;
            }
        }

        if (!previousDescription) {
            _servers.push_back(std::shared_ptr<ServerDescription>(newServerDescription));
        }
    }

    checkWireCompatibilityVersions();
    calculateLogicalSessionTimeout();
    return previousDescription;
}

void TopologyDescription::removeServerDescription(const ServerAddress& serverAddress) {
    auto it = std::find_if(
        _servers.begin(), _servers.end(), [serverAddress](const ServerDescriptionPtr& description) {
            return description->getAddress() == serverAddress;
        });
    if (it != _servers.end()) {
        _servers.erase(it);
    }
}

void TopologyDescription::checkWireCompatibilityVersions() {
    const WireVersionInfo supportedWireVersion = {BATCH_COMMANDS, LATEST_WIRE_VERSION};
    std::ostringstream errorOss;

    _compatible = true;
    for (const auto& serverDescription : _servers) {
        if (serverDescription->getType() == ServerType::kUnknown) {
            continue;
        }

        if (serverDescription->getMinWireVersion() > supportedWireVersion.maxWireVersion) {
            _compatible = false;
            errorOss << "Server at " << serverDescription->getAddress() << " requires wire version "
                     << serverDescription->getMinWireVersion()
                     << " but this version of mongo only supports up to "
                     << supportedWireVersion.maxWireVersion << ".";
            break;
        } else if (serverDescription->getMaxWireVersion() < supportedWireVersion.minWireVersion) {
            _compatible = false;
            const auto& mongoVersion =
                minimumRequiredMongoVersionString(supportedWireVersion.minWireVersion);
            errorOss << "Server at " << serverDescription->getAddress() << " requires wire version "
                     << serverDescription->getMaxWireVersion()
                     << " but this version of mongo requires at least "
                     << supportedWireVersion.minWireVersion << " (MongoDB " << mongoVersion << ").";
            break;
        }
    }

    _compatibleError = (_compatible) ? boost::none : boost::make_optional(errorOss.str());
}

const std::string TopologyDescription::minimumRequiredMongoVersionString(int version) {
    switch (version) {
        case PLACEHOLDER_FOR_44:
            return "4.4";
        case SHARDED_TRANSACTIONS:
            return "4.2";
        case REPLICA_SET_TRANSACTIONS:
            return "4.0";
        case SUPPORTS_OP_MSG:
            return "3.6";
        case COMMANDS_ACCEPT_WRITE_CONCERN:
            return "3.4";
        case BATCH_COMMANDS:
            return "3.2";
        case FIND_COMMAND:
            return "3.2";
        case RELEASE_2_7_7:
            return "3.0";
        case AGG_RETURNS_CURSORS:
            return "2.6";
        case RELEASE_2_4_AND_BEFORE:
            return "2.4";
        default:
            MONGO_UNREACHABLE;
    }
}

void TopologyDescription::calculateLogicalSessionTimeout() {
    int min = INT_MAX;
    bool foundNone = false;
    bool hasDataBearingServer = false;

    invariant(_servers.size() > 0);
    for (auto description : _servers) {
        if (!description->isDataBearingServer()) {
            continue;
        }
        hasDataBearingServer = true;

        auto logicalSessionTimeout = description->getLogicalSessionTimeoutMinutes();
        if (!logicalSessionTimeout) {
            foundNone = true;
            break;
        }
        min = std::min(*logicalSessionTimeout, min);
    }
    _logicalSessionTimeoutMinutes =
        (foundNone || !hasDataBearingServer) ? boost::none : boost::make_optional(min);
}

BSONObj TopologyDescription::toBSON() {
    BSONObjBuilder bson;

    bson << "id" << _id.toString();
    bson << "topologyType" << mongo::sdam::toString(_type);

    BSONObjBuilder bsonServers;
    for (auto server : this->getServers()) {
        bsonServers << server->getAddress() << server->toBson();
    }
    bson.append("servers", bsonServers.obj());

    if (_logicalSessionTimeoutMinutes) {
        bson << "logicalSessionTimeoutMinutes" << *_logicalSessionTimeoutMinutes;
    }

    if (_setName) {
        bson << "setName" << *_setName;
    }

    if (_compatible) {
        bson << "compatible" << true;
    } else {
        bson << "compatible" << false;
        bson << "compatibleError" << *_compatibleError;
    }

    if (_maxSetVersion) {
        bson << "maxSetVersion" << *_maxSetVersion;
    }

    if (_maxElectionId) {
        bson << "maxElectionId" << *_maxElectionId;
    }

    return bson.obj();
}

std::string TopologyDescription::toString() {
    return toBSON().toString();
}

////////////////////////
// SdamConfiguration
////////////////////////
SdamConfiguration::SdamConfiguration(boost::optional<std::vector<ServerAddress>> seedList,
                                     TopologyType initialType,
                                     mongo::Milliseconds heartBeatFrequencyMs,
                                     boost::optional<std::string> setName)
    : _seedList(seedList),
      _initialType(initialType),
      _heartBeatFrequencyMs(heartBeatFrequencyMs),
      _setName(setName) {
    uassert(ErrorCodes::InvalidSeedList,
            "seed list size must be >= 1",
            !seedList || (*seedList).size() >= 1);

    uassert(ErrorCodes::InvalidSeedList,
            "TopologyType Single must have exactly one entry in the seed list.",
            _initialType != TopologyType::kSingle || (*seedList).size() == 1);

    uassert(
        ErrorCodes::InvalidTopologyType,
        "Only ToplogyTypes ReplicaSetNoPrimary and Single are allowed when a setName is provided.",
        !_setName ||
            (_initialType == TopologyType::kReplicaSetNoPrimary ||
             _initialType == TopologyType::kSingle));

    uassert(ErrorCodes::TopologySetNameRequired,
            "setName is required for ReplicaSetNoPrimary",
            _initialType != TopologyType::kReplicaSetNoPrimary || _setName);

    uassert(ErrorCodes::InvalidHeartBeatFrequency,
            "topology heartbeat must be >= 500ms",
            _heartBeatFrequencyMs >= kMinHeartbeatFrequencyMS);
}

const boost::optional<std::vector<ServerAddress>>& SdamConfiguration::getSeedList() const {
    return _seedList;
}

TopologyType SdamConfiguration::getInitialType() const {
    return _initialType;
}

Milliseconds SdamConfiguration::getHeartBeatFrequency() const {
    return _heartBeatFrequencyMs;
}

const boost::optional<std::string>& SdamConfiguration::getSetName() const {
    return _setName;
}
};  // namespace mongo::sdam
