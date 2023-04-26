#ifndef LLVM_PROFILEDATA_MEMPROF_H_
#define LLVM_PROFILEDATA_MEMPROF_H_

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/ProfileData/MemProfData.inc"
#include "llvm/Support/Endian.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <optional>

namespace llvm {
namespace memprof {

enum class Meta : uint64_t {
  Start = 0,
#define MIBEntryDef(NameTag, Name, Type) NameTag,
#include "llvm/ProfileData/MIBEntryDef.inc"
#undef MIBEntryDef
  Size
};

using MemProfSchema = llvm::SmallVector<Meta, static_cast<int>(Meta::Size)>;

// Holds the actual MemInfoBlock data with all fields. Contents may be read or
// written partially by providing an appropriate schema to the serialize and
// deserialize methods.
struct PortableMemInfoBlock {
  PortableMemInfoBlock() = default;
  explicit PortableMemInfoBlock(const MemInfoBlock &Block) {
#define MIBEntryDef(NameTag, Name, Type) Name = Block.Name;
#include "llvm/ProfileData/MIBEntryDef.inc"
#undef MIBEntryDef
  }

  PortableMemInfoBlock(const MemProfSchema &Schema, const unsigned char *Ptr) {
    deserialize(Schema, Ptr);
  }

  // Read the contents of \p Ptr based on the \p Schema to populate the
  // MemInfoBlock member.
  void deserialize(const MemProfSchema &Schema, const unsigned char *Ptr) {
    using namespace support;

    for (const Meta Id : Schema) {
      switch (Id) {
#define MIBEntryDef(NameTag, Name, Type)                                       \
  case Meta::Name: {                                                           \
    Name = endian::readNext<Type, little, unaligned>(Ptr);                     \
  } break;
#include "llvm/ProfileData/MIBEntryDef.inc"
#undef MIBEntryDef
      default:
        llvm_unreachable("Unknown meta type id, is the profile collected from "
                         "a newer version of the runtime?");
      }
    }
  }

  // Write the contents of the MemInfoBlock based on the \p Schema provided to
  // the raw_ostream \p OS.
  void serialize(const MemProfSchema &Schema, raw_ostream &OS) const {
    using namespace support;

    endian::Writer LE(OS, little);
    for (const Meta Id : Schema) {
      switch (Id) {
#define MIBEntryDef(NameTag, Name, Type)                                       \
  case Meta::Name: {                                                           \
    LE.write<Type>(Name);                                                      \
  } break;
#include "llvm/ProfileData/MIBEntryDef.inc"
#undef MIBEntryDef
      default:
        llvm_unreachable("Unknown meta type id, invalid input?");
      }
    }
  }

  // Print out the contents of the MemInfoBlock in YAML format.
  void printYAML(raw_ostream &OS) const {
    OS << "      MemInfoBlock:\n";
#define MIBEntryDef(NameTag, Name, Type)                                       \
  OS << "        " << #Name << ": " << Name << "\n";
#include "llvm/ProfileData/MIBEntryDef.inc"
#undef MIBEntryDef
  }

  // Define getters for each type which can be called by analyses.
#define MIBEntryDef(NameTag, Name, Type)                                       \
  Type get##Name() const { return Name; }
#include "llvm/ProfileData/MIBEntryDef.inc"
#undef MIBEntryDef

  void clear() { *this = PortableMemInfoBlock(); }

  // Returns the full schema currently in use.
  static MemProfSchema getSchema() {
    MemProfSchema List;
#define MIBEntryDef(NameTag, Name, Type) List.push_back(Meta::Name);
#include "llvm/ProfileData/MIBEntryDef.inc"
#undef MIBEntryDef
    return List;
  }

