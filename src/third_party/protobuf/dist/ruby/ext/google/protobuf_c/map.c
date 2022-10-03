// Protocol Buffers - Google's data interchange format
// Copyright 2014 Google Inc.  All rights reserved.
// https://developers.google.com/protocol-buffers/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "convert.h"
#include "defs.h"
#include "message.h"
#include "protobuf.h"

// -----------------------------------------------------------------------------
// Basic map operations on top of upb_map.
//
// Note that we roll our own `Map` container here because, as for
// `RepeatedField`, we want a strongly-typed container. This is so that any user
// errors due to incorrect map key or value types are raised as close as
// possible to the error site, rather than at some deferred point (e.g.,
// serialization).
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Map container type.
// -----------------------------------------------------------------------------

typedef struct {
  const upb_map *map;  // Can convert to mutable when non-frozen.
  upb_fieldtype_t key_type;
  TypeInfo value_type_info;
  VALUE value_type_class;
  VALUE arena;
} Map;

static void Map_mark(void* _self) {
  Map* self = _self;
  rb_gc_mark(self->value_type_class);
  rb_gc_mark(self->arena);
}

const rb_data_type_t Map_type = {
  "Google::Protobuf::Map",
  { Map_mark, RUBY_DEFAULT_FREE, NULL },
  .flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

VALUE cMap;

static Map* ruby_to_Map(VALUE _self) {
  Map* self;
  TypedData_Get_Struct(_self, Map, &Map_type, self);
  return self;
}

static VALUE Map_alloc(VALUE klass) {
  Map* self = ALLOC(Map);
  self->map = NULL;
  self->value_type_class = Qnil;
  self->value_type_info.def.msgdef = NULL;
  self->arena = Qnil;
  return TypedData_Wrap_Struct(klass, &Map_type, self);
}

VALUE Map_GetRubyWrapper(upb_map* map, upb_fieldtype_t key_type,
                         TypeInfo value_type, VALUE arena) {
  PBRUBY_ASSERT(map);

  VALUE val = ObjectCache_Get(map);

  if (val == Qnil) {
    val = Map_alloc(cMap);
    Map* self;
    ObjectCache_Add(map, val);
    TypedData_Get_Struct(val, Map, &Map_type, self);
    self->map = map;
    self->arena = arena;
    self->key_type = key_type;
    self->value_type_info = value_type;
    if (self->value_type_info.type == UPB_TYPE_MESSAGE) {
      const upb_msgdef *val_m = self->value_type_info.def.msgdef;
      self->value_type_class = Descriptor_DefToClass(val_m);
    }
  }

  return val;
}

static VALUE Map_new_this_type(Map *from) {
  VALUE arena_rb = Arena_new();
  upb_map* map = upb_map_new(Arena_get(arena_rb), from->key_type,
                             from->value_type_info.type);
  VALUE ret =
      Map_GetRubyWrapper(map, from->key_type, from->value_type_info, arena_rb);
  PBRUBY_ASSERT(ruby_to_Map(ret)->value_type_class == from->value_type_class);
  return ret;
}

static TypeInfo Map_keyinfo(Map* self) {
  TypeInfo ret;
  ret.type = self->key_type;
  ret.def.msgdef = NULL;
  return ret;
}

static upb_map *Map_GetMutable(VALUE _self) {
  rb_check_frozen(_self);
  return (upb_map*)ruby_to_Map(_self)->map;
}

VALUE Map_CreateHash(const upb_map* map, upb_fieldtype_t key_type,
                     TypeInfo val_info) {
  VALUE hash = rb_hash_new();
  size_t iter = UPB_MAP_BEGIN;
  TypeInfo key_info = TypeInfo_from_type(key_type);

  if (!map) return hash;

  while (upb_mapiter_next(map, &iter)) {
    upb_msgval key = upb_mapiter_key(map, iter);
    upb_msgval val = upb_mapiter_value(map, iter);
    VALUE key_val = Convert_UpbToRuby(key, key_info, Qnil);
    VALUE val_val = Scalar_CreateHash(val, val_info);
    rb_hash_aset(hash, key_val, val_val);
  }

  return hash;
}

VALUE Map_deep_copy(VALUE obj) {
  Map* self = ruby_to_Map(obj);
  VALUE new_arena_rb = Arena_new();
  upb_arena *arena = Arena_get(new_arena_rb);
  upb_map* new_map =
      upb_map_new(arena, self->key_type, self->value_type_info.type);
  size_t iter = UPB_MAP_BEGIN;
  while (upb_mapiter_next(self->map, &iter)) {
    upb_msgval key = upb_mapiter_key(self->map, iter);
    upb_msgval val = upb_mapiter_value(self->map, iter);
    upb_msgval val_copy = Msgval_DeepCopy(val, self->value_type_info, arena);
    upb_map_set(new_map, key, val_copy, arena);
  }

  return Map_GetRubyWrapper(new_map, self->key_type, self->value_type_info,
                            new_arena_rb);
}

const upb_map* Map_GetUpbMap(VALUE val, const upb_fielddef *field) {
  const upb_fielddef* key_field = map_field_key(field);
  const upb_fielddef* value_field = map_field_value(field);
  TypeInfo value_type_info = TypeInfo_get(value_field);
  Map* self;

  if (!RB_TYPE_P(val, T_DATA) || !RTYPEDDATA_P(val) ||
      RTYPEDDATA_TYPE(val) != &Map_type) {
    rb_raise(cTypeError, "Expected Map instance");
  }

  self = ruby_to_Map(val);
  if (self->key_type != upb_fielddef_type(key_field)) {
    rb_raise(cTypeError, "Map key type does not match field's key type");
  }
  if (self->value_type_info.type != value_type_info.type) {
    rb_raise(cTypeError, "Map value type does not match field's value type");
  }
  if (self->value_type_info.def.msgdef != value_type_info.def.msgdef) {
    rb_raise(cTypeError, "Map value type has wrong message/enum class");
  }

  return self->map;
}

void Map_Inspect(StringBuilder* b, const upb_map* map, upb_fieldtype_t key_type,
                 TypeInfo val_type) {
  bool first = true;
  TypeInfo key_type_info = {key_type};
  StringBuilder_Printf(b, "{");
  if (map) {
    size_t iter = UPB_MAP_BEGIN;
    while (upb_mapiter_next(map, &iter)) {
      upb_msgval key = upb_mapiter_key(map, iter);
      upb_msgval val = upb_mapiter_value(map, iter);
      if (first) {
        first = false;
      } else {
        StringBuilder_Printf(b, ", ");
      }
      StringBuilder_PrintMsgval(b, key, key_type_info);
      StringBuilder_Printf(b, "=>");
      StringBuilder_PrintMsgval(b, val, val_type);
    }
  }
  StringBuilder_Printf(b, "}");
}

static int merge_into_self_callback(VALUE key, VALUE val, VALUE _self) {
  Map* self = ruby_to_Map(_self);
  upb_arena *arena = Arena_get(self->arena);
  upb_msgval key_val = Convert_RubyToUpb(key, "", Map_keyinfo(self), arena);
  upb_msgval val_val = Convert_RubyToUpb(val, "", self->value_type_info, arena);
  upb_map_set(Map_GetMutable(_self), key_val, val_val, arena);
  return ST_CONTINUE;
}

// Used only internally -- shared by #merge and #initialize.
static VALUE Map_merge_into_self(VALUE _self, VALUE hashmap) {
  if (TYPE(hashmap) == T_HASH) {
    rb_hash_foreach(hashmap, merge_into_self_callback, _self);
  } else if (RB_TYPE_P(hashmap, T_DATA) && RTYPEDDATA_P(hashmap) &&
             RTYPEDDATA_TYPE(hashmap) == &Map_type) {
    Map* self = ruby_to_Map(_self);
    Map* other = ruby_to_Map(hashmap);
    upb_arena *arena = Arena_get(self->arena);
    upb_msg *self_msg = Map_GetMutable(_self);
    size_t iter = UPB_MAP_BEGIN;

    upb_arena_fuse(arena, Arena_get(other->arena));

    if (self->key_type != other->key_type ||
        self->value_type_info.type != other->value_type_info.type ||
        self->value_type_class != other->value_type_class) {
      rb_raise(rb_eArgError, "Attempt to merge Map with mismatching types");
    }

    while (upb_mapiter_next(other->map, &iter)) {
      upb_msgval key = upb_mapiter_key(other->map, iter);
      upb_msgval val = upb_mapiter_value(other->map, iter);
      upb_map_set(self_msg, key, val, arena);
    }
  } else {
    rb_raise(rb_eArgError, "Unknown type merging into Map");
  }
  return _self;
}

/*
 * call-seq:
 *     Map.new(key_type, value_type, value_typeclass = nil, init_hashmap = {})
 *     => new map
 *
 * Allocates a new Map container. This constructor may be called with 2, 3, or 4
 * arguments. The first two arguments are always present and are symbols (taking
 * on the same values as field-type symbols in message descriptors) that
 * indicate the type of the map key and value fields.
 *
 * The supported key types are: :int32, :int64, :uint32, :uint64, :bool,
 * :string, :bytes.
 *
 * The supported value types are: :int32, :int64, :uint32, :uint64, :bool,
 * :string, :bytes, :enum, :message.
 *
 * The third argument, value_typeclass, must be present if value_type is :enum
 * or :message. As in RepeatedField#new, this argument must be a message class
 * (for :message) or enum module (for :enum).
 *
 * The last argument, if present, provides initial content for map. Note that
 * this may be an ordinary Ruby hashmap or another Map instance with identical
 * key and value types. Also note that this argument may be present whether or
 * not value_typeclass is present (and it is unambiguously separate from
 * value_typeclass because value_typeclass's presence is strictly determined by
 * value_type). The contents of this initial hashmap or Map instance are
 * shallow-copied into the new Map: the original map is unmodified, but
 * references to underlying objects will be shared if the value type is a
 * message type.
 */
static VALUE Map_init(int argc, VALUE* argv, VALUE _self) {
  Map* self = ruby_to_Map(_self);
  VALUE init_arg;

  // We take either two args (:key_type, :value_type), three args (:key_type,
  // :value_type, "ValueMessageType"), or four args (the above plus an initial
  // hashmap).
  if (argc < 2 || argc > 4) {
    rb_raise(rb_eArgError, "Map constructor expects 2, 3 or 4 arguments.");
  }

  self->key_type = ruby_to_fieldtype(argv[0]);
  self->value_type_info =
      TypeInfo_FromClass(argc, argv, 1, &self->value_type_class, &init_arg);
  self->arena = Arena_new();

  // Check that the key type is an allowed type.
  switch (self->key_type) {
    case UPB_TYPE_INT32:
    case UPB_TYPE_INT64:
    case UPB_TYPE_UINT32:
    case UPB_TYPE_UINT64:
    case UPB_TYPE_BOOL:
    case UPB_TYPE_STRING:
    case UPB_TYPE_BYTES:
      // These are OK.
      break;
    default:
      rb_raise(rb_eArgError, "Invalid key type for map.");
  }

  self->map = upb_map_new(Arena_get(self->arena), self->key_type,
                          self->value_type_info.type);
  ObjectCache_Add(self->map, _self);

  if (init_arg != Qnil) {
    Map_merge_into_self(_self, init_arg);
  }

  return Qnil;
}

/*
 * call-seq:
 *     Map.each(&block)
 *
 * Invokes &block on each |key, value| pair in the map, in unspecified order.
 * Note that Map also includes Enumerable; map thus acts like a normal Ruby
 * sequence.
 */
static VALUE Map_each(VALUE _self) {
  Map* self = ruby_to_Map(_self);
  size_t iter = UPB_MAP_BEGIN;

  while (upb_mapiter_next(self->map, &iter)) {
    upb_msgval key = upb_mapiter_key(self->map, iter);
    upb_msgval val = upb_mapiter_value(self->map, iter);
    VALUE key_val = Convert_UpbToRuby(key, Map_keyinfo(self), self->arena);
    VALUE val_val = Convert_UpbToRuby(val, self->value_type_info, self->arena);
    rb_yield_values(2, key_val, val_val);
  }

  return Qnil;
}

/*
 * call-seq:
 *     Map.keys => [list_of_keys]
 *
 * Returns the list of keys contained in the map, in unspecified order.
 */
static VALUE Map_keys(VALUE _self) {
  Map* self = ruby_to_Map(_self);
  size_t iter = UPB_MAP_BEGIN;
  VALUE ret = rb_ary_new();

  while (upb_mapiter_next(self->map, &iter)) {
    upb_msgval key = upb_mapiter_key(self->map, iter);
    VALUE key_val = Convert_UpbToRuby(key, Map_keyinfo(self), self->arena);
    rb_ary_push(ret, key_val);
  }

  return ret;
}

/*
 * call-seq:
 *     Map.values => [list_of_values]
 *
 * Returns the list of values contained in the map, in unspecified order.
 */
static VALUE Map_values(VALUE _self) {
  Map* self = ruby_to_Map(_self);
  size_t iter = UPB_MAP_BEGIN;
  VALUE ret = rb_ary_new();

  while (upb_mapiter_next(self->map, &iter)) {
    upb_msgval val = upb_mapiter_value(self->map, iter);
    VALUE val_val = Convert_UpbToRuby(val, self->value_type_info, self->arena);
    rb_ary_push(ret, val_val);
  }

  return ret;
}

/*
 * call-seq:
 *     Map.[](key) => value
 *
 * Accesses the element at the given key. Throws an exception if the key type is
 * incorrect. Returns nil when the key is not present in the map.
 */
static VALUE Map_index(VALUE _self, VALUE key) {
  Map* self = ruby_to_Map(_self);
  upb_msgval key_upb = Convert_RubyToUpb(key, "", Map_keyinfo(self), NULL);
  upb_msgval val;

  if (upb_map_get(self->map, key_upb, &val)) {
    return Convert_UpbToRuby(val, self->value_type_info, self->arena);
  } else {
    return Qnil;
  }
}

/*
 * call-seq:
 *     Map.[]=(key, value) => value
 *
 * Inserts or overwrites the value at the given key with the given new value.
 * Throws an exception if the key type is incorrect. Returns the new value that
 * was just inserted.
 */
static VALUE Map_index_set(VALUE _self, VALUE key, VALUE val) {
  Map* self = ruby_to_Map(_self);
  upb_arena *arena = Arena_get(self->arena);
  upb_msgval key_upb = Convert_RubyToUpb(key, "", Map_keyinfo(self), NULL);
  upb_msgval val_upb = Convert_RubyToUpb(val, "", self->value_type_info, arena);

  upb_map_set(Map_GetMutable(_self), key_upb, val_upb, arena);

  return val;
}

/*
 * call-seq:
 *     Map.has_key?(key) => bool
 *
 * Returns true if the given key is present in the map. Throws an exception if
 * the key has the wrong type.
 */
static VALUE Map_has_key(VALUE _self, VALUE key) {
  Map* self = ruby_to_Map(_self);
  upb_msgval key_upb = Convert_RubyToUpb(key, "", Map_keyinfo(self), NULL);

  if (upb_map_get(self->map, key_upb, NULL)) {
    return Qtrue;
  } else {
    return Qfalse;
  }
}

/*
 * call-seq:
 *     Map.delete(key) => old_value
 *
 * Deletes the value at the given key, if any, returning either the old value or
 * nil if none was present. Throws an exception if the key is of the wrong type.
 */
static VALUE Map_delete(VALUE _self, VALUE key) {
  Map* self = ruby_to_Map(_self);
  upb_msgval key_upb = Convert_RubyToUpb(key, "", Map_keyinfo(self), NULL);
  upb_msgval val_upb;
  VALUE ret;

  rb_check_frozen(_self);

  // TODO(haberman): make upb_map_delete() also capable of returning the deleted
  // value.
  if (upb_map_get(self->map, key_upb, &val_upb)) {
    ret = Convert_UpbToRuby(val_upb, self->value_type_info, self->arena);
  } else {
    ret = Qnil;
  }

  upb_map_delete(Map_GetMutable(_self), key_upb);

  return ret;
}

/*
 * call-seq:
 *     Map.clear
 *
 * Removes all entries from the map.
 */
static VALUE Map_clear(VALUE _self) {
  upb_map_clear(Map_GetMutable(_self));
  return Qnil;
}

/*
 * call-seq:
 *     Map.length
 *
 * Returns the number of entries (key-value pairs) in the map.
 */
static VALUE Map_length(VALUE _self) {
  Map* self = ruby_to_Map(_self);
  return ULL2NUM(upb_map_size(self->map));
}

/*
 * call-seq:
 *     Map.dup => new_map
 *
 * Duplicates this map with a shallow copy. References to all non-primitive
 * element objects (e.g., submessages) are shared.
 */
static VALUE Map_dup(VALUE _self) {
  Map* self = ruby_to_Map(_self);
  VALUE new_map_rb = Map_new_this_type(self);
  Map* new_self = ruby_to_Map(new_map_rb);
  size_t iter = UPB_MAP_BEGIN;
  upb_arena *arena = Arena_get(new_self->arena);
  upb_map *new_map = Map_GetMutable(new_map_rb);

  upb_arena_fuse(arena, Arena_get(self->arena));

  while (upb_mapiter_next(self->map, &iter)) {
    upb_msgval key = upb_mapiter_key(self->map, iter);
    upb_msgval val = upb_mapiter_value(self->map, iter);
    upb_map_set(new_map, key, val, arena);
  }

  return new_map_rb;
}

/*
 * call-seq:
 *     Map.==(other) => boolean
 *
 * Compares this map to another. Maps are equal if they have identical key sets,
 * and for each key, the values in both maps compare equal. Elements are
 * compared as per normal Ruby semantics, by calling their :== methods (or
 * performing a more efficient comparison for primitive types).
 *
 * Maps with dissimilar key types or value types/typeclasses are never equal,
 * even if value comparison (for example, between integers and floats) would
 * have otherwise indicated that every element has equal value.
 */
VALUE Map_eq(VALUE _self, VALUE _other) {
  Map* self = ruby_to_Map(_self);
  Map* other;

  // Allow comparisons to Ruby hashmaps by converting to a temporary Map
  // instance. Slow, but workable.
  if (TYPE(_other) == T_HASH) {
    VALUE other_map = Map_new_this_type(self);
    Map_merge_into_self(other_map, _other);
    _other = other_map;
  }

  other = ruby_to_Map(_other);

  if (self == other) {
    return Qtrue;
  }
  if (self->key_type != other->key_type ||
      self->value_type_info.type != other->value_type_info.type ||
      self->value_type_class != other->value_type_class) {
    return Qfalse;
  }
  if (upb_map_size(self->map) != upb_map_size(other->map)) {
    return Qfalse;
  }

  // For each member of self, check that an equal member exists at the same key
  // in other.
  size_t iter = UPB_MAP_BEGIN;
  while (upb_mapiter_next(self->map, &iter)) {
    upb_msgval key = upb_mapiter_key(self->map, iter);
    upb_msgval val = upb_mapiter_value(self->map, iter);
    upb_msgval other_val;
    if (!upb_map_get(other->map, key, &other_val)) {
      // Not present in other map.
      return Qfalse;
    }
    if (!Msgval_IsEqual(val, other_val, self->value_type_info)) {
      // Present but different value.
      return Qfalse;
    }
  }

  return Qtrue;
}

/*
 * call-seq:
 *     Message.freeze => self
 *
 * Freezes the message object. We have to intercept this so we can pin the
 * Ruby object into memory so we don't forget it's frozen.
 */
static VALUE Map_freeze(VALUE _self) {
  Map* self = ruby_to_Map(_self);
  if (!RB_OBJ_FROZEN(_self)) {
    Arena_Pin(self->arena, _self);
    RB_OBJ_FREEZE(_self);
  }
  return _self;
}

/*
 * call-seq:
 *     Map.hash => hash_value
 *
 * Returns a hash value based on this map's contents.
 */
VALUE Map_hash(VALUE _self) {
  Map* self = ruby_to_Map(_self);
  uint64_t hash = 0;

  size_t iter = UPB_MAP_BEGIN;
  TypeInfo key_info = {self->key_type};
  while (upb_mapiter_next(self->map, &iter)) {
    upb_msgval key = upb_mapiter_key(self->map, iter);
    upb_msgval val = upb_mapiter_value(self->map, iter);
    hash = Msgval_GetHash(key, key_info, hash);
    hash = Msgval_GetHash(val, self->value_type_info, hash);
  }

  return LL2NUM(hash);
}

/*
 * call-seq:
 *     Map.to_h => {}
 *
 * Returns a Ruby Hash object containing all the values within the map
 */
VALUE Map_to_h(VALUE _self) {
  Map* self = ruby_to_Map(_self);
  return Map_CreateHash(self->map, self->key_type, self->value_type_info);
}

/*
 * call-seq:
 *     Map.inspect => string
 *
 * Returns a string representing this map's elements. It will be formatted as
 * "{key => value, key => value, ...}", with each key and value string
 * representation computed by its own #inspect method.
 */
VALUE Map_inspect(VALUE _self) {
  Map* self = ruby_to_Map(_self);

  StringBuilder* builder = StringBuilder_New();
  Map_Inspect(builder, self->map, self->key_type, self->value_type_info);
  VALUE ret = StringBuilder_ToRubyString(builder);
  StringBuilder_Free(builder);
  return ret;
}

/*
 * call-seq:
 *     Map.merge(other_map) => map
 *
 * Copies key/value pairs from other_map into a copy of this map. If a key is
 * set in other_map and this map, the value from other_map overwrites the value
 * in the new copy of this map. Returns the new copy of this map with merged
 * contents.
 */
static VALUE Map_merge(VALUE _self, VALUE hashmap) {
  VALUE dupped = Map_dup(_self);
  return Map_merge_into_self(dupped, hashmap);
}

void Map_register(VALUE module) {
  VALUE klass = rb_define_class_under(module, "Map", rb_cObject);
  rb_define_alloc_func(klass, Map_alloc);
  rb_gc_register_address(&cMap);
  cMap = klass;

  rb_define_method(klass, "initialize", Map_init, -1);
  rb_define_method(klass, "each", Map_each, 0);
  rb_define_method(klass, "keys", Map_keys, 0);
  rb_define_method(klass, "values", Map_values, 0);
  rb_define_method(klass, "[]", Map_index, 1);
  rb_define_method(klass, "[]=", Map_index_set, 2);
  rb_define_method(klass, "has_key?", Map_has_key, 1);
  rb_define_method(klass, "delete", Map_delete, 1);
  rb_define_method(klass, "clear", Map_clear, 0);
  rb_define_method(klass, "length", Map_length, 0);
  rb_define_method(klass, "dup", Map_dup, 0);
  rb_define_method(klass, "==", Map_eq, 1);
  rb_define_method(klass, "freeze", Map_freeze, 0);
  rb_define_method(klass, "hash", Map_hash, 0);
  rb_define_method(klass, "to_h", Map_to_h, 0);
  rb_define_method(klass, "inspect", Map_inspect, 0);
  rb_define_method(klass, "merge", Map_merge, 1);
  rb_include_module(klass, rb_mEnumerable);
}
