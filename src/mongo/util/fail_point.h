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

#include <functional>
#include <string>
#include <type_traits>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/duration.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/string_map.h"

namespace mongo {

/**
 *
 * A FailPoint is a hook mechanism allowing testing behavior to occur at prearranged
 * execution points in the server code. They can be activated and deactivated, and
 * configured to hold data.
 *
 * A FailPoint is usually defined by the MONGO_FAIL_POINT_DEFINE(name) macro,
 * which arranges for it to be added to the global failpoint registry.
 *
 * A FailPoint object can have unusual lifetime semantics. It can be marked
 * `immortal`, so that its internal state is never destroyed. This is possible
 * because FailPoint is designed to have only trivially destructible nonstatic
 * data members, and we can choose not to manually destroy the internal state
 * object. This enables server code that is instrumented by an immortal
 * static-duration FailPoint to remain valid even during process shutdown.
 *
 * Sample use:
 *
 * // Defined somewhere:
 * MONGO_FAIL_POINT_DEFINE(failPoint);
 *
 * bool somewhereInTheCode() {
 *   ... do some stuff ...
 *   // The failpoint artificially changes the return value of this function when active.
 *   if (MONGO_unlikely(failPoint.shouldFail()))
 *       return false;
 *   return true;
 * }
 *
 * - or to implement more complex scenarios, use execute/executeIf -
 *
 * bool somewhereInTheCode() {
 *     failPoint.execute([&](const BSONObj& data) {
 *         // The bad things happen here, and can read the injected 'data'.
 *     });
 *     return true;
 * }
 *
 * // scoped() is another way to do it, where lambda isn't suitable, e.g. to cause
 * // a return/continue/break to control the enclosing function.
 * for (auto& user : users) {
 *     // The failpoint can be activated and given a user name, to skip that user.
 *     if (auto sfp = failPoint.scoped(); MONGO_unlikely(sfp.isActive())) {
 *         if (sfp.getData()["user"] == user.name()) {
 *             continue;
 *         }
 *     }
 *     processOneUser(user);
 * }
 *
 * // Rendered compactly with scopedIf where the data serves as an activation filter.
 * for (auto& user : users) {
 *     if (MONGO_unlikely(failPoint.scopedIf([&](auto&& o) {
 *         return o["user"] == user.name();
 *     }).isActive())) {
 *         continue;
 *     }
 *     processOneUser(user);
 * }
 *
 *  The `scopedIf` and `executeIf` members have an advantage over `scoped` and `execute`. They
 *  only affect the `FailPoint` activation counters (relevant to the `nTimes` and `skip` modes)
 *  if the predicate is true.
 *
 * A FailPoint can be configured remotely by a database command.
 * See `src/mongo/db/commands/fail_point_cmd.cpp`.
 *
 */
class FailPoint {
private:
    static constexpr auto kWaitGranularity = Milliseconds(100);

    class Impl;

public:
    using ValType = unsigned;
    enum Mode { off, alwaysOn, random, nTimes, skip };

    struct ModeOptions {
        Mode mode;
        ValType val;
        BSONObj extra;
    };

    // long long values are able to be appended to BSON. If this is using declaration is changed,
    // please make sure that the new type is also BSON-compatible.
    using EntryCountT = long long;

    /**
     * An object representing an active FailPoint's interaction with the code it is
     * instrumenting. It holds reference to its associated FailPoint, ensuring
     * that FailPoint's state doesn't change while a Scoped is attached to it.
     * If `isActive()`, then `getData()` may be called to retrieve injected data.
     * Users don't create these. They are only used within the execute and executeIf
     * functions and returned by the scoped() and scopedIf() functions.
     *
     * Ex:
     *     if (auto scoped = failPoint.scoped(); scoped.isActive()) {
     *         const BSONObj& data = scoped.getData();
     *         //  failPoint injects some behavior, informed by `data`.
     *     }
     */
    class Scoped {
    public:
        Scoped(Impl* impl, bool active, bool holdsRef)
            : _impl(impl), _active(active), _holdsRef(holdsRef) {}

        ~Scoped() {
            if (_holdsRef)
                _impl->closeScoped();
        }