  bool operator==(const PortableMemInfoBlock &Other) const {
#define MIBEntryDef(NameTag, Name, Type)                                       \
  if (Other.get##Name() != get##Name())                                        \
    return false;
#include "llvm/ProfileData/MIBEntryDef.inc"
#undef MIBEntryDef
    return true;
  }

  bool operator!=(const PortableMemInfoBlock &Other) const {
    return !operator==(Other);
  }

  static constexpr size_t serializedSize() {
    size_t Result = 0;
#define MIBEntryDef(NameTag, Name, Type) Result += sizeof(Type);
#include "llvm/ProfileData/MIBEntryDef.inc"
#undef MIBEntryDef
    return Result;
  }

private:
#define MIBEntryDef(NameTag, Name, Type) Type Name = Type();
#include "llvm/ProfileData/MIBEntryDef.inc"
#undef MIBEntryDef
};

// A type representing the id generated by hashing the contents of the Frame.
using FrameId = uint64_t;
// Describes a call frame for a dynamic allocation context. The contents of
// the frame are populated by symbolizing the stack depot call frame from the
// compiler runtime.
struct Frame {
  // A uuid (uint64_t) identifying the function. It is obtained by
  // llvm::md5(FunctionName) which returns the lower 64 bits.
  GlobalValue::GUID Function;
  // The symbol name for the function. Only populated in the Frame by the reader
  // if requested during initialization. This field should not be serialized.
  std::optional<std::string> SymbolName;
  // The source line offset of the call from the beginning of parent function.
  uint32_t LineOffset;
  // The source column number of the call to help distinguish multiple calls
  // on the same line.
  uint32_t Column;
  // Whether the current frame is inlined.
  bool IsInlineFrame;

  Frame(const Frame &Other) {
    Function = Other.Function;
    SymbolName = Other.SymbolName;
    LineOffset = Other.LineOffset;
    Column = Other.Column;
    IsInlineFrame = Other.IsInlineFrame;
  }

  Frame(uint64_t Hash, uint32_t Off, uint32_t Col, bool Inline)
      : Function(Hash), LineOffset(Off), Column(Col), IsInlineFrame(Inline) {}

  bool operator==(const Frame &Other) const {
    // Ignore the SymbolName field to avoid a string compare. Comparing the
    // function hash serves the same purpose.
    return Other.Function == Function && Other.LineOffset == LineOffset &&
           Other.Column == Column && Other.IsInlineFrame == IsInlineFrame;
  }

  Frame &operator=(const Frame &Other) {
    Function = Other.Function;
    SymbolName = Other.SymbolName;
    LineOffset = Other.LineOffset;
    Column = Other.Column;
    IsInlineFrame = Other.IsInlineFrame;
    return *this;
  }

  bool operator!=(const Frame &Other) const { return !operator==(Other); }

  // Write the contents of the frame to the ostream \p OS.
  void serialize(raw_ostream &OS) const {
    using namespace support;

    endian::Writer LE(OS, little);

    // If the type of the GlobalValue::GUID changes, then we need to update
    // the reader and the writer.
    static_assert(std::is_same<GlobalValue::GUID, uint64_t>::value,
                  "Expect GUID to be uint64_t.");
    LE.write<uint64_t>(Function);

    LE.write<uint32_t>(LineOffset);
    LE.write<uint32_t>(Column);
    LE.write<bool>(IsInlineFrame);
  }

  // Read a frame from char data which has been serialized as little endian.
  static Frame deserialize(const unsigned char *Ptr) {
    using namespace support;

    const uint64_t F = endian::readNext<uint64_t, little, unaligned>(Ptr);
    const uint32_t L = endian::readNext<uint32_t, little, unaligned>(Ptr);
    const uint32_t C = endian::readNext<uint32_t, little, unaligned>(Ptr);
    const bool I = endian::readNext<bool, little, unaligned>(Ptr);
    return Frame(/*Function=*/F, /*LineOffset=*/L, /*Column=*/C,
                 /*IsInlineFrame=*/I);
  }

  // Returns the size of the frame information.
  static constexpr size_t serializedSize() {
    return sizeof(Frame::Function) + sizeof(Frame::LineOffset) +
           sizeof(Frame::Column) + sizeof(Frame::IsInlineFrame);
  }

  // Print the frame information in YAML format.
  void printYAML(raw_ostream &OS) const {
    OS << "      -\n"
       << "        Function: " << Function << "\n"
       << "        SymbolName: " << SymbolName.value_or("<None>") << "\n"
       << "        LineOffset: " << LineOffset << "\n"
       << "        Column: " << Column << "\n"
       << "        Inline: " << IsInlineFrame << "\n";
  }

