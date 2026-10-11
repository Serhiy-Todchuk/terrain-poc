#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Minimal HDF5 reader, enough for the NetCDF-4 files of the GOES-R ABI Level-2
// products in the NOAA open-data buckets (docs/weather_roadmap.md, step 3):
// superblock version 2 / 3, version-2 object headers with continuation blocks,
// groups with compact or dense links (fractal heap + version-2 B-tree), compact
// or dense attributes, scalar / simple dataspaces, integer, IEEE float (either
// byte order) and fixed-length string types, data layout version 3 (compact,
// contiguous, chunked with a version-1 B-tree index), the shuffle and deflate
// filters (zlib-ng). Anything else is reported as an error, never guessed;
// checksums are not verified. The file is held in memory and every access goes
// through one bounds-checked reader, so a later version can fetch byte ranges
// (only the chunks over the weather box) on demand. Plain C++ (no Windows /
// D3D), so it can move into a WASM build unchanged.
namespace Hdf5 {

constexpr uint64_t kUndefined = ~uint64_t(0); // "undefined address"

struct Datatype {
  enum class Class { Integer, Float, String, Other };
  Class cls = Class::Other;
  int hdfClass = -1;      // HDF5 datatype class (0 fixed-point, 1 floating-point, 3 string, 9 variable-length, ...)
  uint32_t size = 0;      // bytes per element
  bool isSigned = false;  // Integer
  bool bigEndian = false; // Integer / Float, as stored in the file

  std::string describe() const; // "u16", "f32", "string[12]", "class 9 (16 B)"
};

struct Attribute {
  std::string name;
  Datatype type;
  std::vector<uint64_t> dims; // empty = scalar
  std::vector<std::byte> data; // as stored (byte order of `type`)

  uint64_t count() const; // elements
  // Element i as a number (Integer / Float), nullopt otherwise.
  std::optional<double> number(uint64_t i = 0) const;
  // The text of a String attribute (up to the first null), "" otherwise.
  std::string text() const;
  std::string describe() const; // the value(s), shortened for logs
};

// An HDF5 dataset (a NetCDF variable or dimension).
struct Variable {
  enum class Layout { Compact, Contiguous, Chunked, Unsupported };
  std::string name;               // path below the root group: "HT", "group/name"
  Datatype type;
  std::vector<uint64_t> dims;     // empty = scalar
  std::vector<Attribute> attributes;
  Layout layout = Layout::Unsupported;
  std::string layoutProblem;      // why the layout is Unsupported
  uint64_t address = kUndefined;  // contiguous data / root of the chunk index (v1 B-tree)
  uint64_t storageSize = 0;       // contiguous: bytes
  std::vector<std::byte> compactData;
  std::vector<uint64_t> chunk;    // chunked: chunk dimensions (rank entries)
  std::vector<int> filters;       // filter ids in pipeline order (1 deflate, 2 shuffle, ...)
  std::vector<std::byte> fillValue; // one element; zeros when the file defines none

  uint64_t count() const; // elements
  bool hasData() const;   // false: never written (all fill), e.g. NetCDF dimensions without a variable
  const Attribute* attribute(std::string_view attributeName) const;
  std::string describe() const; // "u16[300x500] chunked 262x500 shuffle+deflate"
};

class File {
public:
  // Parses the structure of a whole file held in memory (groups, variables,
  // attributes). False with a description in `error` when it is not HDF5 or
  // uses a structure this reader does not support.
  bool open(std::vector<std::byte> data, std::string& error);

  const std::vector<Attribute>& globalAttributes() const { return _global; } // root group
  const std::vector<Variable>& variables() const { return _variables; }    // sorted by name
  const Variable* variable(std::string_view name) const;
  const Attribute* globalAttribute(std::string_view name) const;
  size_t sizeBytes() const { return _data.size(); }

  // Raw elements of the hyperslab [start, start + count) in row-major order
  // (last dimension fastest), little-endian; elements without stored data hold
  // the fill value. Empty start / count = the whole variable. Only the chunks
  // that intersect the hyperslab are decoded.
  bool read(const Variable& v, std::span<const uint64_t> start, std::span<const uint64_t> count,
            std::vector<std::byte>& out, std::string& error) const;

  // Numeric values with the NetCDF / CF conventions: _FillValue and values
  // outside valid_range / valid_min / valid_max -> NaN, _Unsigned, then
  // scale_factor and add_offset.
  bool readValues(const Variable& v, std::span<const uint64_t> start, std::span<const uint64_t> count,
                  std::vector<float>& out, std::string& error) const;
  bool readValues(const Variable& v, std::span<const uint64_t> start, std::span<const uint64_t> count,
                  std::vector<double>& out, std::string& error) const;

private:
  struct Message;
  struct Chunk;

  std::vector<Message> objectHeader(uint64_t address) const;
  void visit(uint64_t address, const std::string& path, std::vector<uint64_t>& visited);
  std::vector<std::pair<std::string, uint64_t>> denseLinks(uint64_t heap, uint64_t nameIndex) const;
  std::vector<Attribute> denseAttributes(uint64_t heap, uint64_t nameIndex) const;
  Attribute attributeMessage(std::span<const std::byte> message) const;
  void chunks(const Variable& v, uint64_t node, std::vector<Chunk>& out, int depth) const;
  template <typename T>
  bool readValuesT(const Variable& v, std::span<const uint64_t> start, std::span<const uint64_t> count,
                   std::vector<T>& out, std::string& error) const;

  std::vector<std::byte> _data;
  int _offsetSize = 8, _lengthSize = 8;
  std::vector<Attribute> _global;
  std::vector<Variable> _variables;
};

} // namespace Hdf5