        Scoped(const Scoped&) = delete;
        Scoped& operator=(const Scoped&) = delete;

        /**
         * @return true if fail point is on.
         *
         * Calls to isActive should be placed inside MONGO_unlikely for performance.
         */
        bool isActive() const {
            return _active;
        }

        /**
         * @return the data stored in the fail point.
         *
         * #isActive must be true before you can call this.
         */
        const BSONObj& getData() const {
            fassert(16445, _holdsRef);  // getData without holding a ref
            return _impl->getData();
        }

    private:
        Impl* _impl;
        bool _active;
        bool _holdsRef;
    };

    /**
     * Explicitly resets the seed used for the PRNG in this thread.  If not called on a thread,
     * an instance of SecureRandom is used to seed the PRNG.
     */
    static void setThreadPRNGSeed(int32_t seed);

    /**
     * Parses the {mode, val, extra} from the BSON.
     * obj = {
     *   mode: modeElem // required
     *   data: extra    // optional payload to inject into the FailPoint intercept site.
     * }
     * where `modeElem` is one of:
     *       "off"
     *       "alwaysOn"
     *       {"times" : val}   // active for the next val calls
     *       {"skip" : val}    // skip calls, activate on and after call number (val+1).
     *       {"activationProbability" : val}  // val is in interval [0.0, 1.0]
     */
    static StatusWith<ModeOptions> parseBSON(const BSONObj& obj);

    /**
     * FailPoint state can be kept alive during shutdown by setting `immortal` true.
     * The usual macro definition does this, but FailPoint unit tests do not.
     */
    explicit FailPoint(std::string name, bool immortal = false);

    FailPoint(const FailPoint&) = delete;
    FailPoint& operator=(const FailPoint&) = delete;

    /**
     * If this FailPoint was constructed as `immortal` (FailPoints defined by
     * MONGO_FAIL_POINT_DEFINE are immortal), this destructor does nothing. In
     * that case the FailPoint (and the code it is instrumenting) can operate
     * normally while the process shuts down.
     */
    ~FailPoint();

    const std::string& getName() const {
        return _impl()->getName();
    }

    /**
     * Returns true if fail point is active.
     *
     * @param pred       see `executeIf` for more information.
     *
     * Calls to `shouldFail` should be placed inside MONGO_unlikely for performance.
     *    if (MONGO_unlikely(failpoint.shouldFail())) ...
     */
    template <typename Pred>
    bool shouldFail(Pred&& pred) {
        return _impl()->shouldFail(std::forward<Pred>(pred));
    }

    bool shouldFail() {
        return shouldFail(nullptr);
    }

    /**
     * Changes the settings of this fail point. This will turn off the FailPoint and
     * wait for all references on this FailPoint to go away before modifying it.
     *
     * @param mode  new mode
     * @param val   unsigned having different interpretations depending on the mode:
     *
     *     - off, alwaysOn: ignored
     *     - random: static_cast<int32_t>(std::numeric_limits<int32_t>::max() * p), where
     *           where p is the probability that any given evaluation of the failpoint should
     *           activate.
     *     - nTimes: the number of times this fail point will be active when
     *         #shouldFail/#execute/#scoped are called.
     *     - skip: will become active and remain active after
     *         #shouldFail/#execute/#scoped are called this number of times.
     *
     * @param extra arbitrary BSON object that can be stored to this fail point
     *     that can be referenced afterwards with #getData. Defaults to an empty
     *     document.
     *
     * @returns the number of times the fail point has been entered so far.
     */
    EntryCountT setMode(Mode mode, ValType val = 0, BSONObj extra = {}) {
        return _impl()->setMode(std::move(mode), std::move(val), std::move(extra));
    }

    EntryCountT setMode(ModeOptions opt) {
        return setMode(std::move(opt.mode), std::move(opt.val), std::move(opt.extra));
    }

    /**
     * Waits until the fail point has been entered the desired number of times.
     *
     * @param targetTimesEntered the number of times the fail point has been entered.
     *
     * @returns the number of times the fail point has been entered so far.
     */
    EntryCountT waitForTimesEntered(EntryCountT targetTimesEntered) const noexcept {
        return waitForTimesEntered(Interruptible::notInterruptible(), targetTimesEntered);
    }