  // Return a hash value based on the contents of the frame. Here we don't use
  // hashing from llvm ADT since we are going to persist the hash id, the hash
  // combine algorithm in ADT uses a new randomized seed each time.
  inline FrameId hash() const {
    auto HashCombine = [](auto Value, size_t Seed) {
      std::hash<decltype(Value)> Hasher;
      // The constant used below is the 64 bit representation of the fractional
      // part of the golden ratio. Used here for the randomness in their bit
      // pattern.
      return Hasher(Value) + 0x9e3779b97f4a7c15 + (Seed << 6) + (Seed >> 2);
    };

    size_t Result = 0;
    Result ^= HashCombine(Function, Result);
    Result ^= HashCombine(LineOffset, Result);
    Result ^= HashCombine(Column, Result);
    Result ^= HashCombine(IsInlineFrame, Result);
    return static_cast<FrameId>(Result);
  }
};

// Holds allocation information in a space efficient format where frames are
// represented using unique identifiers.
struct IndexedAllocationInfo {
  // The dynamic calling context for the allocation in bottom-up (leaf-to-root)
  // order. Frame contents are stored out-of-line.
  llvm::SmallVector<FrameId> CallStack;
  // The statistics obtained from the runtime for the allocation.
  PortableMemInfoBlock Info;

  IndexedAllocationInfo() = default;
  IndexedAllocationInfo(ArrayRef<FrameId> CS, const MemInfoBlock &MB)
      : CallStack(CS.begin(), CS.end()), Info(MB) {}

  // Returns the size in bytes when this allocation info struct is serialized.
  size_t serializedSize() const {
    return sizeof(uint64_t) + // The number of frames to serialize.
           sizeof(FrameId) * CallStack.size() +    // The callstack frame ids.
           PortableMemInfoBlock::serializedSize(); // The size of the payload.
  }

  bool operator==(const IndexedAllocationInfo &Other) const {
    if (Other.Info != Info)
      return false;

    if (Other.CallStack.size() != CallStack.size())
      return false;

    for (size_t J = 0; J < Other.CallStack.size(); J++) {
      if (Other.CallStack[J] != CallStack[J])
        return false;
    }
    return true;
  }

  bool operator!=(const IndexedAllocationInfo &Other) const {
    return !operator==(Other);
  }
};

// Holds allocation information with frame contents inline. The type should
// be used for temporary in-memory instances.
struct AllocationInfo {
  // Same as IndexedAllocationInfo::CallStack with the frame contents inline.
  llvm::SmallVector<Frame> CallStack;
  // Same as IndexedAllocationInfo::Info;
  PortableMemInfoBlock Info;

  AllocationInfo() = default;
  AllocationInfo(
      const IndexedAllocationInfo &IndexedAI,
      llvm::function_ref<const Frame(const FrameId)> IdToFrameCallback) {
    for (const FrameId &Id : IndexedAI.CallStack) {
      CallStack.push_back(IdToFrameCallback(Id));
    }
    Info = IndexedAI.Info;
  }

  void printYAML(raw_ostream &OS) const {
    OS << "    -\n";
    OS << "      Callstack:\n";
    // TODO: Print out the frame on one line with to make it easier for deep
    // callstacks once we have a test to check valid YAML is generated.
    for (const Frame &F : CallStack) {
      F.printYAML(OS);
    }
    Info.printYAML(OS);
  }
};

// Holds the memprof profile information for a function. The internal
// representation stores frame ids for efficiency. This representation should
// be used in the profile conversion and manipulation tools.
struct IndexedMemProfRecord {
  // Memory allocation sites in this function for which we have memory
  // profiling data.
  llvm::SmallVector<IndexedAllocationInfo> AllocSites;
  // Holds call sites in this function which are part of some memory
  // allocation context. We store this as a list of locations, each with its
  // list of inline locations in bottom-up order i.e. from leaf to root. The
  // inline location list may include additional entries, users should pick
  // the last entry in the list with the same function GUID.
  llvm::SmallVector<llvm::SmallVector<FrameId>> CallSites;

  void clear() {
    AllocSites.clear();
    CallSites.clear();
  }

  void merge(const IndexedMemProfRecord &Other) {
    // TODO: Filter out duplicates which may occur if multiple memprof
    // profiles are merged together using llvm-profdata.
    AllocSites.append(Other.AllocSites);
    CallSites.append(Other.CallSites);
  }

  size_t serializedSize() const {
    size_t Result = sizeof(GlobalValue::GUID);
    for (const IndexedAllocationInfo &N : AllocSites)
      Result += N.serializedSize();

    // The number of callsites we have information for.
    Result += sizeof(uint64_t);
    for (const auto &Frames : CallSites) {
      // The number of frame ids to serialize.
      Result += sizeof(uint64_t);
      Result += Frames.size() * sizeof(FrameId);
    }
    return Result;
  }

