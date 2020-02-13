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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/str.h"

namespace mongo {

class OperationContext;

/*
 * This class provides a more sensible interface with JavaScript Scope objects. It helps with
 * boilerplate related to calling JS functions from C++ code, and extracting BSON objects from the
 * JS engine.
 */
class JsExecution {
public:
    /**
     * Create or get a pointer to a JsExecution instance, capable of invoking Javascript functions
     * and reading the return value. This will load all stored procedures from database unless
     * 'disableLoadStored' is set on the global ScriptEngine. The JsExecution* returned is owned by
     * 'opCtx'.
     */
    static JsExecution* get(OperationContext* opCtx,
                            const BSONObj& scope,
                            StringData database,
                            bool inMongos,
                            boost::optional<int> jsHeapLimitMB);

    /**
     * Construct with a thread-local scope and initialize with the given scope variables.
     */
    explicit JsExecution(const BSONObj& scopeVars, boost::optional<int> jsHeapLimitMB = boost::none)
        : _scope(getGlobalScriptEngine()->newScopeForCurrentThread(jsHeapLimitMB)) {
        _scopeVars = scopeVars.getOwned();
        _scope->init(&_scopeVars);
        _fnCallTimeoutMillis = internalQueryJavaScriptFnTimeoutMillis.load();
    }

    /**
     * Registers and invokes the javascript function given by 'func' with the arguments 'params' and
     * input object 'thisObj'.
     *
     * This method assumes that the desired function to execute does not return a value.
     */
    void callFunctionWithoutReturn(ScriptingFunction func,
                                   const BSONObj& params,
                                   const BSONObj& thisObj) {
        doCallFunction(func, params, thisObj, true);
    }

    /**
     * Registers and invokes the javascript function given by 'func' with the arguments 'params' and
     * input object 'thisObj'.
     *
     * Returns the value returned by the function.
     */
    Value callFunction(ScriptingFunction func, const BSONObj& params, const BSONObj& thisObj) {
        return doCallFunction(func, params, thisObj, false);
    }

    /**
     * Injects the given function 'emitFn' as a native JS function named 'emit', callable from
     * user-defined functions.
     */
    void injectEmitIfNecessary(NativeFunction emitFn, void* data) {
        if (!_emitCreated) {
            _scope->injectNative("emit", emitFn, data);
            _emitCreated = true;
        }
    }

    Scope* getScope() {
        return _scope.get();
    }

private:
    BSONObj _scopeVars;
    std::unique_ptr<Scope> _scope;
    bool _emitCreated = false;
    int _fnCallTimeoutMillis;

    Value doCallFunction(ScriptingFunction func,
                         const BSONObj& params,
                         const BSONObj& thisObj,
                         bool noReturnVal);
};
}  // namespace mongo
