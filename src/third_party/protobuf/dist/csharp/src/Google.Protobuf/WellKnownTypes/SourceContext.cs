// <auto-generated>
//     Generated by the protocol buffer compiler.  DO NOT EDIT!
//     source: google/protobuf/source_context.proto
// </auto-generated>
#pragma warning disable 1591, 0612, 3021
#region Designer generated code

using pb = global::Google.Protobuf;
using pbc = global::Google.Protobuf.Collections;
using pbr = global::Google.Protobuf.Reflection;
using scg = global::System.Collections.Generic;
namespace Google.Protobuf.WellKnownTypes {

  /// <summary>Holder for reflection information generated from google/protobuf/source_context.proto</summary>
  public static partial class SourceContextReflection {

    #region Descriptor
    /// <summary>File descriptor for google/protobuf/source_context.proto</summary>
    public static pbr::FileDescriptor Descriptor {
      get { return descriptor; }
    }
    private static pbr::FileDescriptor descriptor;

    static SourceContextReflection() {
      byte[] descriptorData = global::System.Convert.FromBase64String(
          string.Concat(
            "CiRnb29nbGUvcHJvdG9idWYvc291cmNlX2NvbnRleHQucHJvdG8SD2dvb2ds",
            "ZS5wcm90b2J1ZiIiCg1Tb3VyY2VDb250ZXh0EhEKCWZpbGVfbmFtZRgBIAEo",
            "CUKKAQoTY29tLmdvb2dsZS5wcm90b2J1ZkISU291cmNlQ29udGV4dFByb3Rv",
            "UAFaNmdvb2dsZS5nb2xhbmcub3JnL3Byb3RvYnVmL3R5cGVzL2tub3duL3Nv",
            "dXJjZWNvbnRleHRwYqICA0dQQqoCHkdvb2dsZS5Qcm90b2J1Zi5XZWxsS25v",
            "d25UeXBlc2IGcHJvdG8z"));
      descriptor = pbr::FileDescriptor.FromGeneratedCode(descriptorData,
          new pbr::FileDescriptor[] { },
          new pbr::GeneratedClrTypeInfo(null, null, new pbr::GeneratedClrTypeInfo[] {
            new pbr::GeneratedClrTypeInfo(typeof(global::Google.Protobuf.WellKnownTypes.SourceContext), global::Google.Protobuf.WellKnownTypes.SourceContext.Parser, new[]{ "FileName" }, null, null, null, null)
          }));
    }
    #endregion

  }
  #region Messages
  /// <summary>
  /// `SourceContext` represents information about the source of a
  /// protobuf element, like the file in which it is defined.
  /// </summary>
  public sealed partial class SourceContext : pb::IMessage<SourceContext>
  #if !GOOGLE_PROTOBUF_REFSTRUCT_COMPATIBILITY_MODE
      , pb::IBufferMessage
  #endif
  {
    private static readonly pb::MessageParser<SourceContext> _parser = new pb::MessageParser<SourceContext>(() => new SourceContext());
    private pb::UnknownFieldSet _unknownFields;
    [global::System.Diagnostics.DebuggerNonUserCodeAttribute]
    public static pb::MessageParser<SourceContext> Parser { get { return _parser; } }

    [global::System.Diagnostics.DebuggerNonUserCodeAttribute]
    public static pbr::MessageDescriptor Descriptor {
      get { return global::Google.Protobuf.WellKnownTypes.SourceContextReflection.Descriptor.MessageTypes[0]; }
    }

    [global::System.Diagnostics.DebuggerNonUserCodeAttribute]
    pbr::MessageDescriptor pb::IMessage.Descriptor {
      get { return Descriptor; }
    }

    [global::System.Diagnostics.DebuggerNonUserCodeAttribute]
    public SourceContext() {
      OnConstruction();
    }

    partial void OnConstruction();

    [global::System.Diagnostics.DebuggerNonUserCodeAttribute]
    public SourceContext(SourceContext other) : this() {
      fileName_ = other.fileName_;
      _unknownFields = pb::UnknownFieldSet.Clone(other._unknownFields);
    }

    [global::System.Diagnostics.DebuggerNonUserCodeAttribute]
    public SourceContext Clone() {
      return new SourceContext(this);
    }

    /// <summary>Field number for the "file_name" field.</summary>
    public const int FileNameFieldNumber = 1;
    private string fileName_ = "";
    /// <summary>
    /// The path-qualified name of the .proto file that contained the associated
    /// protobuf element.  For example: `"google/protobuf/source_context.proto"`.
    /// </summary>
    [global::System.Diagnostics.DebuggerNonUserCodeAttribute]
    public string FileName {
      get { return fileName_; }
      set {
        fileName_ = pb::ProtoPreconditions.CheckNotNull(value, "value");
      }
    }

    [global::System.Diagnostics.DebuggerNonUserCodeAttribute]
    public override bool Equals(object other) {
      return Equals(other as SourceContext);
    }

    [global::System.Diagnostics.DebuggerNonUserCodeAttribute]
    public bool Equals(SourceContext other) {
      if (ReferenceEquals(other, null)) {
        return false;
      }
      if (ReferenceEquals(other, this)) {
        return true;
      }
      if (FileName != other.FileName) return false;
      return Equals(_unknownFields, other._unknownFields);
    }

    [global::System.Diagnostics.DebuggerNonUserCodeAttribute]
    public override int GetHashCode() {
      int hash = 1;
      if (FileName.Length != 0) hash ^= FileName.GetHashCode();
      if (_unknownFields != null) {
        hash ^= _unknownFields.GetHashCode();
      }
      return hash;
    }

    [global::System.Diagnostics.DebuggerNonUserCodeAttribute]
    public override string ToString() {
      return pb::JsonFormatter.ToDiagnosticString(this);
    }

    [global::System.Diagnostics.DebuggerNonUserCodeAttribute]
    public void WriteTo(pb::CodedOutputStream output) {
    #if !GOOGLE_PROTOBUF_REFSTRUCT_COMPATIBILITY_MODE
      output.WriteRawMessage(this);
    #else
      if (FileName.Length != 0) {
        output.WriteRawTag(10);
        output.WriteString(FileName);
      }
      if (_unknownFields != null) {
        _unknownFields.WriteTo(output);
      }
    #endif
    }

    #if !GOOGLE_PROTOBUF_REFSTRUCT_COMPATIBILITY_MODE
    [global::System.Diagnostics.DebuggerNonUserCodeAttribute]
    void pb::IBufferMessage.InternalWriteTo(ref pb::WriteContext output) {
      if (FileName.Length != 0) {
        output.WriteRawTag(10);
        output.WriteString(FileName);
      }
      if (_unknownFields != null) {
        _unknownFields.WriteTo(ref output);
      }
    }
    #endif

    [global::System.Diagnostics.DebuggerNonUserCodeAttribute]
    public int CalculateSize() {
      int size = 0;
      if (FileName.Length != 0) {
        size += 1 + pb::CodedOutputStream.ComputeStringSize(FileName);
      }
      if (_unknownFields != null) {
        size += _unknownFields.CalculateSize();
      }
      return size;
    }

    [global::System.Diagnostics.DebuggerNonUserCodeAttribute]
    public void MergeFrom(SourceContext other) {
      if (other == null) {
        return;
      }
      if (other.FileName.Length != 0) {
        FileName = other.FileName;
      }
      _unknownFields = pb::UnknownFieldSet.MergeFrom(_unknownFields, other._unknownFields);
    }

    [global::System.Diagnostics.DebuggerNonUserCodeAttribute]
    public void MergeFrom(pb::CodedInputStream input) {
    #if !GOOGLE_PROTOBUF_REFSTRUCT_COMPATIBILITY_MODE
      input.ReadRawMessage(this);
    #else
      uint tag;
      while ((tag = input.ReadTag()) != 0) {
        switch(tag) {
          default:
            _unknownFields = pb::UnknownFieldSet.MergeFieldFrom(_unknownFields, input);
            break;
          case 10: {
            FileName = input.ReadString();
            break;
          }
        }
      }
    #endif
    }

    #if !GOOGLE_PROTOBUF_REFSTRUCT_COMPATIBILITY_MODE
    [global::System.Diagnostics.DebuggerNonUserCodeAttribute]
    void pb::IBufferMessage.InternalMergeFrom(ref pb::ParseContext input) {
      uint tag;
      while ((tag = input.ReadTag()) != 0) {
        switch(tag) {
          default:
            _unknownFields = pb::UnknownFieldSet.MergeFieldFrom(_unknownFields, ref input);
            break;
          case 10: {
            FileName = input.ReadString();
            break;
          }
        }
      }
    }
    #endif

  }

  #endregion

}

#endregion Designer generated code