  bool operator==(const IndexedMemProfRecord &Other) const {
    if (Other.AllocSites.size() != AllocSites.size())
      return false;

    if (Other.CallSites.size() != CallSites.size())
      return false;

    for (size_t I = 0; I < AllocSites.size(); I++) {
      if (AllocSites[I] != Other.AllocSites[I])
        return false;
    }

    for (size_t I = 0; I < CallSites.size(); I++) {
      if (CallSites[I] != Other.CallSites[I])
        return false;
    }
    return true;
  }

  // Serializes the memprof records in \p Records to the ostream \p OS based
  // on the schema provided in \p Schema.
  void serialize(const MemProfSchema &Schema, raw_ostream &OS);

  // Deserializes memprof records from the Buffer.
  static IndexedMemProfRecord deserialize(const MemProfSchema &Schema,
                                          const unsigned char *Buffer);

  // Returns the GUID for the function name after canonicalization. For
  // memprof, we remove any .llvm suffix added by LTO. MemProfRecords are
  // mapped to functions using this GUID.
  static GlobalValue::GUID getGUID(const StringRef FunctionName);
};

// Holds the memprof profile information for a function. The internal
// representation stores frame contents inline. This representation should
// be used for small amount of temporary, in memory instances.
struct MemProfRecord {
  // Same as IndexedMemProfRecord::AllocSites with frame contents inline.
  llvm::SmallVector<AllocationInfo> AllocSites;
  // Same as IndexedMemProfRecord::CallSites with frame contents inline.
  llvm::SmallVector<llvm::SmallVector<Frame>> CallSites;

  MemProfRecord() = default;
  MemProfRecord(
      const IndexedMemProfRecord &Record,
      llvm::function_ref<const Frame(const FrameId Id)> IdToFrameCallback) {
    for (const IndexedAllocationInfo &IndexedAI : Record.AllocSites) {
      AllocSites.emplace_back(IndexedAI, IdToFrameCallback);
    }
    for (const ArrayRef<FrameId> Site : Record.CallSites) {
      llvm::SmallVector<Frame> Frames;
      for (const FrameId Id : Site) {
        Frames.push_back(IdToFrameCallback(Id));
      }
      CallSites.push_back(Frames);
    }
  }

  // Prints out the contents of the memprof record in YAML.
  void print(llvm::raw_ostream &OS) const {
    if (!AllocSites.empty()) {
      OS << "    AllocSites:\n";
      for (const AllocationInfo &N : AllocSites)
        N.printYAML(OS);
    }

    if (!CallSites.empty()) {
      OS << "    CallSites:\n";
      for (const llvm::SmallVector<Frame> &Frames : CallSites) {
        for (const Frame &F : Frames) {
          OS << "    -\n";
          F.printYAML(OS);
        }
      }
    }
  }
};

// Reads a memprof schema from a buffer. All entries in the buffer are
// interpreted as uint64_t. The first entry in the buffer denotes the number of
// ids in the schema. Subsequent entries are integers which map to memprof::Meta
// enum class entries. After successfully reading the schema, the pointer is one
// byte past the schema contents.
Expected<MemProfSchema> readMemProfSchema(const unsigned char *&Buffer);

// Trait for reading IndexedMemProfRecord data from the on-disk hash table.
class RecordLookupTrait {
public:
  using data_type = const IndexedMemProfRecord &;
  using internal_key_type = uint64_t;
  using external_key_type = uint64_t;
  using hash_value_type = uint64_t;
  using offset_type = uint64_t;

  RecordLookupTrait() = delete;
  RecordLookupTrait(const MemProfSchema &S) : Schema(S) {}

  static bool EqualKey(uint64_t A, uint64_t B) { return A == B; }
  static uint64_t GetInternalKey(uint64_t K) { return K; }
  static uint64_t GetExternalKey(uint64_t K) { return K; }

  hash_value_type ComputeHash(uint64_t K) { return K; }

  static std::pair<offset_type, offset_type>
  ReadKeyDataLength(const unsigned char *&D) {
    using namespace support;

    offset_type KeyLen = endian::readNext<offset_type, little, unaligned>(D);
    offset_type DataLen = endian::readNext<offset_type, little, unaligned>(D);
    return std::make_pair(KeyLen, DataLen);
  }

  uint64_t ReadKey(const unsigned char *D, offset_type /*Unused*/) {
    using namespace support;
    return endian::readNext<external_key_type, little, unaligned>(D);
  }

