/*
 * Protocol Buffers - Google's data interchange format
 * Copyright 2014 Google Inc.  All rights reserved.
 * https://developers.google.com/protocol-buffers/
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

package com.google.protobuf.jruby;

import com.google.protobuf.Descriptors.Descriptor;
import com.google.protobuf.Descriptors.EnumDescriptor;
import com.google.protobuf.Descriptors.EnumValueDescriptor;
import com.google.protobuf.Descriptors.FieldDescriptor;
import com.google.protobuf.Descriptors.FileDescriptor;
import com.google.protobuf.Descriptors.OneofDescriptor;
import com.google.protobuf.ByteString;
import com.google.protobuf.DynamicMessage;
import com.google.protobuf.InvalidProtocolBufferException;
import com.google.protobuf.Message;
import com.google.protobuf.UnknownFieldSet;
import com.google.protobuf.util.JsonFormat;
import org.jruby.*;
import org.jruby.anno.JRubyMethod;
import org.jruby.exceptions.RaiseException;
import org.jruby.runtime.Block;
import org.jruby.runtime.Helpers;
import org.jruby.runtime.ThreadContext;
import org.jruby.runtime.builtin.IRubyObject;
import org.jruby.util.ByteList;

import java.nio.ByteBuffer;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public class RubyMessage extends RubyObject {
    public RubyMessage(Ruby runtime, RubyClass klazz, Descriptor descriptor) {
        super(runtime, klazz);

        this.descriptor = descriptor;
        this.cRepeatedField = (RubyClass) runtime.getClassFromPath("Google::Protobuf::RepeatedField");
        this.cMap = (RubyClass) runtime.getClassFromPath("Google::Protobuf::Map");
        this.builder = DynamicMessage.newBuilder(descriptor);
        this.fields = new HashMap<FieldDescriptor, IRubyObject>();
        this.oneofCases = new HashMap<OneofDescriptor, FieldDescriptor>();
        this.proto3 = descriptor.getFile().getSyntax() == FileDescriptor.Syntax.PROTO3;
    }

    /*
     * call-seq:
     *     Message.new(kwargs) => new_message
     *
     * Creates a new instance of the given message class. Keyword arguments may be
     * provided with keywords corresponding to field names.
     *
     * Note that no literal Message class exists. Only concrete classes per message
     * type exist, as provided by the #msgclass method on Descriptors after they
     * have been added to a pool. The method definitions described here on the
     * Message class are provided on each concrete message class.
     */
    @JRubyMethod(optional = 1)
    public IRubyObject initialize(final ThreadContext context, IRubyObject[] args) {
        final Ruby runtime = context.runtime;
        if (args.length == 1) {
            if (!(args[0] instanceof RubyHash)) {
                throw runtime.newArgumentError("expected Hash arguments.");
            }
            RubyHash hash = args[0].convertToHash();
            hash.visitAll(new RubyHash.Visitor() {
                @Override
                public void visit(IRubyObject key, IRubyObject value) {
                    if (!(key instanceof RubySymbol) && !(key instanceof RubyString))
                        throw runtime.newTypeError("Expected string or symbols as hash keys in initialization map.");
                    final FieldDescriptor fieldDescriptor = findField(context, key, ignoreUnknownFieldsOnInit);

                    if (value == null || value.isNil()) return;

                    if (Utils.isMapEntry(fieldDescriptor)) {
                        if (!(value instanceof RubyHash))
                            throw runtime.newArgumentError("Expected Hash object as initializer value for map field '" +  key.asJavaString() + "' (given " + value.getMetaClass() + ").");

                        final RubyMap map = newMapForField(context, fieldDescriptor);
                        map.mergeIntoSelf(context, value);
                        fields.put(fieldDescriptor, map);
                    } else if (fieldDescriptor.isRepeated()) {
                        if (!(value instanceof RubyArray))
                            throw runtime.newArgumentError("Expected array as initializer value for repeated field '" +  key.asJavaString() + "' (given " + value.getMetaClass() + ").");
                        fields.put(fieldDescriptor, rubyToRepeatedField(context, fieldDescriptor, value));
                    } else {
                        OneofDescriptor oneof = fieldDescriptor.getContainingOneof();
                        if (oneof != null) {
                            oneofCases.put(oneof, fieldDescriptor);
                        }

                        if (value instanceof RubyHash && fieldDescriptor.getType() == FieldDescriptor.Type.MESSAGE) {
                            RubyDescriptor descriptor = (RubyDescriptor) getDescriptorForField(context, fieldDescriptor);
                            RubyClass typeClass = (RubyClass) descriptor.msgclass(context);
                            value = (IRubyObject) typeClass.newInstance(context, value, Block.NULL_BLOCK);
                            fields.put(fieldDescriptor, value);
                        } else {
                            indexSet(context, key, value);
                        }

                    }
                }
            });
        }
        return this;
    }

    /*
     * call-seq:
     *     Message.[]=(index, value)
     *
     * Sets a field's value by field name. The provided field name should be a
     * string.
     */
    @JRubyMethod(name = "[]=")
    public IRubyObject indexSet(ThreadContext context, IRubyObject fieldName, IRubyObject value) {
        FieldDescriptor fieldDescriptor = findField(context, fieldName);
        return setFieldInternal(context, fieldDescriptor, value);
    }

    /*
     * call-seq:
     *     Message.[](index) => value
     *
     * Accesses a field's value by field name. The provided field name should be a
     * string.
     */
    @JRubyMethod(name = "[]")
    public IRubyObject index(ThreadContext context, IRubyObject fieldName) {
        FieldDescriptor fieldDescriptor = findField(context, fieldName);
        return getFieldInternal(context, fieldDescriptor);
    }

    /*
     * call-seq:
     *     Message.inspect => string
     *
     * Returns a human-readable string representing this message. It will be
     * formatted as "<MessageType: field1: value1, field2: value2, ...>". Each
     * field's value is represented according to its own #inspect method.
     */
    @JRubyMethod(name = {"inspect", "to_s"})
    public IRubyObject inspect() {
        ThreadContext context = getRuntime().getCurrentContext();
        String cname = metaClass.getName();
        String colon = ": ";
        String comma = ", ";
        StringBuilder sb = new StringBuilder("<");
        boolean addComma = false;

        sb.append(cname).append(colon);

        for (FieldDescriptor fd : descriptor.getFields()) {
            if (addComma) {
                sb.append(comma);
            } else {
                addComma = true;
            }

            sb.append(fd.getName()).append(colon);

            IRubyObject value = getFieldInternal(context, fd);
            if (value instanceof RubyBoolean) {
                // Booleans don't implement internal "inspect" methods so have to call handle them manually
                sb.append(value.isTrue() ? "true" : "false");
            } else {
                sb.append(value.inspect());
            }
        }
        sb.append(">");

        return context.runtime.newString(sb.toString());
    }

    /*
     * call-seq:
     *     Message.hash => hash_value
     *
     * Returns a hash value that represents this message's field values.
     */
    @JRubyMethod
    public IRubyObject hash(ThreadContext context) {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            for (FieldDescriptor fd : descriptor.getFields()) {
                digest.update((byte) getFieldInternal(context, fd).hashCode());
            }
            return context.runtime.newFixnum(ByteBuffer.wrap(digest.digest()).getLong());
        } catch (NoSuchAlgorithmException ignore) {
            return context.runtime.newFixnum(System.identityHashCode(this));
        }
    }

    /*
     * call-seq:
     *     Message.==(other) => boolean
     *
     * Performs a deep comparison of this message with another. Messages are equal
     * if they have the same type and if each field is equal according to the :==
     * method's semantics (a more efficient comparison may actually be done if the
     * field is of a primitive type).
     */
    @JRubyMethod(name = {"==", "eql?"})
    public IRubyObject eq(ThreadContext context, IRubyObject other) {
        Ruby runtime = context.runtime;
        if (!(other instanceof RubyMessage))
            return runtime.getFalse();
        RubyMessage message = (RubyMessage) other;
        if (descriptor != message.descriptor) {
            return runtime.getFalse();
        }

        for (FieldDescriptor fdef : descriptor.getFields()) {
            IRubyObject thisVal = getFieldInternal(context, fdef);
            IRubyObject thatVal = message.getFieldInternal(context, fdef);
            IRubyObject ret = thisVal.callMethod(context, "==", thatVal);
            if (!ret.isTrue()) {
                return runtime.getFalse();
            }
        }
        return runtime.getTrue();
    }

    /*
     * call-seq:
     *     Message.method_missing(*args)
     *
     * Provides accessors and setters and methods to clear and check for presence of
     * message fields according to their field names.
     *
     * For any field whose name does not conflict with a built-in method, an
     * accessor is provided with the same name as the field, and a setter is
     * provided with the name of the field plus the '=' suffix. Thus, given a
     * message instance 'msg' with field 'foo', the following code is valid:
     *
     *     msg.foo = 42
     *     puts msg.foo
     *
     * This method also provides read-only accessors for oneofs. If a oneof exists
     * with name 'my_oneof', then msg.my_oneof will return a Ruby symbol equal to
     * the name of the field in that oneof that is currently set, or nil if none.
     *
     * It also provides methods of the form 'clear_fieldname' to clear the value
     * of the field 'fieldname'. For basic data types, this will set the default
     * value of the field.
     *
     * Additionally, it provides methods of the form 'has_fieldname?', which returns
     * true if the field 'fieldname' is set in the message object, else false. For
     * 'proto3' syntax, calling this for a basic type field will result in an error.
     */
    @JRubyMethod(name = "method_missing", rest = true)
    public IRubyObject methodMissing(ThreadContext context, IRubyObject[] args) {
        Ruby runtime = context.runtime;
        String methodName = args[0].asJavaString();

        if (args.length == 1) {
            RubyDescriptor rubyDescriptor = (RubyDescriptor) getDescriptor(context, metaClass);

            // If we find a Oneof return it's name (use lookupOneof because it has an index)
            IRubyObject oneofDescriptor = rubyDescriptor.lookupOneof(context, args[0]);

            if (!oneofDescriptor.isNil()) {
                RubyOneofDescriptor rubyOneofDescriptor = (RubyOneofDescriptor) oneofDescriptor;
                OneofDescriptor ood = rubyOneofDescriptor.getDescriptor();

                // Check to see if we set this through ruby
                FieldDescriptor fieldDescriptor = oneofCases.get(ood);

                if (fieldDescriptor == null) {
                    // See if we set this from decoding a message
                    fieldDescriptor = builder.getOneofFieldDescriptor(ood);

                    if (fieldDescriptor == null) {
                        return context.nil;
                    } else {
                        // Cache it so we don't need to do multiple checks next time
                        oneofCases.put(ood, fieldDescriptor);
                        return runtime.newSymbol(fieldDescriptor.getName());
                    }
                } else {
                    return runtime.newSymbol(fieldDescriptor.getName());
                }
            }

            // If we find a field return its value
            FieldDescriptor fieldDescriptor = descriptor.findFieldByName(methodName);

            if (fieldDescriptor != null) {
                return getFieldInternal(context, fieldDescriptor);
            }

            if (methodName.startsWith(CLEAR_PREFIX)) {
                methodName = methodName.substring(6);
                oneofDescriptor = rubyDescriptor.lookupOneof(context, runtime.newSymbol(methodName));

                if (!oneofDescriptor.isNil()) {
                    fieldDescriptor = oneofCases.get(((RubyOneofDescriptor) oneofDescriptor).getDescriptor());
                }

                if (fieldDescriptor == null) {
                    fieldDescriptor = descriptor.findFieldByName(methodName);
                }

                if (fieldDescriptor != null) {
                    return clearFieldInternal(context, fieldDescriptor);
                }

            } else if (methodName.startsWith(HAS_PREFIX) && methodName.endsWith(QUESTION_MARK)) {
                methodName = methodName.substring(4, methodName.length() - 1); // Trim "has_" and "?" off the field name
                oneofDescriptor = rubyDescriptor.lookupOneof(context, runtime.newSymbol(methodName));
                if (!oneofDescriptor.isNil()) {
                    RubyOneofDescriptor rubyOneofDescriptor = (RubyOneofDescriptor) oneofDescriptor;
                    return oneofCases.containsKey(rubyOneofDescriptor.getDescriptor()) ? runtime.getTrue() : runtime.getFalse();
                }

                fieldDescriptor = descriptor.findFieldByName(methodName);

                if (fieldDescriptor != null &&
                        (!proto3 || fieldDescriptor.getContainingOneof() == null) && // This seems like a bug but its needed to pass the tests...
                        fieldHasPresence(fieldDescriptor)) {
                    return fields.containsKey(fieldDescriptor) ? runtime.getTrue() : runtime.getFalse();
                }

            } else if (methodName.endsWith(AS_VALUE_SUFFIX)) {
                methodName = methodName.substring(0, methodName.length() - 9);
                fieldDescriptor = descriptor.findFieldByName(methodName);

                if (fieldDescriptor != null && isWrappable(fieldDescriptor)) {
                    IRubyObject value = getFieldInternal(context, fieldDescriptor);

                    if (!value.isNil() && value instanceof RubyMessage) {
                        return ((RubyMessage) value).index(context, runtime.newString("value"));
                    }

                    return value;
                }

            } else if (methodName.endsWith(CONST_SUFFIX)) {
                methodName = methodName.substring(0, methodName.length() - 6);
                fieldDescriptor = descriptor.findFieldByName(methodName);

                if (fieldDescriptor.getType() == FieldDescriptor.Type.ENUM) {
                    IRubyObject enumValue = getFieldInternal(context, fieldDescriptor);

                    if (!enumValue.isNil()) {
                        EnumDescriptor enumDescriptor = fieldDescriptor.getEnumType();
                        if (enumValue instanceof RubyRepeatedField) {
                            RubyArray values = (RubyArray) ((RubyRepeatedField) enumValue).toArray(context);
                            RubyArray retValues = runtime.newArray(values.getLength());
                            for (int i = 0; i < values.getLength(); i++) {
                                String val = values.eltInternal(i).toString();
                                retValues.store((long) i, runtime.newFixnum(enumDescriptor.findValueByName(val).getNumber()));
                            }
                            return retValues;
                        }

                        return runtime.newFixnum(enumDescriptor.findValueByName(enumValue.asJavaString()).getNumber());
                    }
                }
            }

        } else if (args.length == 2 && methodName.endsWith(Utils.EQUAL_SIGN)) {

            methodName = methodName.substring(0, methodName.length() - 1); // Trim equals sign
            FieldDescriptor fieldDescriptor = descriptor.findFieldByName(methodName);

            if (fieldDescriptor != null) {
                return setFieldInternal(context, fieldDescriptor, args[1]);
            }

            if (methodName.endsWith(AS_VALUE_SUFFIX)) {
                methodName = methodName.substring(0, methodName.length() - 9);

                fieldDescriptor = descriptor.findFieldByName(methodName);

                if (fieldDescriptor != null) {
                    if (args[1].isNil()) {
                        return setFieldInternal(context, fieldDescriptor, args[1]);
                    }

                    RubyClass typeClass = (RubyClass) ((RubyDescriptor) getDescriptorForField(context, fieldDescriptor)).msgclass(context);
                    RubyMessage msg = (RubyMessage) typeClass.newInstance(context, Block.NULL_BLOCK);
                    msg.indexSet(context, runtime.newString("value"), args[1]);
                    return setFieldInternal(context, fieldDescriptor, msg);
                }
            }

        }

        return Helpers.invokeSuper(context, this, metaClass, "method_missing", args, Block.NULL_BLOCK);
    }

    /**
     * call-seq:
     * Message.dup => new_message
     * Performs a shallow copy of this message and returns the new copy.
     */
    @JRubyMethod
    public IRubyObject dup(ThreadContext context) {
        RubyMessage dup = (RubyMessage) metaClass.newInstance(context, Block.NULL_BLOCK);
        IRubyObject value;
        for (FieldDescriptor fieldDescriptor : this.descriptor.getFields()) {
            if (fieldDescriptor.isRepeated()) {
                dup.fields.put(fieldDescriptor, this.getRepeatedField(context, fieldDescriptor));
            } else if (fields.containsKey(fieldDescriptor)) {
                dup.fields.put(fieldDescriptor, fields.get(fieldDescriptor));
            } else if (this.builder.hasField(fieldDescriptor)) {
                dup.fields.put(fieldDescriptor, wrapField(context, fieldDescriptor, this.builder.getField(fieldDescriptor)));
            }
        }
        return dup;
    }

    /*
     * call-seq:
     *     Message.descriptor => descriptor
     *
     * Class method that returns the Descriptor instance corresponding to this
     * message class's type.
     */
    @JRubyMethod(name = "descriptor", meta = true)
    public static IRubyObject getDescriptor(ThreadContext context, IRubyObject recv) {
        return ((RubyClass) recv).getInstanceVariable(Utils.DESCRIPTOR_INSTANCE_VAR);
    }

    /*
     * call-seq:
     *     MessageClass.encode(msg) => bytes
     *
     * Encodes the given message object to its serialized form in protocol buffers
     * wire format.
     */
    @JRubyMethod(meta = true)
    public static IRubyObject encode(ThreadContext context, IRubyObject recv, IRubyObject value) {
        if (recv != value.getMetaClass()) {
            throw context.runtime.newArgumentError("Tried to encode a " + value.getMetaClass() + " message with " + recv);
        }
        RubyMessage message = (RubyMessage) value;
        return context.runtime.newString(new ByteList(message.build(context).toByteArray()));
    }

    /*
     * call-seq:
     *     MessageClass.decode(data) => message
     *
     * Decodes the given data (as a string containing bytes in protocol buffers wire
     * format) under the interpretration given by this message class's definition
     * and returns a message object with the corresponding field values.
     */
    @JRubyMethod(meta = true)
    public static IRubyObject decode(ThreadContext context, IRubyObject recv, IRubyObject data) {
        byte[] bin = data.convertToString().getBytes();
        RubyMessage ret = (RubyMessage) ((RubyClass) recv).newInstance(context, Block.NULL_BLOCK);
        try {
            ret.builder.mergeFrom(bin);
        } catch (InvalidProtocolBufferException e) {
            throw RaiseException.from(context.runtime, (RubyClass) context.runtime.getClassFromPath("Google::Protobuf::ParseError"), e.getMessage());
        }

        if (!ret.proto3) {
            // Need to reset unknown values in repeated enum fields
            ret.builder.getUnknownFields().asMap().forEach((i, values) -> {
                FieldDescriptor fd = ret.builder.getDescriptorForType().findFieldByNumber(i);
                if (fd != null && fd.isRepeated() && fd.getType() == FieldDescriptor.Type.ENUM) {
                    EnumDescriptor ed = fd.getEnumType();
                    values.getVarintList().forEach(value -> {
                        ret.builder.addRepeatedField(fd, ed.findValueByNumberCreatingIfUnknown(value.intValue()));
                    });
                }
            });
        }

        return ret;
    }

    /*
     * call-seq:
     *     MessageClass.encode_json(msg, options = {}) => json_string
     *
     * Encodes the given message object into its serialized JSON representation.
     * @param options [Hash] options for the decoder
     *  preserve_proto_fieldnames: set true to use original fieldnames (default is to camelCase)
     *  emit_defaults: set true to emit 0/false values (default is to omit them)
     */
    @JRubyMethod(name = "encode_json", required = 1, optional = 1, meta = true)
    public static IRubyObject encodeJson(ThreadContext context, IRubyObject recv, IRubyObject[] args) {
        Ruby runtime = context.runtime;
        RubyMessage message = (RubyMessage) args[0];
        JsonFormat.Printer printer = JsonFormat.printer().omittingInsignificantWhitespace();
        String result;

        if (args.length > 1) {
            RubyHash options = (RubyHash) args[1];
            IRubyObject emitDefaults = options.fastARef(runtime.newSymbol("emit_defaults"));
            IRubyObject preserveNames = options.fastARef(runtime.newSymbol("preserve_proto_fieldnames"));

            if (emitDefaults != null && emitDefaults.isTrue()) {
                printer = printer.includingDefaultValueFields();
            }

            if (preserveNames != null && preserveNames.isTrue()) {
                printer = printer.preservingProtoFieldNames();
            }
        }

        try {
            result = printer.print(message.build(context));
        } catch(InvalidProtocolBufferException e) {
            throw runtime.newRuntimeError(e.getMessage());
        }

        return runtime.newString(result);
    }

    /*
     * call-seq:
     *     MessageClass.decode_json(data, options = {}) => message
     *
     * Decodes the given data (as a string containing bytes in protocol buffers wire
     * format) under the interpretration given by this message class's definition
     * and returns a message object with the corresponding field values.
     *
     *  @param options [Hash] options for the decoder
     *   ignore_unknown_fields: set true to ignore unknown fields (default is to
     *   raise an error)
     */
    @JRubyMethod(name = "decode_json", required = 1, optional = 1, meta = true)
    public static IRubyObject decodeJson(ThreadContext context, IRubyObject recv, IRubyObject[] args) {
        Ruby runtime = context.runtime;
        boolean ignoreUnknownFields = false;
        IRubyObject data = args[0];
        JsonFormat.Parser parser = JsonFormat.parser();

        if (args.length == 2) {
            if (!(args[1] instanceof RubyHash)) {
                throw runtime.newArgumentError("Expected hash arguments.");
            }

            IRubyObject ignoreSetting = ((RubyHash) args[1]).fastARef(runtime.newSymbol("ignore_unknown_fields"));
            if (ignoreSetting != null && ignoreSetting.isTrue()) {
                parser = parser.ignoringUnknownFields();
            }
        }

        if (!(data instanceof RubyString)) {
            throw runtime.newArgumentError("Expected string for JSON data.");
        }

        RubyMessage ret = (RubyMessage) ((RubyClass) recv).newInstance(context, Block.NULL_BLOCK);

        try {
            parser.merge(data.asJavaString(), ret.builder);
        } catch(InvalidProtocolBufferException e) {
            throw createParseError(context, e.getMessage().replace("Cannot find", "No such"));
        }

        if (isWrapper(ret.descriptor)) {
            throw runtime.newRuntimeError("Parsing a wrapper type from JSON at the top level does not work.");
        }

        return ret;
    }

    @JRubyMethod(name = "to_h")
    public IRubyObject toHash(ThreadContext context) {
        Ruby runtime = context.runtime;
        RubyHash ret = RubyHash.newHash(runtime);
        for (FieldDescriptor fdef : this.descriptor.getFields()) {
            IRubyObject value = getFieldInternal(context, fdef, proto3);

            if (!value.isNil()) {
                if (fdef.isRepeated() && !fdef.isMapField()) {
                    if (!proto3 && ((RubyRepeatedField) value).size() == 0) continue; // Don't output empty repeated fields for proto2
                    if (fdef.getType() != FieldDescriptor.Type.MESSAGE) {
                        value = Helpers.invoke(context, value, "to_a");
                    } else {
                        RubyArray ary = value.convertToArray();
                        for (int i = 0; i < ary.size(); i++) {
                            IRubyObject submsg = Helpers.invoke(context, ary.eltInternal(i), "to_h");
                            ary.eltInternalSet(i, submsg);
                        }

                        value = ary.to_ary();
                    }
                } else if (value.respondsTo("to_h")) {
                    value = Helpers.invoke(context, value, "to_h");
                } else if (value.respondsTo("to_a")) {
                    value = Helpers.invoke(context, value, "to_a");
                }
            }
            if (proto3 || !value.isNil()) {
                ret.fastASet(runtime.newSymbol(fdef.getName()), value);
            }
        }
        return ret;
    }

    protected DynamicMessage build(ThreadContext context) {
        return build(context, 0);
    }

    protected DynamicMessage build(ThreadContext context, int depth) {
        if (depth > SINK_MAXIMUM_NESTING) {
            throw context.runtime.newRuntimeError("Maximum recursion depth exceeded during encoding.");
        }
        for (FieldDescriptor fieldDescriptor : fields.keySet()) {
            IRubyObject value = fields.get(fieldDescriptor);

            if (value instanceof RubyMap) {
                builder.clearField(fieldDescriptor);
                RubyDescriptor mapDescriptor = (RubyDescriptor) getDescriptorForField(context, fieldDescriptor);
                for (DynamicMessage kv : ((RubyMap) value).build(context, mapDescriptor, depth)) {
                    builder.addRepeatedField(fieldDescriptor, kv);
                }

            } else if (value instanceof RubyRepeatedField) {
                RubyRepeatedField repeatedField = (RubyRepeatedField) value;

                builder.clearField(fieldDescriptor);
                for (int i = 0; i < repeatedField.size(); i++) {
                    Object item = convert(context, fieldDescriptor, repeatedField.get(i), depth);
                    builder.addRepeatedField(fieldDescriptor, item);
                }

            } else {
                builder.setField(fieldDescriptor, convert(context, fieldDescriptor, value, depth));
            }
        }

        return builder.build();
    }

    // Internal use only, called by Google::Protobuf.deep_copy
    protected IRubyObject deepCopy(ThreadContext context) {
        RubyMessage copy = (RubyMessage) metaClass.newInstance(context, Block.NULL_BLOCK);
        for (FieldDescriptor fdef : descriptor.getFields()) {
            if (fdef.isRepeated()) {
                copy.fields.put(fdef, this.getRepeatedField(context, fdef).deepCopy(context));
            } else if (fields.containsKey(fdef)) {
                copy.fields.put(fdef, fields.get(fdef));
            } else if (builder.hasField(fdef)) {
                copy.fields.put(fdef, wrapField(context, fdef, builder.getField(fdef)));
            }
        }
        return copy;
    }

    protected IRubyObject clearField(ThreadContext context, FieldDescriptor fieldDescriptor) {
        validateMessageType(context, fieldDescriptor, "clear");
        return clearFieldInternal(context, fieldDescriptor);
    }

    protected void discardUnknownFields(ThreadContext context) {
        discardUnknownFields(context, builder);
    }

    protected IRubyObject getField(ThreadContext context, FieldDescriptor fieldDescriptor) {
        validateMessageType(context, fieldDescriptor, "get");
        return getFieldInternal(context, fieldDescriptor);
    }

    protected IRubyObject hasField(ThreadContext context, FieldDescriptor fieldDescriptor) {
        validateMessageType(context, fieldDescriptor, "has?");
        if (!fieldHasPresence(fieldDescriptor)) throw context.runtime.newArgumentError("does not track presence");
        return fields.containsKey(fieldDescriptor) ? context.runtime.getTrue() : context.runtime.getFalse();
    }

    protected IRubyObject setField(ThreadContext context, FieldDescriptor fieldDescriptor, IRubyObject value) {
        validateMessageType(context, fieldDescriptor, "set");
        return setFieldInternal(context, fieldDescriptor, value);
    }

    private RubyRepeatedField getRepeatedField(ThreadContext context, FieldDescriptor fieldDescriptor) {
        if (fields.containsKey(fieldDescriptor)) {
            return (RubyRepeatedField) fields.get(fieldDescriptor);
        }
        int count = this.builder.getRepeatedFieldCount(fieldDescriptor);
        RubyRepeatedField ret = repeatedFieldForFieldDescriptor(context, fieldDescriptor);
        for (int i = 0; i < count; i++) {
            ret.push(context, new IRubyObject[] {wrapField(context, fieldDescriptor, this.builder.getRepeatedField(fieldDescriptor, i))});
        }
        fields.put(fieldDescriptor, ret);
        return ret;
    }

    private IRubyObject buildFrom(ThreadContext context, DynamicMessage dynamicMessage) {
        this.builder.mergeFrom(dynamicMessage);
        return this;
    }

    private IRubyObject clearFieldInternal(ThreadContext context, FieldDescriptor fieldDescriptor) {
        OneofDescriptor ood = fieldDescriptor.getContainingOneof();
        if (ood != null) oneofCases.remove(ood);
        fields.remove(fieldDescriptor);
        builder.clearField(fieldDescriptor);
        return context.nil;
    }

    private void discardUnknownFields(ThreadContext context, Message.Builder messageBuilder) {
        messageBuilder.setUnknownFields(UnknownFieldSet.getDefaultInstance());
        messageBuilder.getAllFields().forEach((fd, value) -> {
            if (fd.getType() == FieldDescriptor.Type.MESSAGE) {
                if (fd.isRepeated()) {
                    messageBuilder.clearField(fd);
                    ((List) value).forEach((val) -> {
                        Message.Builder submessageBuilder = ((DynamicMessage) val).toBuilder();
                        discardUnknownFields(context, submessageBuilder);
                        messageBuilder.addRepeatedField(fd, submessageBuilder.build());
                    });
                } else {
                    Message.Builder submessageBuilder = ((DynamicMessage) value).toBuilder();
                    discardUnknownFields(context, submessageBuilder);
                    messageBuilder.setField(fd, submessageBuilder.build());
                }
            }
        });
    }

    private FieldDescriptor findField(ThreadContext context, IRubyObject fieldName) {
        return findField(context, fieldName, false);
    }

    private FieldDescriptor findField(ThreadContext context, IRubyObject fieldName, boolean ignoreUnknownField) {
        String nameStr = fieldName.asJavaString();
        FieldDescriptor ret = this.descriptor.findFieldByName(nameStr);
        if (ret == null && !ignoreUnknownField) {
            throw context.runtime.newArgumentError("field " + fieldName.asJavaString() + " is not found");
        }
        return ret;
    }

    // convert a ruby object to protobuf type, skip type check since it is checked on the way in
    private Object convert(ThreadContext context,
                           FieldDescriptor fieldDescriptor,
                           IRubyObject value, int depth) {
        Ruby runtime = context.runtime;
        Object val = null;
        switch (fieldDescriptor.getType()) {
            case INT32:
                val = RubyNumeric.num2int(value);
                break;
            case INT64:
                val = RubyNumeric.num2long(value);
                break;
            case UINT32:
                val = Utils.num2uint(value);
                break;
            case UINT64:
                val = Utils.num2ulong(context.runtime, value);
                break;
            case FLOAT:
                val = (float) RubyNumeric.num2dbl(value);
                break;
            case DOUBLE:
                val = (double) RubyNumeric.num2dbl(value);
                break;
            case BOOL:
                val = value.isTrue();
                break;
            case BYTES:
                val = ByteString.copyFrom(((RubyString) value).getBytes());
                break;
            case STRING:
                val = ((RubyString) value).asJavaString();
                break;
            case MESSAGE:
                val = ((RubyMessage) value).build(context, depth + 1);
                break;
            case ENUM:
                EnumDescriptor enumDescriptor = fieldDescriptor.getEnumType();
                if (Utils.isRubyNum(value)) {
                    val = enumDescriptor.findValueByNumberCreatingIfUnknown(RubyNumeric.num2int(value));
                } else {
                    val = enumDescriptor.findValueByName(value.asJavaString());
                }
                break;
            default:
                break;
        }

        return val;
    }

    private static RaiseException createParseError(ThreadContext context, String message) {
        if (parseErrorClass == null) {
            parseErrorClass = (RubyClass) context.runtime.getClassFromPath("Google::Protobuf::ParseError");
        }
        return RaiseException.from(context.runtime, parseErrorClass, message);
    }

    private IRubyObject wrapField(ThreadContext context, FieldDescriptor fieldDescriptor, Object value) {
        if (value == null) {
            return context.runtime.getNil();
        }
        Ruby runtime = context.runtime;

        switch (fieldDescriptor.getType()) {
            case INT32:
            case INT64:
            case UINT32:
            case UINT64:
            case FLOAT:
            case DOUBLE:
            case BOOL:
            case BYTES:
            case STRING:
                return Utils.wrapPrimaryValue(context, fieldDescriptor.getType(), value);
            case MESSAGE:
                RubyClass typeClass = (RubyClass) ((RubyDescriptor) getDescriptorForField(context, fieldDescriptor)).msgclass(context);
                RubyMessage msg = (RubyMessage) typeClass.newInstance(context, Block.NULL_BLOCK);
                return msg.buildFrom(context, (DynamicMessage) value);
            case ENUM:
                EnumValueDescriptor enumValueDescriptor = (EnumValueDescriptor) value;
                if (enumValueDescriptor.getIndex() == -1) { // UNKNOWN ENUM VALUE
                    return runtime.newFixnum(enumValueDescriptor.getNumber());
                }
                return runtime.newSymbol(enumValueDescriptor.getName());
            default:
                return runtime.newString(value.toString());
        }
    }

    private RubyRepeatedField repeatedFieldForFieldDescriptor(ThreadContext context, FieldDescriptor fieldDescriptor) {
        IRubyObject typeClass = context.runtime.getNilClass();
        IRubyObject descriptor = getDescriptorForField(context, fieldDescriptor);
        FieldDescriptor.Type type = fieldDescriptor.getType();

        if (type == FieldDescriptor.Type.MESSAGE) {
            typeClass = ((RubyDescriptor) descriptor).msgclass(context);

        } else if (type == FieldDescriptor.Type.ENUM) {
            typeClass = ((RubyEnumDescriptor) descriptor).enummodule(context);
        }

        RubyRepeatedField field = new RubyRepeatedField(context.runtime, cRepeatedField, type, typeClass);
        field.setName(fieldDescriptor.getName());

        return field;
    }

    private IRubyObject getFieldInternal(ThreadContext context, FieldDescriptor fieldDescriptor) {
        return getFieldInternal(context, fieldDescriptor, true);
    }

    private IRubyObject getFieldInternal(ThreadContext context, FieldDescriptor fieldDescriptor, boolean returnDefaults) {
        OneofDescriptor oneofDescriptor = fieldDescriptor.getContainingOneof();
        if (oneofDescriptor != null) {
            if (oneofCases.get(oneofDescriptor) == fieldDescriptor) {
                return fields.get(fieldDescriptor);
            } else {
                FieldDescriptor oneofCase = builder.getOneofFieldDescriptor(oneofDescriptor);
                if (oneofCase != fieldDescriptor) {
                  if (fieldDescriptor.getType() == FieldDescriptor.Type.MESSAGE || !returnDefaults) {
                    return context.nil;
                  } else {
                    return wrapField(context, fieldDescriptor, fieldDescriptor.getDefaultValue());
                  }
                }
                if (returnDefaults || builder.hasField(fieldDescriptor)) {
                    IRubyObject value = wrapField(context, oneofCase, builder.getField(oneofCase));
                    fields.put(fieldDescriptor, value);
                    return value;
                } else {
                    return context.nil;
                }
            }
        }

        if (Utils.isMapEntry(fieldDescriptor)) {
            RubyMap map = (RubyMap) fields.get(fieldDescriptor);
            if (map == null) {
                map = newMapForField(context, fieldDescriptor);
                int mapSize = this.builder.getRepeatedFieldCount(fieldDescriptor);
                FieldDescriptor keyField = fieldDescriptor.getMessageType().findFieldByNumber(1);
                FieldDescriptor valueField = fieldDescriptor.getMessageType().findFieldByNumber(2);
                RubyDescriptor kvDescriptor = (RubyDescriptor) getDescriptorForField(context, fieldDescriptor);
                RubyClass kvClass = (RubyClass) kvDescriptor.msgclass(context);
                for (int i = 0; i < mapSize; i++) {
                    RubyMessage kvMessage = (RubyMessage) kvClass.newInstance(context, Block.NULL_BLOCK);
                    DynamicMessage message = (DynamicMessage) this.builder.getRepeatedField(fieldDescriptor, i);
                    kvMessage.buildFrom(context, message);
                    map.indexSet(context, kvMessage.getField(context, keyField), kvMessage.getField(context, valueField));
                }
                fields.put(fieldDescriptor, map);
            }
            return map;
        }

        if (fieldDescriptor.isRepeated()) {
            return getRepeatedField(context, fieldDescriptor);
        }

        if (fieldDescriptor.getType() != FieldDescriptor.Type.MESSAGE ||
                builder.hasField(fieldDescriptor) || fields.containsKey(fieldDescriptor)) {
            if (fields.containsKey(fieldDescriptor)) {
                return fields.get(fieldDescriptor);
            } else if (returnDefaults || builder.hasField(fieldDescriptor)) {
                IRubyObject value = wrapField(context, fieldDescriptor, builder.getField(fieldDescriptor));
                if (builder.hasField(fieldDescriptor)) {
                    fields.put(fieldDescriptor, value);
                }
                return value;
            }
        }
        return context.nil;
    }

    private IRubyObject setFieldInternal(ThreadContext context, FieldDescriptor fieldDescriptor, IRubyObject value) {
        testFrozen("can't modify frozen " + getMetaClass());

        if (Utils.isMapEntry(fieldDescriptor)) {
            if (!(value instanceof RubyMap)) {
                throw Utils.createTypeError(context, "Expected Map instance");
            }
            RubyMap thisMap = (RubyMap) getFieldInternal(context, fieldDescriptor);
            thisMap.mergeIntoSelf(context, value);

        } else if (fieldDescriptor.isRepeated()) {
            if (value instanceof RubyRepeatedField) {
                fields.put(fieldDescriptor, value);
            } else {
                throw Utils.createTypeError(context, "Expected repeated field array");
            }

        } else {
            boolean addValue = true;
            FieldDescriptor.Type fieldType = fieldDescriptor.getType();
            OneofDescriptor oneofDescriptor = fieldDescriptor.getContainingOneof();

            // Determine the typeclass, if any
            IRubyObject typeClass = context.runtime.getObject();
            if (fieldType == FieldDescriptor.Type.MESSAGE) {
                typeClass = ((RubyDescriptor) getDescriptorForField(context, fieldDescriptor)).msgclass(context);
                if (value.isNil()){
                    addValue = false;
                }
            } else if (fieldType == FieldDescriptor.Type.ENUM) {
                typeClass = ((RubyEnumDescriptor) getDescriptorForField(context, fieldDescriptor)).enummodule(context);
                value = enumToSymbol(context, fieldDescriptor.getEnumType(), value);
            }

            if (oneofDescriptor != null) {
                FieldDescriptor oneofCase = oneofCases.get(oneofDescriptor);

                // Remove the existing field if we are setting a different field in the Oneof
                if (oneofCase != null && oneofCase != fieldDescriptor) {
                    fields.remove(oneofCase);
                }

                // Keep track of what Oneofs are set
                if (value.isNil()) {
                    oneofCases.remove(oneofDescriptor);
                    addValue = false;
                } else {
                    oneofCases.put(oneofDescriptor, fieldDescriptor);
                }
            }

            if (addValue) {
                value = Utils.checkType(context, fieldType, fieldDescriptor.getName(), value, (RubyModule) typeClass);
                fields.put(fieldDescriptor, value);
            } else {
                fields.remove(fieldDescriptor);
            }
        }
        return context.nil;
    }

    private IRubyObject getDescriptorForField(ThreadContext context, FieldDescriptor fieldDescriptor) {
        RubyDescriptor thisRbDescriptor = (RubyDescriptor) getDescriptor(context, metaClass);
        RubyFieldDescriptor fd = (RubyFieldDescriptor) thisRbDescriptor.lookup(context, context.runtime.newString(fieldDescriptor.getName()));
        return fd.getSubtype(context);
    }

    private IRubyObject enumToSymbol(ThreadContext context, EnumDescriptor enumDescriptor, IRubyObject value) {
        if (value instanceof RubySymbol) {
            return (RubySymbol) value;
        } else if (Utils.isRubyNum(value)) {
            EnumValueDescriptor enumValue = enumDescriptor.findValueByNumberCreatingIfUnknown(RubyNumeric.num2int(value));
            if (enumValue.getIndex() != -1) {
                return context.runtime.newSymbol(enumValue.getName());
            } else {
                return value;
            }
        } else if (value instanceof RubyString) {
            return ((RubyString) value).intern();
        }

        return context.runtime.newSymbol("UNKNOWN");
    }

    private boolean fieldHasPresence(FieldDescriptor fieldDescriptor) {
      return !fieldDescriptor.isRepeated() &&
              (fieldDescriptor.getType() == FieldDescriptor.Type.MESSAGE ||
                fieldDescriptor.getContainingOneof() != null ||
                !proto3);
    }

    private RubyRepeatedField rubyToRepeatedField(ThreadContext context,
                                                  FieldDescriptor fieldDescriptor, IRubyObject value) {
        RubyArray arr = value.convertToArray();
        RubyRepeatedField repeatedField = repeatedFieldForFieldDescriptor(context, fieldDescriptor);
        IRubyObject[] values = new IRubyObject[arr.size()];
        FieldDescriptor.Type fieldType = fieldDescriptor.getType();
        String fieldName = fieldDescriptor.getName();

        RubyModule typeClass = null;
        if (fieldType == FieldDescriptor.Type.MESSAGE) {
            RubyDescriptor descriptor = (RubyDescriptor) getDescriptorForField(context, fieldDescriptor);
            typeClass = (RubyModule) descriptor.msgclass(context);
        } else if (fieldType == FieldDescriptor.Type.ENUM) {
            RubyEnumDescriptor enumDescriptor = (RubyEnumDescriptor) getDescriptorForField(context, fieldDescriptor);
            typeClass = (RubyModule) enumDescriptor.enummodule(context);
        }

        for (int i = 0; i < arr.size(); i++) {
            IRubyObject item = arr.eltInternal(i);
            if (item instanceof RubyHash && typeClass != null) {
                values[i] = (IRubyObject) ((RubyClass) typeClass).newInstance(context, item, Block.NULL_BLOCK);
            } else {
                if (fieldType == FieldDescriptor.Type.ENUM) {
                    item = enumToSymbol(context, fieldDescriptor.getEnumType(), item);
                }

                values[i] = item;
            }
        }
        repeatedField.push(context, values);

        return repeatedField;
    }

    private RubyMap newMapForField(ThreadContext context, FieldDescriptor fieldDescriptor) {
        RubyDescriptor mapDescriptor = (RubyDescriptor) getDescriptorForField(context, fieldDescriptor);
        FieldDescriptor keyField = fieldDescriptor.getMessageType().findFieldByNumber(1);
        FieldDescriptor valueField = fieldDescriptor.getMessageType().findFieldByNumber(2);
        IRubyObject keyType = RubySymbol.newSymbol(context.runtime, keyField.getType().name());
        IRubyObject valueType = RubySymbol.newSymbol(context.runtime, valueField.getType().name());

        if (valueField.getType() == FieldDescriptor.Type.MESSAGE) {
            RubyFieldDescriptor rubyFieldDescriptor = (RubyFieldDescriptor) mapDescriptor.lookup(context,
                    context.runtime.newString("value"));
            RubyDescriptor rubyDescriptor = (RubyDescriptor) rubyFieldDescriptor.getSubtype(context);
            return (RubyMap) cMap.newInstance(context, keyType, valueType,
                    rubyDescriptor.msgclass(context), Block.NULL_BLOCK);

        } else if (valueField.getType() == FieldDescriptor.Type.ENUM) {
            RubyFieldDescriptor rubyFieldDescriptor = (RubyFieldDescriptor) mapDescriptor.lookup(context,
                    context.runtime.newString("value"));
            RubyEnumDescriptor rubyEnumDescriptor = (RubyEnumDescriptor) rubyFieldDescriptor.getSubtype(context);
            return (RubyMap) cMap.newInstance(context, keyType, valueType,
                    rubyEnumDescriptor.enummodule(context), Block.NULL_BLOCK);

        } else {
            return (RubyMap) cMap.newInstance(context, keyType, valueType, Block.NULL_BLOCK);
        }
    }

    private FieldDescriptor getOneofCase(OneofDescriptor oneof) {
        if (oneofCases.containsKey(oneof)) {
            return oneofCases.get(oneof);
        }
        return builder.getOneofFieldDescriptor(oneof);
    }

    private boolean isWrappable(FieldDescriptor fieldDescriptor) {
      if (fieldDescriptor.getType() != FieldDescriptor.Type.MESSAGE) return false;

      return isWrapper(fieldDescriptor.getMessageType());
    }

    private static boolean isWrapper(Descriptor messageDescriptor) {
      switch(messageDescriptor.getFullName()) {
          case "google.protobuf.DoubleValue":
          case "google.protobuf.FloatValue":
          case "google.protobuf.Int64Value":
          case "google.protobuf.UInt64Value":
          case "google.protobuf.Int32Value":
          case "google.protobuf.UInt32Value":
          case "google.protobuf.BoolValue":
          case "google.protobuf.StringValue":
          case "google.protobuf.BytesValue":
              return true;
          default:
              return false;
      }
    }

    private void validateMessageType(ThreadContext context, FieldDescriptor fieldDescriptor, String methodName) {
        if (descriptor != fieldDescriptor.getContainingType()) {
            throw context.runtime.newTypeError(methodName + " method called on wrong message type");
        }
    }

    private static RubyClass parseErrorClass;

    private static final String AS_VALUE_SUFFIX = "_as_value";
    private static final String CLEAR_PREFIX = "clear_";
    private static final String CONST_SUFFIX = "_const";
    private static final String HAS_PREFIX = "has_";
    private static final String QUESTION_MARK = "?";
    private static final int SINK_MAXIMUM_NESTING = 63;

    private Descriptor descriptor;
    private DynamicMessage.Builder builder;
    private Map<FieldDescriptor, IRubyObject> fields;
    private Map<OneofDescriptor, FieldDescriptor> oneofCases;
    private RubyClass cRepeatedField;
    private RubyClass cMap;
    private boolean ignoreUnknownFieldsOnInit = false;
    private boolean proto3;


}
