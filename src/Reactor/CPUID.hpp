// Copyright 2016 The SwiftShader Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef rr_CPUID_hpp
#define rr_CPUID_hpp

namespace rr {

#if !defined(__i386__) && defined(_M_IX86)
#	define __i386__ 1
#endif

#if !defined(__x86_64__) && (defined(_M_AMD64) || defined(_M_X64))
#	define __x86_64__ 1
#endif

class CPUID
{
public:
	static bool supportsSSE4_1();
};

}  // namespace rr

#endif  // rr_CPUID_hpp