  data_type ReadData(uint64_t K, const unsigned char *D,
                     offset_type /*Unused*/) {
    Record = IndexedMemProfRecord::deserialize(Schema, D);
    return Record;
  }

private:
  // Holds the memprof schema used to deserialize records.
  MemProfSchema Schema;
  // Holds the records from one function deserialized from the indexed format.
  IndexedMemProfRecord Record;
};

// Trait for writing IndexedMemProfRecord data to the on-disk hash table.
class RecordWriterTrait {
public:
  using key_type = uint64_t;
  using key_type_ref = uint64_t;

  using data_type = IndexedMemProfRecord;
  using data_type_ref = IndexedMemProfRecord &;

  using hash_value_type = uint64_t;
  using offset_type = uint64_t;

  // Pointer to the memprof schema to use for the generator. Unlike the reader
  // we must use a default constructor with no params for the writer trait so we
  // have a public member which must be initialized by the user.
  MemProfSchema *Schema = nullptr;

  RecordWriterTrait() = default;

  static hash_value_type ComputeHash(key_type_ref K) { return K; }

  static std::pair<offset_type, offset_type>
  EmitKeyDataLength(raw_ostream &Out, key_type_ref K, data_type_ref V) {
    using namespace support;

    endian::Writer LE(Out, little);
    offset_type N = sizeof(K);
    LE.write<offset_type>(N);
    offset_type M = V.serializedSize();
    LE.write<offset_type>(M);
    return std::make_pair(N, M);
  }

  void EmitKey(raw_ostream &Out, key_type_ref K, offset_type /*Unused*/) {
    using namespace support;
    endian::Writer LE(Out, little);
    LE.write<uint64_t>(K);
  }

  void EmitData(raw_ostream &Out, key_type_ref /*Unused*/, data_type_ref V,
                offset_type /*Unused*/) {
    assert(Schema != nullptr && "MemProf schema is not initialized!");
    V.serialize(*Schema, Out);
  }
};

// Trait for writing frame mappings to the on-disk hash table.
class FrameWriterTrait {
public:
  using key_type = FrameId;
  using key_type_ref = FrameId;

  using data_type = Frame;
  using data_type_ref = Frame &;

  using hash_value_type = FrameId;
  using offset_type = uint64_t;

  static hash_value_type ComputeHash(key_type_ref K) { return K; }

  static std::pair<offset_type, offset_type>
  EmitKeyDataLength(raw_ostream &Out, key_type_ref K, data_type_ref V) {
    using namespace support;
    endian::Writer LE(Out, little);
    offset_type N = sizeof(K);
    LE.write<offset_type>(N);
    offset_type M = V.serializedSize();
    LE.write<offset_type>(M);
    return std::make_pair(N, M);
  }

  void EmitKey(raw_ostream &Out, key_type_ref K, offset_type /*Unused*/) {
    using namespace support;
    endian::Writer LE(Out, little);
    LE.write<key_type>(K);
  }

  void EmitData(raw_ostream &Out, key_type_ref /*Unused*/, data_type_ref V,
                offset_type /*Unused*/) {
    V.serialize(Out);
  }
};

// Trait for reading frame mappings from the on-disk hash table.
class FrameLookupTrait {
public:
  using data_type = const Frame;
  using internal_key_type = FrameId;
  using external_key_type = FrameId;
  using hash_value_type = FrameId;
  using offset_type = uint64_t;

  static bool EqualKey(internal_key_type A, internal_key_type B) {
    return A == B;
  }
  static uint64_t GetInternalKey(internal_key_type K) { return K; }
  static uint64_t GetExternalKey(external_key_type K) { return K; }

  hash_value_type ComputeHash(internal_key_type K) { return K; }

  static std::pair<offset_type, offset_type>
  ReadKeyDataLength(const unsigned char *&D) {
    using namespace support;

    offset_type KeyLen = endian::readNext<offset_type, little, unaligned>(D);
    offset_type DataLen = endian::readNext<offset_type, little, unaligned>(D);
    return std::make_pair(KeyLen, DataLen);
  }

  uint64_t ReadKey(const unsigned char *D, offset_type /*Unused*/) {
    using namespace support;
    return endian::readNext<external_key_type, little, unaligned>(D);
  }

  data_type ReadData(uint64_t K, const unsigned char *D,
                     offset_type /*Unused*/) {
    return Frame::deserialize(D);
  }
};
} // namespace memprof
} // namespace llvm

#endif // LLVM_PROFILEDATA_MEMPROF_H_
