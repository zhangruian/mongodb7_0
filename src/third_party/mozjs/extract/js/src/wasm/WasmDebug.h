/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2016 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_debug_h
#define wasm_debug_h

#include "js/HashTable.h"
#include "wasm/WasmCode.h"
#include "wasm/WasmCodegenTypes.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmExprType.h"
#include "wasm/WasmModule.h"
#include "wasm/WasmTypeDecls.h"
#include "wasm/WasmValType.h"

namespace js {

class Debugger;
class WasmBreakpointSite;
class WasmInstanceObject;

namespace wasm {

struct MetadataTier;

// The generated source location for the AST node/expression. The offset field
// refers an offset in an binary format file.

struct ExprLoc {
  uint32_t lineno;
  uint32_t column;
  uint32_t offset;
  ExprLoc() : lineno(0), column(0), offset(0) {}
  ExprLoc(uint32_t lineno_, uint32_t column_, uint32_t offset_)
      : lineno(lineno_), column(column_), offset(offset_) {}
};

using StepperCounters =
    HashMap<uint32_t, uint32_t, DefaultHasher<uint32_t>, SystemAllocPolicy>;
using WasmBreakpointSiteMap =
    HashMap<uint32_t, WasmBreakpointSite*, DefaultHasher<uint32_t>,
            SystemAllocPolicy>;

/*
 * [SMDOC] Wasm debug traps
 *
 * There is a single debug trap handler for the process, WasmHandleDebugTrap in
 * WasmBuiltins.cpp.  That function is invoked through the Debug Trap Stub,
 * generated by GenerateDebugTrapStub in WasmStubs.cpp.  When any function in an
 * instance needs to trap for any reason (enter frame, leave frame, breakpoint,
 * or single-stepping) then a pointer to the Debug Trap Stub is installed in the
 * Instance.  Debug-enabled code will look for this pointer and call it if it is
 * not null.
 *
 * WasmHandleDebugTrap may therefore be called very frequently when any function
 * in the instance is being debugged, and must filter the trap against the
 * tables in the DebugState.  It can make use of the return address for the
 * call, which identifies the site uniquely.
 *
 * In order to greatly reduce the frequency of calls to the Debug Trap Stub, an
 * array of flag bits, one per function, is attached to the instance.  The code
 * at the breakable point calls a shared stub within the function containing the
 * breakable point to check whether the bit is set for the function.  If it is
 * not set, the stub can return to its caller immediately; if the bit is set,
 * the stub will jump to the installed Debug Trap Stub.
 */

class DebugState {
  const SharedCode code_;
  const SharedModule module_;

  // State maintained when debugging is enabled.

  bool enterFrameTrapsEnabled_;
  uint32_t enterAndLeaveFrameTrapsCounter_;
  WasmBreakpointSiteMap breakpointSites_;
  StepperCounters stepperCounters_;

  void enableDebuggingForFunction(Instance* instance, uint32_t funcIndex);
  void disableDebuggingForFunction(Instance* instance, uint32_t funcIndex);
  void enableDebugTrap(Instance* instance);
  void disableDebugTrap(Instance* instance);

 public:
  DebugState(const Code& code, const Module& module);

  void trace(JSTracer* trc);
  void finalize(JS::GCContext* gcx);

  const Bytes& bytecode() const { return module_->debugBytecode(); }

  [[nodiscard]] bool getLineOffsets(size_t lineno, Vector<uint32_t>* offsets);
  [[nodiscard]] bool getAllColumnOffsets(Vector<ExprLoc>* offsets);
  [[nodiscard]] bool getOffsetLocation(uint32_t offset, size_t* lineno,
                                       size_t* column);

  // The Code can track enter/leave frame events. Any such event triggers
  // debug trap. The enter/leave frame events enabled or disabled across
  // all functions.

  void adjustEnterAndLeaveFrameTrapsState(JSContext* cx, Instance* instance,
                                          bool enabled);
  void ensureEnterFrameTrapsState(JSContext* cx, Instance* instance,
                                  bool enabled);
  bool enterFrameTrapsEnabled() const { return enterFrameTrapsEnabled_; }

  // When the Code is debugEnabled, individual breakpoints can be enabled or
  // disabled at instruction offsets.

  bool hasBreakpointTrapAtOffset(uint32_t offset);
  void toggleBreakpointTrap(JSRuntime* rt, Instance* instance, uint32_t offset,
                            bool enabled);
  WasmBreakpointSite* getBreakpointSite(uint32_t offset) const;
  WasmBreakpointSite* getOrCreateBreakpointSite(JSContext* cx,
                                                Instance* instance,
                                                uint32_t offset);
  bool hasBreakpointSite(uint32_t offset);
  void destroyBreakpointSite(JS::GCContext* gcx, Instance* instance,
                             uint32_t offset);
  void clearBreakpointsIn(JS::GCContext* gcx, WasmInstanceObject* instance,
                          js::Debugger* dbg, JSObject* handler);

  // When the Code is debug-enabled, single-stepping mode can be toggled on
  // the granularity of individual functions.

  bool stepModeEnabled(uint32_t funcIndex) const;
  [[nodiscard]] bool incrementStepperCount(JSContext* cx, Instance* instance,
                                           uint32_t funcIndex);
  void decrementStepperCount(JS::GCContext* gcx, Instance* instance,
                             uint32_t funcIndex);

  // Stack inspection helpers.

  [[nodiscard]] bool debugGetLocalTypes(uint32_t funcIndex,
                                        ValTypeVector* locals,
                                        size_t* argsLength,
                                        StackResults* stackResults);
  [[nodiscard]] bool getGlobal(Instance& instance, uint32_t globalIndex,
                               MutableHandleValue vp);

  // Debug URL helpers.

  [[nodiscard]] bool getSourceMappingURL(JSContext* cx,
                                         MutableHandleString result) const;

  // Accessors for commonly used elements of linked structures.

  const MetadataTier& metadata(Tier t) const { return code_->metadata(t); }
  const Metadata& metadata() const { return code_->metadata(); }
  const CodeRangeVector& codeRanges(Tier t) const {
    return metadata(t).codeRanges;
  }
  const CallSiteVector& callSites(Tier t) const {
    return metadata(t).callSites;
  }

  uint32_t funcToCodeRangeIndex(uint32_t funcIndex) const {
    return metadata(Tier::Debug).funcToCodeRange[funcIndex];
  }

  // about:memory reporting:

  void addSizeOfMisc(MallocSizeOf mallocSizeOf, Metadata::SeenSet* seenMetadata,
                     Code::SeenSet* seenCode, size_t* code, size_t* data) const;
};

using UniqueDebugState = UniquePtr<DebugState>;

}  // namespace wasm
}  // namespace js

#endif  // wasm_debug_h
