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

#include <boost/date_time/posix_time/ptime.hpp>
#include <boost/optional.hpp>
#include <cstdint>

#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"

namespace mongo {

class BSONObj;
class MigrationSecondaryThrottleOptions;
class OperationContext;
class Status;
template <typename T>
class StatusWith;

/**
 * Utility class to parse the balancer settings document, which has the following format:
 *
 * balancer: {
 *  stopped: <true|false>,
 *  mode: <full|autoSplitOnly|off>,  // Only consulted if "stopped" is missing or
 * false activeWindow: { start: "<HH:MM>", stop: "<HH:MM>" }
 * }
 */
class BalancerSettingsType {
public:
    // Supported balancer modes
    enum BalancerMode {
        kFull,           // Balancer will always try to keep the cluster even
        kAutoSplitOnly,  // Only balance on auto splits
        kOff,            // Balancer is completely off
    };

    // The key under which this setting is stored on the config server
    static const char kKey[];

    // String representation of the balancer modes
    static const char* kBalancerModes[];

    /**
     * Constructs a settings object with the default values. To be used when no balancer settings
     * have been specified.
     */
    static BalancerSettingsType createDefault();

    /**
     * Interprets the BSON content as balancer settings and extracts the respective values.
     */
    static StatusWith<BalancerSettingsType> fromBSON(const BSONObj& obj);

    /**
     * Returns whether the balancer is enabled.
     */
    BalancerMode getMode() const {
        return _mode;
    }

    /**
     * Returns true if either 'now' is in the balancing window or if no balancing window exists.
     */
    bool isTimeInBalancingWindow(const boost::posix_time::ptime& now) const;

    /**
     * Returns the secondary throttle options.
     */
    const MigrationSecondaryThrottleOptions& getSecondaryThrottle() const {
        return _secondaryThrottle;
    }

    /**
     * Returns whether the balancer should wait for deletions after each completed move.
     */
    bool waitForDelete() const {
        return _waitForDelete;
    }

    /**
     * Returns whether the balancer should schedule migrations of chunks that are 'large' rather
     * than marking these chunks as 'jumbo' (meaning they will not be scheduled for split or
     * migration).
     */
    bool attemptToBalanceJumboChunks() const {
        return _attemptToBalanceJumboChunks;
    }

private:
    BalancerSettingsType();

    BalancerMode _mode{kFull};

    boost::optional<boost::posix_time::ptime> _activeWindowStart;
    boost::optional<boost::posix_time::ptime> _activeWindowStop;

    MigrationSecondaryThrottleOptions _secondaryThrottle;

    bool _waitForDelete{false};

    bool _attemptToBalanceJumboChunks{false};
};

/**
 * Utility class to parse the chunk size settings document, which has the following format:
 *
 * chunksize: { value: <value in MB between 1 and 1024> }
 */
class ChunkSizeSettingsType {
public:
    // The key under which this setting is stored on the config server
    static const char kKey[];

    // Default value to use for the max chunk size if one is not specified in the balancer
    // configuration
    static const uint64_t kDefaultMaxChunkSizeBytes;

    /**
     * Constructs a settings object with the default values. To be used when no chunk size settings
     * have been specified.
     */
    static ChunkSizeSettingsType createDefault();

    /**
     * Interprets the BSON content as chunk size settings and extracts the respective values.
     */
    static StatusWith<ChunkSizeSettingsType> fromBSON(const BSONObj& obj);

    uint64_t getMaxChunkSizeBytes() const {
        return _maxChunkSizeBytes;
    }

    /**
     * Validates that the specified max chunk size value (in bytes) is allowed.
     */
    static bool checkMaxChunkSizeValid(uint64_t maxChunkSizeBytes);

private:
    ChunkSizeSettingsType();

    uint64_t _maxChunkSizeBytes{kDefaultMaxChunkSizeBytes};
};

/**
 * Utility class to parse the sharding autoSplit settings document, which has the following format:
 *
 * autosplit: { enabled: <true|false> }
 */
class AutoSplitSettingsType {
public:
    // The key under which this setting is stored on the config server
    static const char kKey[];

    /**
     * Constructs a settings object with the default values. To be used when no AutoSplit settings
     * have been specified.
     */
    static AutoSplitSettingsType createDefault();

    /**
     * Interprets the BSON content as autosplit settings and extracts the respective values
     */
    static StatusWith<AutoSplitSettingsType> fromBSON(const BSONObj& obj);

    bool getShouldAutoSplit() const {
        return _shouldAutoSplit;
    }

private:
    AutoSplitSettingsType();

    bool _shouldAutoSplit{true};
};

/**
 * Utility class to parse the sharding autoMerge settings document, which has the following format:
 *
 * automerge: { enabled: <true|false> }
 */
class AutoMergeSettingsType {
public:
    // The key under which this setting is stored on the config server
    static const char kKey[];

