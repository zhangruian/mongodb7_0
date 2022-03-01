// Copyright 2019 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_REGEXP_REGEXP_BYTECODE_PEEPHOLE_H_
#define V8_REGEXP_REGEXP_BYTECODE_PEEPHOLE_H_

#include "irregexp/RegExpShim.h"

namespace v8 {
namespace internal {

class ByteArray;

// Peephole optimization for regexp interpreter bytecode.
// Pre-defined bytecode sequences occuring in the bytecode generated by the
// RegExpBytecodeGenerator can be optimized into a single bytecode.
class RegExpBytecodePeepholeOptimization : public AllStatic {
 public:
  // Performs peephole optimization on the given bytecode and returns the
  // optimized bytecode.
  static Handle<ByteArray> OptimizeBytecode(
      Isolate* isolate, Zone* zone, Handle<String> source, const byte* bytecode,
      int length, const ZoneUnorderedMap<int, int>& jump_edges);
};

}  // namespace internal
}  // namespace v8

#endif  // V8_REGEXP_REGEXP_BYTECODE_PEEPHOLE_H_