    /**
     * Like `waitForTimesEntered`, but interruptible via the `interruptible->sleepFor` mechanism.
     * See `mongo::Interruptible::sleepFor`.
     */
    EntryCountT waitForTimesEntered(Interruptible* interruptible,
                                    EntryCountT targetTimesEntered) const {
        return _impl()->waitForTimesEntered(interruptible, targetTimesEntered);
    }

    /**
     * @returns a BSON object showing the current mode and data stored.
     */
    BSONObj toBSON() const {
        return _impl()->toBSON();
    }

    /**
     * Create a Scoped from this FailPoint.
     * The returned Scoped object will be active if the failpoint is active.
     * If it's active, the returned object can be used to access FailPoint data.
     */
    Scoped scoped() {
        return scopedIf(nullptr);
    }

    /**
     * Create a Scoped from this FailPoint.
     * If `pred(payload)` is true, then the returned Scoped object is active and the
     * FailPoint's activation count is altered (relevant to e.g. the `nTimes` mode). If the
     * predicate is false, an inactive Scoped is returned and this FailPoint's mode is not
     * modified at all.
     * If it's active, the returned object can be used to access FailPoint data.
     * The `pred` should be callable like a `bool pred(const BSONObj&)`.
     */
    template <typename Pred>
    Scoped scopedIf(Pred&& pred) {
        return _impl()->scopedIf(std::forward<Pred>(pred));
    }

    template <typename F>
    void execute(F&& f) {
        return executeIf(f, nullptr);
    }

    /**
     * If `pred(payload)` is true, then `f(payload)` is executed and the FailPoint's
     * activation count is altered (relevant to e.g. the `nTimes` mode). Otherwise, `f`
     * is not executed and this FailPoint's mode is not altered (e.g. `nTimes` isn't
     * consumed).
     * The `pred` should be callable like a `bool pred(const BSONObj&)`.
     */
    template <typename F, typename Pred>
    void executeIf(F&& f, Pred&& pred) {
        auto sfp = scopedIf(std::forward<Pred>(pred));
        if (MONGO_unlikely(sfp.isActive())) {
            std::forward<F>(f)(sfp.getData());
        }
    }

    /**
     * Take short `kWaitGranularity` pauses for as long as the FailPoint is
     * active. Though this makes several accesses to `shouldFail()`, it counts
     * as only one increment in the FailPoint `nTimes` counter.
     */
    void pauseWhileSet() {
        pauseWhileSet(Interruptible::notInterruptible());
    }

    /**
     * Like `pauseWhileSet`, but interruptible via the `interruptible->sleepFor` mechanism.  See
     * `mongo::Interruptible::sleepFor`.
     */
    void pauseWhileSet(Interruptible* interruptible) {
        _impl()->pauseWhileSet(interruptible);
    }

private:
    class Impl {
    private:
        enum ShouldFailEntryMode { kFirstTimeEntered, kEnteredAlready };

        /** Possible return values from `_shouldFailOpenBlock`. */
        enum ShouldFailOpenBlockResult {
            fastOff,      ///< disabled and doesn't need to be closed
            slowOff,      ///< disabled and needs to be closed
            slowOn,       ///< active and needs to be closed
            userIgnored,  ///< active and needs to be closed, but shouldn't be acted on
        };

        static constexpr ValType _kActiveBit = ValType{1} << 31;

    public:
        Impl(std::string name) : _name(std::move(name)) {}

        template <typename Pred>
        bool shouldFail(Pred&& pred) {
            return _shouldFail(kFirstTimeEntered, std::forward<Pred>(pred));
        }

        EntryCountT setMode(Mode mode, ValType val = 0, BSONObj extra = {});

        EntryCountT waitForTimesEntered(Interruptible* interruptible,
                                        EntryCountT targetTimesEntered) const;

        BSONObj toBSON() const;

        template <typename Pred>
        Scoped scopedIf(Pred&& pred) {
            auto ret = _shouldFailOpenBlock(kFirstTimeEntered, std::forward<Pred>(pred));
            bool active = (ret == slowOn);
            bool holdsRef = (ret != fastOff);
            return Scoped(this, active, holdsRef);
        }

