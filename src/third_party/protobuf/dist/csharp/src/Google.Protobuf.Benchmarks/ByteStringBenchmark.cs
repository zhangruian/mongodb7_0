#region Copyright notice and license
// Protocol Buffers - Google's data interchange format
// Copyright 2019 Google Inc.  All rights reserved.
// https://github.com/protocolbuffers/protobuf
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
#endregion

using BenchmarkDotNet.Attributes;

namespace Google.Protobuf.Benchmarks
{
    /// <summary>
    /// Benchmarks using ByteString.
    /// </summary>
    [MemoryDiagnoser]
    public class ByteStringBenchmark
    {
        private const int Zero = 0;
        private const int Kilobyte = 1024;
        private const int _128Kilobytes = 1024 * 128;
        private const int Megabyte = 1024 * 1024;
        private const int _10Megabytes = 1024 * 1024 * 10;

        byte[] byteBuffer;

        [GlobalSetup]
        public void GlobalSetup()
        {
            byteBuffer = new byte[PayloadSize];
        }

        [Params(Zero, Kilobyte, _128Kilobytes, Megabyte, _10Megabytes)]
        public int PayloadSize { get; set; }

        [Benchmark]
        public ByteString CopyFrom()
        {
            return ByteString.CopyFrom(byteBuffer);
        }

        [Benchmark]
        public ByteString UnsafeWrap()
        {
            return UnsafeByteOperations.UnsafeWrap(byteBuffer);
        }
    }
}