    AutoMergeSettingsType() = default;

    /**
     * Constructs a settings object with the default values. To be used when no AutoMerge settings
     * have been specified.
     */
    static AutoMergeSettingsType createDefault() {
        return AutoMergeSettingsType();
    }

    /**
     * Interprets the BSON content as autoMerge settings and extracts the respective values
     */
    static StatusWith<AutoMergeSettingsType> fromBSON(const BSONObj& obj);

    bool isEnabled() const {
        return _isEnabled;
    }

private:
    bool _isEnabled{true};
};

/**
 * Contains settings, which control the behaviour of the balancer.
 */
class BalancerConfiguration {
    BalancerConfiguration(const BalancerConfiguration&) = delete;
    BalancerConfiguration& operator=(const BalancerConfiguration&) = delete;

public:
    /**
     * Primes the balancer configuration with some default values. The effective settings may change
     * at a later time after a call to refresh().
     */
    BalancerConfiguration();
    ~BalancerConfiguration();

    /**
     * Non-blocking method, which checks whether the balancer is enabled (without checking for the
     * balancing window).
     */
    BalancerSettingsType::BalancerMode getBalancerMode() const;

    /**
     * Synchronous method, which writes the balancer mode to the configuration data.
     */
    Status setBalancerMode(OperationContext* opCtx, BalancerSettingsType::BalancerMode mode);

    /**
     * Returns whether balancing is allowed based on both the enabled state of the balancer and the
     * balancing window.
     */
    bool shouldBalance() const;
    bool shouldBalanceForAutoSplit() const;
    bool shouldBalanceForAutoMerge() const;

    /**
     * Returns the secondary throttle options for the balancer.
     */
    MigrationSecondaryThrottleOptions getSecondaryThrottle() const;

    /**
     * Returns whether the balancer should wait for deletion of orphaned chunk data at the end of
     * each migration.
     */
    bool waitForDelete() const;

    /**
     * Returns whether the balancer should attempt to schedule migrations of 'large' chunks. If
     * false, the balancer will instead mark these chunks as 'jumbo', meaning they will not be
     * scheduled for any split or move in the future.
     */
    bool attemptToBalanceJumboChunks() const;

    /**
     * Returns the max chunk size after which a chunk would be considered jumbo.
     */
    uint64_t getMaxChunkSizeBytes() const {
        return _maxChunkSizeBytes.loadRelaxed();
    }

    /**
     * Change the cluster wide auto split settings.
     */
    Status enableAutoSplit(OperationContext* opCtx, bool enable);

    bool getShouldAutoSplit() const {
        return _shouldAutoSplit.loadRelaxed();
    }

    /**
     * Change the cluster wide auto merge settings.
     */
    Status changeAutoMergeSettings(OperationContext* opCtx, bool enable);

    bool shouldAutoMerge() const {
        return _shouldAutoMerge.loadRelaxed();
    }

    /**
     * Blocking method, which refreshes the balancer configuration from the settings in the
     * config.settings collection. It will stop at the first bad configuration value and return an
     * error indicating what failed. The value for the bad configuration and the ones after it will
     * remain unchanged.
     *
     * This method is thread-safe but it doesn't make sense to be called from more than one thread
     * at a time.
     */
    Status refreshAndCheck(OperationContext* opCtx);

private:
    /**
     * Reloads the balancer configuration from the settings document. Fails if the settings document
     * cannot be read, in which case the values will remain unchanged.
     */
    Status _refreshBalancerSettings(OperationContext* opCtx);

    /**
     * Reloads the chunk sizes configuration from the settings document. Fails if the settings
     * document cannot be read or if any setting contains invalid value, in which case the offending
     * value will remain unchanged.
     */
    Status _refreshChunkSizeSettings(OperationContext* opCtx);

    /**
     * Reloads the autosplit configuration from the settings document. Fails if the settings
     * document cannot be read.
     */
    Status _refreshAutoSplitSettings(OperationContext* opCtx);

    /**
     * Reloads the autoMerge configuration from the settings document. Fails if the settings
     * document cannot be read.
     */
    Status _refreshAutoMergeSettings(OperationContext* opCtx);

    // The latest read balancer settings and a mutex to protect its swaps
    mutable Mutex _balancerSettingsMutex =
        MONGO_MAKE_LATCH("BalancerConfiguration::_balancerSettingsMutex");
    BalancerSettingsType _balancerSettings;

    // Max chunk size after which a chunk would be considered jumbo and won't be moved. This value
    // is read on the critical path after each write operation, that's why it is cached.
    AtomicWord<unsigned long long> _maxChunkSizeBytes;
    AtomicWord<bool> _shouldAutoSplit;
    AtomicWord<bool> _shouldAutoMerge;
};

}  // namespace mongo