        void closeScoped() {
            _shouldFailCloseBlock();
        }

        /** See `FailPoint::pauseWhileSet`. */
        void pauseWhileSet(Interruptible* interruptible) {
            for (auto entryMode = kFirstTimeEntered;
                 MONGO_unlikely(_shouldFail(entryMode, nullptr));
                 entryMode = kEnteredAlready) {
                interruptible->sleepFor(kWaitGranularity);
            }
        }

        /** Return the stored BSONObj. Safe only when this FailPoint is on. */
        const BSONObj& getData() const {
            return _data;
        }

        const std::string& getName() const {
            return _name;
        }

    private:
        void _enable();
        void _disable();

        /** No default parameters. No-Frills shouldFail implementation. */
        template <typename Pred>
        bool _shouldFail(ShouldFailEntryMode entryMode, Pred&& pred) {
            auto ret = _shouldFailOpenBlock(entryMode, std::forward<Pred>(pred));

            if (MONGO_likely(ret == fastOff)) {
                return false;
            }

            _shouldFailCloseBlock();
            return ret == slowOn;
        }

        /**
         * Checks whether fail point is active and increments the reference counter without
         * decrementing it. Must call shouldFailCloseBlock afterwards when the return value
         * is not fastOff. Otherwise, this will remain read-only forever.
         *
         * Note: see `executeIf` for information on `pred`, and `shouldFail` for information
         *       on `entryMode`.
         *
         * @return slowOn if its active and needs to be closed
         *         userIgnored if its active and needs to be closed, but shouldn't be acted on
         *         slowOff if its disabled and needs to be closed
         *         fastOff if its disabled and doesn't need to be closed
         */
        template <typename Pred>
        ShouldFailOpenBlockResult _shouldFailOpenBlock(ShouldFailEntryMode entryMode, Pred&& pred) {
            if (MONGO_likely((_fpInfo.loadRelaxed() & _kActiveBit) == 0)) {
                return fastOff;
            }

            if (entryMode == kEnteredAlready) {
                return _slowShouldFailOpenBlockWithoutIncrementingTimesEntered(
                    std::forward<Pred>(pred));
            }
            return _slowShouldFailOpenBlock(std::forward<Pred>(pred));
        }

        ShouldFailOpenBlockResult _shouldFailOpenBlock(ShouldFailEntryMode entryMode) {
            return _shouldFailOpenBlock(entryMode, nullptr);
        }

        /**
         * Decrements the reference counter.
         * @see #_shouldFailOpenBlock
         */
        void _shouldFailCloseBlock() {
            _fpInfo.subtractAndFetch(1);
        }

        /**
         * slow path for #_shouldFailOpenBlock
         *
         * If a callable is passed, and returns false, this will return userIgnored and avoid
         * altering the mode in any way.  The argument is the fail point payload.
         */
        ShouldFailOpenBlockResult _slowShouldFailOpenBlockWithoutIncrementingTimesEntered(
            std::function<bool(const BSONObj&)> cb) noexcept;

        /**
         * slow path for #_shouldFailOpenBlock
         *
         * Calls _slowShouldFailOpenBlockWithoutIncrementingTimesEntered. If it returns slowOn,
         * increments the number of times the fail point has been entered before returning.
         */
        ShouldFailOpenBlockResult _slowShouldFailOpenBlock(
            std::function<bool(const BSONObj&)> cb) noexcept;

        // Bit layout:
        // 31: tells whether this fail point is active.
        // 0~30: unsigned ref counter for active dynamic instances.
        AtomicWord<std::uint32_t> _fpInfo{0};

        // Total number of times the fail point has been entered.
        AtomicWord<EntryCountT> _timesEntered{0};

        // Invariant: These should be read only if _kActiveBit of _fpInfo is set.
        Mode _mode{off};
        AtomicWord<int> _timesOrPeriod{0};
        BSONObj _data;

        const std::string _name;

        // protects _mode, _timesOrPeriod, _data
        mutable Mutex _modMutex = MONGO_MAKE_LATCH("FailPoint::_modMutex");
    };

    const Impl* _rawImpl() const {
        return reinterpret_cast<const Impl*>(&_implStorage);
    }

