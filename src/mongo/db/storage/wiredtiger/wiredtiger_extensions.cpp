/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_extensions.h"

#include "mongo/base/init.h"
#include "mongo/base/string_data.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"

namespace mongo {

MONGO_INITIALIZER(SetWiredTigerExtensions)
(InitializerContext* context) {
    auto configHooks = stdx::make_unique<WiredTigerExtensions>();
    WiredTigerExtensions::set(getGlobalServiceContext(), std::move(configHooks));

    return Status::OK();
}

namespace {
const auto getConfigHooks =
    ServiceContext::declareDecoration<std::unique_ptr<WiredTigerExtensions>>();
}  // namespace

void WiredTigerExtensions::set(ServiceContext* service,
                               std::unique_ptr<WiredTigerExtensions> configHooks) {
    auto& hooks = getConfigHooks(service);
    invariant(configHooks);
    hooks = std::move(configHooks);
}

WiredTigerExtensions* WiredTigerExtensions::get(ServiceContext* service) {
    return getConfigHooks(service).get();
}

std::string WiredTigerExtensions::getOpenExtensionsConfig() const {
    if (_wtExtensions.size() == 0) {
        return "";
    }

    StringBuilder extensions;
    extensions << "extensions=[";
    for (const auto& ext : _wtExtensions) {
        extensions << ext << ",";
    }
    extensions << "],";

    return extensions.str();
}

void WiredTigerExtensions::addExtension(StringData extensionConfigStr) {
    _wtExtensions.emplace_back(extensionConfigStr.toString());
}

}  // namespace mongo
