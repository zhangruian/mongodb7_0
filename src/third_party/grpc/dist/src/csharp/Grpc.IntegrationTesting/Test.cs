// <auto-generated>
//     Generated by the protocol buffer compiler.  DO NOT EDIT!
//     source: src/proto/grpc/testing/test.proto
// </auto-generated>
#pragma warning disable 1591, 0612, 3021
#region Designer generated code

using pb = global::Google.Protobuf;
using pbc = global::Google.Protobuf.Collections;
using pbr = global::Google.Protobuf.Reflection;
using scg = global::System.Collections.Generic;
namespace Grpc.Testing {

  /// <summary>Holder for reflection information generated from src/proto/grpc/testing/test.proto</summary>
  public static partial class TestReflection {

    #region Descriptor
    /// <summary>File descriptor for src/proto/grpc/testing/test.proto</summary>
    public static pbr::FileDescriptor Descriptor {
      get { return descriptor; }
    }
    private static pbr::FileDescriptor descriptor;

    static TestReflection() {
      byte[] descriptorData = global::System.Convert.FromBase64String(
          string.Concat(
            "CiFzcmMvcHJvdG8vZ3JwYy90ZXN0aW5nL3Rlc3QucHJvdG8SDGdycGMudGVz",
            "dGluZxoic3JjL3Byb3RvL2dycGMvdGVzdGluZy9lbXB0eS5wcm90bxolc3Jj",
            "L3Byb3RvL2dycGMvdGVzdGluZy9tZXNzYWdlcy5wcm90bzLLBQoLVGVzdFNl",
            "cnZpY2USNQoJRW1wdHlDYWxsEhMuZ3JwYy50ZXN0aW5nLkVtcHR5GhMuZ3Jw",
            "Yy50ZXN0aW5nLkVtcHR5EkYKCVVuYXJ5Q2FsbBIbLmdycGMudGVzdGluZy5T",
            "aW1wbGVSZXF1ZXN0GhwuZ3JwYy50ZXN0aW5nLlNpbXBsZVJlc3BvbnNlEk8K",
            "EkNhY2hlYWJsZVVuYXJ5Q2FsbBIbLmdycGMudGVzdGluZy5TaW1wbGVSZXF1",
            "ZXN0GhwuZ3JwYy50ZXN0aW5nLlNpbXBsZVJlc3BvbnNlEmwKE1N0cmVhbWlu",
            "Z091dHB1dENhbGwSKC5ncnBjLnRlc3RpbmcuU3RyZWFtaW5nT3V0cHV0Q2Fs",
            "bFJlcXVlc3QaKS5ncnBjLnRlc3RpbmcuU3RyZWFtaW5nT3V0cHV0Q2FsbFJl",
            "c3BvbnNlMAESaQoSU3RyZWFtaW5nSW5wdXRDYWxsEicuZ3JwYy50ZXN0aW5n",
            "LlN0cmVhbWluZ0lucHV0Q2FsbFJlcXVlc3QaKC5ncnBjLnRlc3RpbmcuU3Ry",
            "ZWFtaW5nSW5wdXRDYWxsUmVzcG9uc2UoARJpCg5GdWxsRHVwbGV4Q2FsbBIo",
            "LmdycGMudGVzdGluZy5TdHJlYW1pbmdPdXRwdXRDYWxsUmVxdWVzdBopLmdy",
            "cGMudGVzdGluZy5TdHJlYW1pbmdPdXRwdXRDYWxsUmVzcG9uc2UoATABEmkK",
            "DkhhbGZEdXBsZXhDYWxsEiguZ3JwYy50ZXN0aW5nLlN0cmVhbWluZ091dHB1",
            "dENhbGxSZXF1ZXN0GikuZ3JwYy50ZXN0aW5nLlN0cmVhbWluZ091dHB1dENh",
            "bGxSZXNwb25zZSgBMAESPQoRVW5pbXBsZW1lbnRlZENhbGwSEy5ncnBjLnRl",
            "c3RpbmcuRW1wdHkaEy5ncnBjLnRlc3RpbmcuRW1wdHkyVQoUVW5pbXBsZW1l",
            "bnRlZFNlcnZpY2USPQoRVW5pbXBsZW1lbnRlZENhbGwSEy5ncnBjLnRlc3Rp",
            "bmcuRW1wdHkaEy5ncnBjLnRlc3RpbmcuRW1wdHkyiQEKEFJlY29ubmVjdFNl",
            "cnZpY2USOwoFU3RhcnQSHS5ncnBjLnRlc3RpbmcuUmVjb25uZWN0UGFyYW1z",
            "GhMuZ3JwYy50ZXN0aW5nLkVtcHR5EjgKBFN0b3ASEy5ncnBjLnRlc3Rpbmcu",
            "RW1wdHkaGy5ncnBjLnRlc3RpbmcuUmVjb25uZWN0SW5mbzKGAgoYTG9hZEJh",
            "bGFuY2VyU3RhdHNTZXJ2aWNlEmMKDkdldENsaWVudFN0YXRzEiYuZ3JwYy50",
            "ZXN0aW5nLkxvYWRCYWxhbmNlclN0YXRzUmVxdWVzdBonLmdycGMudGVzdGlu",
            "Zy5Mb2FkQmFsYW5jZXJTdGF0c1Jlc3BvbnNlIgAShAEKGUdldENsaWVudEFj",
            "Y3VtdWxhdGVkU3RhdHMSMS5ncnBjLnRlc3RpbmcuTG9hZEJhbGFuY2VyQWNj",
            "dW11bGF0ZWRTdGF0c1JlcXVlc3QaMi5ncnBjLnRlc3RpbmcuTG9hZEJhbGFu",
            "Y2VyQWNjdW11bGF0ZWRTdGF0c1Jlc3BvbnNlIgAyiwEKFlhkc1VwZGF0ZUhl",
            "YWx0aFNlcnZpY2USNgoKU2V0U2VydmluZxITLmdycGMudGVzdGluZy5FbXB0",
            "eRoTLmdycGMudGVzdGluZy5FbXB0eRI5Cg1TZXROb3RTZXJ2aW5nEhMuZ3Jw",
            "Yy50ZXN0aW5nLkVtcHR5GhMuZ3JwYy50ZXN0aW5nLkVtcHR5MnsKH1hkc1Vw",
            "ZGF0ZUNsaWVudENvbmZpZ3VyZVNlcnZpY2USWAoJQ29uZmlndXJlEiQuZ3Jw",
            "Yy50ZXN0aW5nLkNsaWVudENvbmZpZ3VyZVJlcXVlc3QaJS5ncnBjLnRlc3Rp",
            "bmcuQ2xpZW50Q29uZmlndXJlUmVzcG9uc2ViBnByb3RvMw=="));
      descriptor = pbr::FileDescriptor.FromGeneratedCode(descriptorData,
          new pbr::FileDescriptor[] { global::Grpc.Testing.EmptyReflection.Descriptor, global::Grpc.Testing.MessagesReflection.Descriptor, },
          new pbr::GeneratedClrTypeInfo(null, null, null));
    }
    #endregion

  }
}

#endregion Designer generated code