    Impl* _rawImpl() {
        return const_cast<Impl*>(std::as_const(*this)._rawImpl());  // Reuse const overload
    }

    const Impl* _impl() const {
        // Relaxed: such violations can only occur during single-threaded static initialization.
        invariant(_ready.loadRelaxed(), "Use of uninitialized FailPoint");
        return _rawImpl();
    }

    Impl* _impl() {
        return const_cast<Impl*>(std::as_const(*this)._impl());  // Reuse const overload
    }

    const bool _immortal;

    /**
     * True only when `_impl()` should succeed.
     * We exploit zero-initialization of statics to detect use-before-init.
     */
    AtomicWord<bool> _ready;

    std::aligned_storage_t<sizeof(Impl), alignof(Impl)> _implStorage;
};

class FailPointRegistry {
public:
    FailPointRegistry();

    /**
     * Adds a new fail point to this registry. Duplicate names are not allowed.
     *
     * @return the status code under these circumstances:
     *     OK - if successful.
     *     51006 - if the given name already exists in this registry.
     *     CannotMutateObject - if this registry is already frozen.
     */
    Status add(FailPoint* failPoint);

    /**
     * @return a registered FailPoint, or nullptr if it was not registered.
     */
    FailPoint* find(StringData name) const;

    /**
     * Freezes this registry from being modified.
     */
    void freeze();

    /**
     * Creates a new FailPointServerParameter for each failpoint in the registry. This allows the
     * failpoint to be set on the command line via --setParameter, but is only allowed when
     * running with '--setParameter enableTestCommands=1'.
     */
    void registerAllFailPointsAsServerParameters();

    /**
     * Sets all registered FailPoints to Mode::off. Used primarily during unit test cleanup to
     * reset the state of all FailPoints set by the unit test. Does not prevent FailPoints from
     * being enabled again after.
     */
    void disableAllFailpoints();

private:
    bool _frozen;
    StringMap<FailPoint*> _fpMap;
};

/**
 * A scope guard that enables a named FailPoint on construction and disables it on destruction.
 */
class FailPointEnableBlock {
public:
    explicit FailPointEnableBlock(StringData failPointName);
    FailPointEnableBlock(StringData failPointName, BSONObj data);
    explicit FailPointEnableBlock(FailPoint* failPoint);
    FailPointEnableBlock(FailPoint* failPoint, BSONObj data);
    ~FailPointEnableBlock();

    FailPointEnableBlock(const FailPointEnableBlock&) = delete;
    FailPointEnableBlock& operator=(const FailPointEnableBlock&) = delete;

    // Const access to the underlying FailPoint
    const FailPoint* failPoint() const {
        return _failPoint;
    }

    // Const access to the underlying FailPoint
    const FailPoint* operator->() const {
        return failPoint();
    }

    // Return the value of timesEntered() when the block was entered
    auto initialTimesEntered() const {
        return _initialTimesEntered;
    }

private:
    FailPoint* const _failPoint;
    FailPoint::EntryCountT _initialTimesEntered;
};

/**
 * Set a fail point in the global registry to a given value via BSON
 * @return the number of times the fail point has been entered so far.
 * @throw DBException corresponding to ErrorCodes::FailPointSetFailed if no failpoint
 * called failPointName exists.
 */
FailPoint::EntryCountT setGlobalFailPoint(const std::string& failPointName, const BSONObj& cmdObj);

/**
 * Registration object for FailPoint. Its static-initializer registers FailPoint `fp`
 * into the `globalFailPointRegistry()` under the specified `name`.
 */
class FailPointRegisterer {
public:
    explicit FailPointRegisterer(FailPoint* fp);
};

FailPointRegistry& globalFailPointRegistry();

/**
 * Convenience macro for defining a fail point and registering it.
 * Must be used at namespace scope, not at local (inside a function) or class scope.
 * Never use in header files, only .cpp files.
 */
#define MONGO_FAIL_POINT_DEFINE(fp)                               \
    ::mongo::FailPoint fp(#fp, true); /* An immortal FailPoint */ \
    ::mongo::FailPointRegisterer fp##failPointRegisterer(&fp);


}  // namespace mongo
