/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kControl

#include "mongo/logv2/log_util.h"

#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/time_support.h"

#include <string>
#include <vector>

namespace mongo::logv2 {
namespace {
AtomicWord<bool> redactionEnabled{false};
std::vector<LogRotateCallback> logRotateCallbacks;
}  // namespace

void addLogRotator(LogRotateCallback cb) {
    logRotateCallbacks.push_back(std::move(cb));
}

bool rotateLogs(bool renameFiles) {
    // Rotate on both logv1 and logv2 so all files that need rotation gets rotated
    std::string suffix = "." + terseCurrentTime(false);
    LOGV2(23166, "Log rotation initiated", "suffix"_attr = suffix);
    bool success = true;

    // Call each callback in turn.
    // If they fail, they must log why.
    // We only return true if all succeed.
    for (const auto& cb : logRotateCallbacks) {
        auto status = cb(renameFiles, suffix);
        if (!status.isOK()) {
            LOGV2_WARNING(23168, "Log rotation failed", "reason"_attr = status);
            success = false;
        }
    }

    return success;
}

bool shouldRedactLogs() {
    return redactionEnabled.loadRelaxed();
}

void setShouldRedactLogs(bool enabled) {
    redactionEnabled.store(enabled);
}
}  // namespace mongo::logv2
