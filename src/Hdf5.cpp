#include "pch.h"
#include "Hdf5.h"

#include <zlib-ng/zlib-ng.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

// Format reference: "HDF5 File Format Specification Version 3.0" (The HDF
// Group). Section numbers below refer to it.

namespace Hdf5 {

static_assert(std::endian::native == std::endian::little, "the reader assumes a little-endian host");

// Positions of the structures read in the file, for the private methods.
struct File::Message {
  int type = 0;
  uint8_t flags = 0;
  uint64_t offset = 0; // of the message data
  uint64_t size = 0;
};

struct File::Chunk {
  std::vector<uint64_t> offset; // element index of the chunk's first element per dimension
  uint32_t size = 0;            // stored (filtered) bytes
  uint32_t filterMask = 0;      // bit i set: filter i of the pipeline was not applied
  uint64_t address = 0;
};

namespace {

// Structural problems are thrown inside the reader and reported by open() / read().
struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const std::string& what) { throw Error(what); }

std::string hex(uint64_t v) {
  char buf[24];
  snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

constexpr uint64_t kMaxReadBytes = uint64_t(1) << 30; // sanity limit for one read

// Little-endian reads from a bounds-checked view of the file (or of a heap object).
class Bytes {
public:
  Bytes(std::span<const std::byte> data, int offsetSize, int lengthSize)
      : _d(data), _o(offsetSize), _l(lengthSize) {}

  void check(uint64_t at, uint64_t n) const {
    if (at > _d.size() || n > _d.size() - at) {
      fail("read of " + std::to_string(n) + " bytes at " + hex(at) + " past the end (" + std::to_string(_d.size()) +
           " bytes)");
    }
  }
  uint64_t u(uint64_t at, int n) const {
    check(at, uint64_t(n));
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; --i) {
      v = (v << 8) | uint64_t(_d[size_t(at) + size_t(i)]);
    }
    return v;
  }
  uint8_t u8(uint64_t at) const { return uint8_t(u(at, 1)); }
  uint16_t u16(uint64_t at) const { return uint16_t(u(at, 2)); }
  uint32_t u32(uint64_t at) const { return uint32_t(u(at, 4)); }
  // An address; all bits set = undefined.
  uint64_t offset(uint64_t at) const {
    const uint64_t v = u(at, _o);
    return _o < 8 && v == (uint64_t(1) << (8 * _o)) - 1 ? kUndefined : v;
  }
  uint64_t length(uint64_t at) const { return u(at, _l); }
  std::span<const std::byte> span(uint64_t at, uint64_t n) const {
    check(at, n);
    return _d.subspan(size_t(at), size_t(n));
  }
  bool signature(uint64_t at, const char* sig) const {
    check(at, 4);
    return std::memcmp(_d.data() + at, sig, 4) == 0;
  }
  // A name of n bytes, up to the first null.
  std::string name(uint64_t at, uint64_t n) const {
    const auto s = span(at, n);
    const std::string text(reinterpret_cast<const char*>(s.data()), s.size());
    return text.substr(0, text.find('\0'));
  }
  int O() const { return _o; }
  int L() const { return _l; }

private:
  std::span<const std::byte> _d;
  int _o, _l;
};

int log2Floor(uint64_t v) {
  int r = 0;
  while (v >>= 1) {
    ++r;
  }
  return r;
}
// Bytes needed to encode values up to v (H5VM_limit_enc_size).
int limitEncSize(uint64_t v) { return log2Floor(v) / 8 + 1; }
uint64_t pad8(uint64_t n) { return (n + 7) & ~uint64_t(7); }

// IV.A.2.d Datatype message. Variants outside the supported set keep their
// class and size (attributes stay listed) but cannot be read as numbers / text.
Datatype parseDatatype(const Bytes& b, uint64_t p) {
  Datatype t;
  t.hdfClass = b.u8(p) & 0x0f;
  const uint32_t bits = uint32_t(b.u(p + 1, 3));
  t.size = b.u32(p + 4);
  if (t.hdfClass == 0) { // fixed-point
    const uint16_t bitOffset = b.u16(p + 8), precision = b.u16(p + 10);
    if (bitOffset == 0 && precision == t.size * 8 && (t.size == 1 || t.size == 2 || t.size == 4 || t.size == 8)) {
      t.cls = Datatype::Class::Integer;
      t.bigEndian = (bits & 0x01) != 0;
      t.isSigned = (bits & 0x08) != 0;
    }
  } else if (t.hdfClass == 1) { // floating-point: IEEE 754 binary32 / binary64 only
    const uint16_t bitOffset = b.u16(p + 8), precision = b.u16(p + 10);
    const uint8_t expLocation = b.u8(p + 12), expSize = b.u8(p + 13), manLocation = b.u8(p + 14),
                  manSize = b.u8(p + 15);
    const uint32_t bias = b.u32(p + 16), sign = (bits >> 8) & 0xff;
    const bool vax = (bits & 0x40) != 0;
    const bool ieee32 = t.size == 4 && precision == 32 && expLocation == 23 && expSize == 8 && manSize == 23 &&
                        bias == 127 && sign == 31;
    const bool ieee64 = t.size == 8 && precision == 64 && expLocation == 52 && expSize == 11 && manSize == 52 &&
                        bias == 1023 && sign == 63;
    if (!vax && bitOffset == 0 && manLocation == 0 && (ieee32 || ieee64)) {
      t.cls = Datatype::Class::Float;
      t.bigEndian = (bits & 0x01) != 0;
    }
  } else if (t.hdfClass == 3) { // fixed-length string
    t.cls = Datatype::Class::String;
  }
  return t;
}

// IV.A.2.b Dataspace message. A null dataspace has no elements ({0}).
std::vector<uint64_t> parseDataspace(const Bytes& b, uint64_t p) {
  const uint8_t version = b.u8(p), rank = b.u8(p + 1);
  uint64_t q = 0;
  if (version == 1) {
    q = p + 8;
  } else if (version == 2) {
    if (b.u8(p + 3) == 2) {
      return {0};
    }
    q = p + 4;
  } else {
    fail("dataspace message version " + std::to_string(version));
  }
  std::vector<uint64_t> dims;
  for (int i = 0; i < rank; ++i) {
    dims.push_back(b.length(q + uint64_t(i) * uint64_t(b.L())));
  }
  return dims;
}

// IV.A.2.g Link message: a hard link (name, object header address).
std::pair<std::string, uint64_t> parseLink(const Bytes& b, uint64_t p) {
  const uint8_t version = b.u8(p), flags = b.u8(p + 1);
  if (version != 1) {
    fail("link message version " + std::to_string(version));
  }
  uint64_t q = p + 2;
  int type = 0;
  if (flags & 0x08) {
    type = b.u8(q++);
  }
  if (flags & 0x04) {
    q += 8; // creation order
  }
  if (flags & 0x10) {
    q += 1; // name character set
  }
  const int lengthSize = 1 << (flags & 3);
  const uint64_t nameLength = b.u(q, lengthSize);
  q += uint64_t(lengthSize);
  std::string name = b.name(q, nameLength);
  q += nameLength;
  if (type != 0) {
    fail("link '" + name + "': soft / external links are not supported");
  }
  return {name, b.offset(q)};
}

// III.G Fractal heap (the dense storage of links and attributes).
struct Heap {
  uint16_t idLength = 0;
  uint32_t maxManaged = 0;
  uint16_t width = 0;
  uint64_t startBlock = 0, maxDirect = 0;
  uint64_t root = kUndefined;
  int rootRows = 0;
  int offsetBytes = 0, lengthBytes = 0; // fields of a managed object's heap ID
};

Heap readHeap(const Bytes& b, uint64_t a) {
  if (!b.signature(a, "FRHP")) {
    fail("no fractal heap at " + hex(a));
  }
  const uint64_t O = uint64_t(b.O()), L = uint64_t(b.L());
  Heap h;
  uint64_t p = a + 5;
  h.idLength = b.u16(p);
  const uint16_t filterLength = b.u16(p + 2);
  p += 5; // + flags
  h.maxManaged = b.u32(p);
  p += 4 + L + O;         // next huge object ID, huge objects B-tree
  p += L + O;             // free space, free-space manager
  p += 3 * L;             // managed space, allocated managed space, direct block iterator
  p += L + 4 * L;         // managed / huge / tiny object counts and sizes
  h.width = b.u16(p);
  h.startBlock = b.length(p + 2);
  h.maxDirect = b.length(p + 2 + L);
  const uint16_t maxHeapBits = b.u16(p + 2 + 2 * L);
  p += 2 + 2 * L + 2 + 2; // + starting number of rows
  h.root = b.offset(p);
  h.rootRows = b.u16(p + O);
  if (filterLength != 0) {
    fail("fractal heap at " + hex(a) + ": filtered heaps are not supported");
  }
  if (h.width == 0 || !std::has_single_bit(h.startBlock) || !std::has_single_bit(h.maxDirect) ||
      h.maxDirect < h.startBlock || maxHeapBits == 0 || maxHeapBits > 64) {
    fail("fractal heap at " + hex(a) + ": unexpected doubling table");
  }
  h.offsetBytes = (maxHeapBits + 7) / 8;
  h.lengthBytes = std::min((log2Floor(h.maxDirect) + 7) / 8, limitEncSize(h.maxManaged));
  return h;
}

uint64_t rowBlockSize(const Heap& h, int row) { return row == 0 ? h.startBlock : h.startBlock << (row - 1); }

// The bytes of a managed object: walk the doubling table from the root block.
std::span<const std::byte> managedObject(const Bytes& b, const Heap& h, uint64_t offset, uint64_t length) {
  if (h.root == kUndefined) {
    fail("empty fractal heap");
  }
  if (h.rootRows == 0) { // the root is a direct block
    if (offset + length > h.startBlock) {
      fail("heap object past the root direct block");
    }
    return b.span(h.root + offset, length);
  }
  const uint64_t O = uint64_t(b.O());
  uint64_t block = h.root, base = 0;
  int rows = h.rootRows;
  for (int depth = 0; depth < 16; ++depth) {
    if (!b.signature(block, "FHIB")) {
      fail("no fractal heap indirect block at " + hex(block));
    }
    uint64_t entry = block + 5 + O + uint64_t(h.offsetBytes); // child block addresses, row by row
    uint64_t rowStart = base;
    bool descended = false;
    for (int r = 0; r < rows && !descended; ++r) {
      const uint64_t size = rowBlockSize(h, r);
      if (offset >= rowStart + size * h.width) {
        rowStart += size * h.width;
        entry += O * h.width;
        continue;
      }
      const uint64_t column = (offset - rowStart) / size, childStart = rowStart + column * size;
      const uint64_t child = b.offset(entry + column * O);
      if (child == kUndefined) {
        fail("heap object in an unallocated block");
      }
      if (size <= h.maxDirect) {
        if (offset - childStart + length > size) {
          fail("heap object past its direct block");
        }
        return b.span(child + (offset - childStart), length);
      }
      // A child indirect block: its rows cover `size` bytes.
      block = child;
      base = childStart;
      rows = log2Floor(size) - log2Floor(h.startBlock * h.width) + 1;
      descended = true;
    }
    if (!descended) {
      fail("heap offset " + hex(offset) + " past the heap");
    }
  }
  fail("fractal heap nested too deep");
}

std::span<const std::byte> heapObject(const Bytes& b, const Heap& h, std::span<const std::byte> id) {
  if (id.empty() || (uint8_t(id[0]) >> 6) != 0) {
    fail("unsupported heap ID version");
  }
  const int type = (uint8_t(id[0]) >> 4) & 3;
  if (type == 2) { // tiny object: the data is in the ID
    const size_t length = size_t(uint8_t(id[0]) & 0x0f) + 1;
    if (1 + length > id.size()) {
      fail("tiny heap object longer than its ID");
    }
    return id.subspan(1, length);
  }
  if (type != 0) {
    fail("huge heap objects are not supported");
  }
  if (id.size() < size_t(1 + h.offsetBytes + h.lengthBytes)) {
    fail("heap ID too short");
  }
  const Bytes idBytes(id, b.O(), b.L());
  const uint64_t offset = idBytes.u(1, h.offsetBytes), length = idBytes.u(1 + uint64_t(h.offsetBytes), h.lengthBytes);
  return managedObject(b, h, offset, length);
}

// III.A.2 Version-2 B-tree: all records in key order (depth-first, in order).
std::vector<std::span<const std::byte>> btree2Records(const Bytes& b, uint64_t a, int expectedType) {
  if (!b.signature(a, "BTHD")) {
    fail("no v2 B-tree header at " + hex(a));
  }
  const uint64_t O = uint64_t(b.O());
  const int type = b.u8(a + 5);
  const uint32_t nodeSize = b.u32(a + 6);
  const uint16_t recordSize = b.u16(a + 10), depth = b.u16(a + 12);
  const uint64_t root = b.offset(a + 16);
  const uint16_t rootCount = b.u16(a + 16 + O);
  const uint64_t total = b.length(a + 18 + O);
  if (type != expectedType) {
    fail("v2 B-tree at " + hex(a) + ": type " + std::to_string(type) + ", expected " + std::to_string(expectedType));
  }
  if (recordSize == 0 || nodeSize <= 10 || depth > 16) {
    fail("v2 B-tree at " + hex(a) + ": unexpected node / record size or depth");
  }
  std::vector<std::span<const std::byte>> out;
  if (root == kUndefined || total == 0) {
    return out;
  }
  // Node capacities per depth, as the library computes them (H5B2__hdr_init):
  // they size the record-count fields of the child pointers.
  constexpr uint64_t kPrefix = 10; // signature, version, type, checksum
  std::vector<uint64_t> maxRecords(depth + 1u), cumulative(depth + 1u);
  std::vector<int> cumulativeSize(depth + 1u, 0);
  maxRecords[0] = (nodeSize - kPrefix) / recordSize;
  cumulative[0] = maxRecords[0];
  const int countSize = limitEncSize(maxRecords[0]);
  for (int d = 1; d <= depth; ++d) {
    const uint64_t pointerSize = O + uint64_t(countSize) + (d > 1 ? uint64_t(cumulativeSize[d - 1]) : 0);
    maxRecords[d] = (nodeSize - (kPrefix + pointerSize)) / (recordSize + pointerSize);
    cumulative[d] = (maxRecords[d] + 1) * cumulative[d - 1] + maxRecords[d];
    cumulativeSize[d] = limitEncSize(cumulative[d]);
  }
  auto node = [&](auto&& self, uint64_t address, uint64_t count, int d) -> void {
    if (!b.signature(address, d == 0 ? "BTLF" : "BTIN")) {
      fail("no v2 B-tree node at " + hex(address));
    }
    if (count > maxRecords[size_t(d)]) {
      fail("v2 B-tree node at " + hex(address) + ": too many records");
    }
    const uint64_t records = address + 6;
    if (d == 0) {
      for (uint64_t i = 0; i < count; ++i) {
        out.push_back(b.span(records + i * recordSize, recordSize));
      }
      return;
    }
    uint64_t pointer = records + count * recordSize;
    for (uint64_t i = 0; i <= count; ++i) {
      const uint64_t child = b.offset(pointer);
      const uint64_t childCount = b.u(pointer + O, countSize);
      pointer += O + uint64_t(countSize) + (d > 1 ? uint64_t(cumulativeSize[size_t(d - 1)]) : 0);
      self(self, child, childCount, d - 1);
      if (i < count) {
        out.push_back(b.span(records + i * recordSize, recordSize));
      }
    }
  };
  node(node, root, rootCount, depth);
  if (out.size() != total) {
    fail("v2 B-tree at " + hex(a) + ": " + std::to_string(out.size()) + " records, header says " +
         std::to_string(total));
  }
  return out;
}

// IV.A.2.i Data layout message (version 3; version 4 only for compact / contiguous).
void parseLayout(const Bytes& b, uint64_t p, Variable& v) {
  const uint8_t version = b.u8(p), cls = b.u8(p + 1);
  if (version < 3 || version > 4) {
    v.layoutProblem = "data layout message version " + std::to_string(version);
    return;
  }
  if (cls == 0) {
    const uint16_t size = b.u16(p + 2);
    const auto data = b.span(p + 4, size);
    v.compactData.assign(data.begin(), data.end());
    v.layout = Variable::Layout::Compact;
  } else if (cls == 1) {
    v.address = b.offset(p + 2);
    v.storageSize = b.length(p + 2 + uint64_t(b.O()));
    v.layout = Variable::Layout::Contiguous;
  } else if (cls == 2 && version == 3) {
    const int dimensionality = b.u8(p + 2); // rank + 1 (the last one is the element size)
    v.address = b.offset(p + 3);
    v.chunk.clear();
    for (int i = 0; i + 1 < dimensionality; ++i) {
      v.chunk.push_back(b.u32(p + 3 + uint64_t(b.O()) + 4 * uint64_t(i)));
    }
    v.layout = Variable::Layout::Chunked;
  } else {
    v.layoutProblem = cls == 2 ? "chunked layout version 4 (newer chunk indexes)"
                               : "layout class " + std::to_string(cls);
  }
}

// IV.A.2.l Filter pipeline message: filter ids in pipeline order.
std::vector<int> parseFilters(const Bytes& b, uint64_t p) {
  const uint8_t version = b.u8(p), count = b.u8(p + 1);
  if (version != 1 && version != 2) {
    fail("filter pipeline message version " + std::to_string(version));
  }
  uint64_t q = version == 1 ? p + 8 : p + 2;
  std::vector<int> ids;
  for (int i = 0; i < count; ++i) {
    const uint16_t id = b.u16(q);
    q += 2;
    uint16_t nameLength = 0;
    if (version == 1 || id >= 256) {
      nameLength = b.u16(q);
      q += 2;
    }
    q += 2; // flags
    const uint16_t values = b.u16(q);
    q += 2;
    q += version == 1 ? pad8(nameLength) : nameLength;
    q += 4 * uint64_t(values);
    if (version == 1 && values % 2) {
      q += 4;
    }
    ids.push_back(id);
  }
  return ids;
}

// IV.A.2.f Fill value message (versions 1-3): the value, empty when undefined.
std::vector<std::byte> parseFillValue(const Bytes& b, uint64_t p) {
  const uint8_t version = b.u8(p);
  uint64_t q = 0;
  if (version == 1) {
    q = p + 4;
  } else if (version == 2) {
    if (b.u8(p + 3) == 0) {
      return {};
    }
    q = p + 4;
  } else if (version == 3) {
    if (!(b.u8(p + 1) & 0x20)) {
      return {};
    }
    q = p + 2;
  } else {
    fail("fill value message version " + std::to_string(version));
  }
  const uint32_t size = b.u32(q);
  const auto value = b.span(q + 4, size);
  return std::vector<std::byte>(value.begin(), value.end());
}

std::string filterName(int id) {
  switch (id) {
  case 1:
    return "deflate";
  case 2:
    return "shuffle";
  case 3:
    return "fletcher32";
  case 4:
    return "szip";
  default:
    return "filter " + std::to_string(id);
  }
}

void inflate(std::vector<std::byte>& data, size_t expected) {
  std::vector<std::byte> out(expected);
  size_t outLength = expected;
  const int32_t result = zng_uncompress(reinterpret_cast<uint8_t*>(out.data()), &outLength,
                                        reinterpret_cast<const uint8_t*>(data.data()), data.size());
  if (result != Z_OK || outLength != expected) {
    fail("deflate: zlib error " + std::to_string(result) + " (" + std::to_string(outLength) + " of " +
         std::to_string(expected) + " bytes)");
  }
  data.swap(out);
}

// The shuffle filter stores byte k of every element together; undo it.
void unshuffle(std::vector<std::byte>& data, size_t elementSize) {
  if (elementSize <= 1) {
    return;
  }
  const size_t n = data.size() / elementSize;
  std::vector<std::byte> out(data.size());
  for (size_t k = 0; k < elementSize; ++k) {
    const std::byte* src = data.data() + k * n;
    for (size_t i = 0; i < n; ++i) {
      out[i * elementSize + k] = src[i];
    }
  }
  std::copy(data.begin() + std::ptrdiff_t(n * elementSize), data.end(), out.begin() + std::ptrdiff_t(n * elementSize));
  data.swap(out);
}

// Copies the part of a block (dimensions blockDims at blockStart, row-major) that
// lies in the window [start, start + count) to `out` (the window, row-major).
void copyBlock(const std::byte* block, std::span<const uint64_t> blockStart, std::span<const uint64_t> blockDims,
               std::span<const uint64_t> start, std::span<const uint64_t> count, size_t elementSize, std::byte* out) {
  const size_t rank = blockDims.size();
  if (rank == 0) {
    std::memcpy(out, block, elementSize);
    return;
  }
  std::vector<uint64_t> lo(rank), hi(rank), blockStride(rank, 1), outStride(rank, 1);
  for (size_t d = rank; d-- > 0;) {
    lo[d] = std::max(blockStart[d], start[d]);
    hi[d] = std::min(blockStart[d] + blockDims[d], start[d] + count[d]);
    if (lo[d] >= hi[d]) {
      return;
    }
    if (d + 1 < rank) {
      blockStride[d] = blockStride[d + 1] * blockDims[d + 1];
      outStride[d] = outStride[d + 1] * count[d + 1];
    }
  }
  const size_t run = size_t(hi[rank - 1] - lo[rank - 1]) * elementSize;
  std::vector<uint64_t> index(lo.begin(), lo.end());
  for (;;) {
    uint64_t blockOffset = 0, outOffset = 0;
    for (size_t d = 0; d < rank; ++d) {
      blockOffset += (index[d] - blockStart[d]) * blockStride[d];
      outOffset += (index[d] - start[d]) * outStride[d];
    }
    std::memcpy(out + outOffset * elementSize, block + blockOffset * elementSize, run);
    // next row: increment the index over all but the last dimension
    size_t d = rank - 1;
    while (d > 0) {
      --d;
      if (++index[d] < hi[d]) {
        break;
      }
      index[d] = lo[d];
      if (d == 0) {
        return;
      }
    }
    if (rank == 1) {
      return;
    }
  }
}

// One element of little-endian data as a double.
double element(const std::byte* p, const Datatype& t, bool asUnsigned) {
  if (t.cls == Datatype::Class::Float) {
    if (t.size == 4) {
      float f;
      std::memcpy(&f, p, 4);
      return f;
    }
    double d;
    std::memcpy(&d, p, 8);
    return d;
  }
  uint64_t u = 0;
  std::memcpy(&u, p, t.size);
  if (asUnsigned) {
    return double(u);
  }
  const int shift = 64 - int(t.size) * 8;
  return double(int64_t(u << shift) >> shift);
}

std::string dimsText(const std::vector<uint64_t>& dims) {
  std::string s;
  for (const uint64_t d : dims) {
    s += (s.empty() ? "" : "x") + std::to_string(d);
  }
  return s;
}

} // namespace

// --- Datatype / Attribute / Variable -------------------------------------------

std::string Datatype::describe() const {
  std::string s;
  switch (cls) {
  case Class::Integer:
    s = (isSigned ? "i" : "u") + std::to_string(size * 8);
    break;
  case Class::Float:
    s = "f" + std::to_string(size * 8);
    break;
  case Class::String:
    return "string[" + std::to_string(size) + "]";
  case Class::Other:
    return "class " + std::to_string(hdfClass) + " (" + std::to_string(size) + " B)";
  }
  return bigEndian ? s + " BE" : s;
}

uint64_t Attribute::count() const {
  uint64_t n = 1;
  for (const uint64_t d : dims) {
    n *= d;
  }
  return n;
}

std::optional<double> Attribute::number(uint64_t i) const {
  if ((type.cls != Datatype::Class::Integer && type.cls != Datatype::Class::Float) || i >= count() ||
      (i + 1) * type.size > data.size()) {
    return std::nullopt;
  }
  std::byte value[8];
  std::memcpy(value, data.data() + i * type.size, type.size);
  if (type.bigEndian) {
    std::reverse(value, value + type.size);
  }
  return element(value, type, !type.isSigned);
}

std::string Attribute::text() const {
  if (type.cls != Datatype::Class::String) {
    return {};
  }
  const std::string s(reinterpret_cast<const char*>(data.data()), data.size());
  return s.substr(0, s.find('\0'));
}

std::string Attribute::describe() const {
  if (type.cls == Datatype::Class::String) {
    std::string t = text();
    if (t.size() > 100) {
      t = t.substr(0, 97) + "...";
    }
    return "\"" + t + "\"";
  }
  if (type.cls == Datatype::Class::Other) {
    return "<" + type.describe() + (dims.empty() ? "" : " [" + dimsText(dims) + "]") + ">";
  }
  std::string s;
  const uint64_t n = count();
  for (uint64_t i = 0; i < std::min<uint64_t>(n, 8); ++i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.9g", number(i).value_or(std::nan("")));
    s += (i ? ", " : "") + std::string(buf);
  }
  if (n > 8) {
    s += ", ... (" + std::to_string(n) + ")";
  }
  return dims.empty() ? s : "[" + s + "]";
}

uint64_t Variable::count() const {
  uint64_t n = 1;
  for (const uint64_t d : dims) {
    n *= d;
  }
  return n;
}

bool Variable::hasData() const {
  return layout == Layout::Compact ? !compactData.empty() : address != kUndefined;
}

const Attribute* Variable::attribute(std::string_view attributeName) const {
  for (const Attribute& a : attributes) {
    if (a.name == attributeName) {
      return &a;
    }
  }
  return nullptr;
}

std::string Variable::describe() const {
  std::string s = type.describe() + (dims.empty() ? " scalar" : "[" + dimsText(dims) + "]");
  switch (layout) {
  case Layout::Compact:
    s += " compact";
    break;
  case Layout::Contiguous:
    s += " contiguous";
    break;
  case Layout::Chunked:
    s += " chunked " + dimsText(chunk);
    break;
  case Layout::Unsupported:
    s += " (" + layoutProblem + ")";
    break;
  }
  std::string f;
  for (const int id : filters) {
    f += (f.empty() ? " " : "+") + filterName(id);
  }
  return s + f + (hasData() ? "" : " (no data)");
}

// --- File ----------------------------------------------------------------------

bool File::open(std::vector<std::byte> data, std::string& error) {
  _data = std::move(data);
  _global.clear();
  _variables.clear();
  try {
    static constexpr unsigned char kSignature[8] = {0x89, 'H', 'D', 'F', '\r', '\n', 0x1a, '\n'};
    if (_data.size() < 64 || std::memcmp(_data.data(), kSignature, 8) != 0) {
      fail("not an HDF5 file (no signature at offset 0)");
    }
    // II.A Superblock, versions 2 / 3 (NetCDF-4 files written by HDF5 1.10+).
    const Bytes header(_data, 8, 8);
    const int version = header.u8(8);
    if (version != 2 && version != 3) {
      fail("superblock version " + std::to_string(version) + " is not supported (only 2 / 3)");
    }
    _offsetSize = header.u8(9);
    _lengthSize = header.u8(10);
    if ((_offsetSize != 4 && _offsetSize != 8) || (_lengthSize != 4 && _lengthSize != 8)) {
      fail("unsupported offset / length sizes " + std::to_string(_offsetSize) + " / " + std::to_string(_lengthSize));
    }
    const Bytes b(_data, _offsetSize, _lengthSize);
    const uint64_t O = uint64_t(_offsetSize);
    if (b.offset(12) != 0) {
      fail("base address " + hex(b.offset(12)) + " (a user block) is not supported");
    }
    std::vector<uint64_t> visited;
    visit(b.offset(12 + 3 * O), "", visited);
    std::sort(_variables.begin(), _variables.end(),
              [](const Variable& x, const Variable& y) { return x.name < y.name; });
    return true;
  } catch (const std::exception& e) {
    error = e.what();
    _global.clear();
    _variables.clear();
    return false;
  }
}

const Variable* File::variable(std::string_view name) const {
  if (!name.empty() && name.front() == '/') {
    name.remove_prefix(1);
  }
  for (const Variable& v : _variables) {
    if (v.name == name) {
      return &v;
    }
  }
  return nullptr;
}

const Attribute* File::globalAttribute(std::string_view name) const {
  for (const Attribute& a : _global) {
    if (a.name == name) {
      return &a;
    }
  }
  return nullptr;
}

// IV.A.1.b Version-2 object header, with its continuation blocks.
std::vector<File::Message> File::objectHeader(uint64_t address) const {
  const Bytes b(_data, _offsetSize, _lengthSize);
  if (!b.signature(address, "OHDR")) {
    if (b.u8(address) == 1) {
      fail("object header at " + hex(address) + ": version 1 (the older HDF5 layout) is not supported");
    }
    fail("no object header at " + hex(address));
  }
  const uint8_t version = b.u8(address + 4), flags = b.u8(address + 5);
  if (version != 2) {
    fail("object header version " + std::to_string(version) + " at " + hex(address));
  }
  uint64_t p = address + 6;
  if (flags & 0x20) {
    p += 16; // access / modification / change / birth times
  }
  if (flags & 0x10) {
    p += 4; // attribute storage phase change values
  }
  const int sizeBytes = 1 << (flags & 3);
  std::vector<std::pair<uint64_t, uint64_t>> blocks{{p + uint64_t(sizeBytes), b.u(p, sizeBytes)}};
  const uint64_t prefix = (flags & 0x04) ? 6 : 4; // + creation order
  std::vector<Message> out;
  for (size_t k = 0; k < blocks.size(); ++k) {
    if (k > 256) {
      fail("object header at " + hex(address) + ": too many continuation blocks");
    }
    const auto [start, size] = blocks[k];
    b.check(start, size);
    uint64_t q = start;
    while (q + prefix <= start + size) {
      Message m;
      m.type = b.u8(q);
      m.size = b.u16(q + 1);
      m.flags = b.u8(q + 3);
      m.offset = q + prefix;
      if (m.offset + m.size > start + size) {
        fail("object header at " + hex(address) + ": message past its block");
      }
      if (m.type == 0x10) { // continuation: the next block (signature OCHK ... checksum)
        const uint64_t next = b.offset(m.offset), length = b.length(m.offset + uint64_t(_offsetSize));
        if (!b.signature(next, "OCHK") || length < 8) {
          fail("object header at " + hex(address) + ": bad continuation block at " + hex(next));
        }
        blocks.emplace_back(next + 4, length - 8);
      }
      out.push_back(m);
      q = m.offset + m.size;
    }
  }
  return out;
}

void File::visit(uint64_t address, const std::string& path, std::vector<uint64_t>& visited) {
  if (std::find(visited.begin(), visited.end(), address) != visited.end()) {
    return;
  }
  visited.push_back(address);
  const Bytes b(_data, _offsetSize, _lengthSize);
  const uint64_t O = uint64_t(_offsetSize);
  const std::string where = path.empty() ? "root group" : "'" + path + "'";
  const std::vector<Message> messages = objectHeader(address);

  std::vector<Attribute> attributes;
  std::vector<std::pair<std::string, uint64_t>> links;
  bool dataset = false;
  for (const Message& m : messages) {
    const bool shared = (m.flags & 0x02) != 0;
    switch (m.type) {
    case 0x01: // dataspace
    case 0x03: // datatype
    case 0x05: // fill value
    case 0x08: // data layout
    case 0x0B: // filter pipeline
    case 0x0C: // attribute
      if (shared) {
        fail(where + ": shared message (type " + std::to_string(m.type) + ") is not supported");
      }
      if (m.type == 0x08) {
        dataset = true;
      } else if (m.type == 0x0C) {
        attributes.push_back(attributeMessage(b.span(m.offset, m.size)));
      }
      break;
    case 0x02: { // link info: dense links
      const uint8_t flags = b.u8(m.offset + 1);
      const uint64_t p = m.offset + 2 + ((flags & 1) ? 8 : 0);
      const uint64_t heap = b.offset(p), nameIndex = b.offset(p + O);
      if (heap != kUndefined) {
        for (auto& link : denseLinks(heap, nameIndex)) {
          links.push_back(std::move(link));
        }
      }
      break;
    }
    case 0x06: // link (compact storage)
      links.push_back(parseLink(b, m.offset));
      break;
    case 0x11:
      fail(where + ": old-style group (symbol table) is not supported");
    case 0x15: { // attribute info: dense attributes
      const uint8_t flags = b.u8(m.offset + 1);
      const uint64_t p = m.offset + 2 + ((flags & 1) ? 2 : 0);
      const uint64_t heap = b.offset(p), nameIndex = b.offset(p + O);
      if (heap != kUndefined) {
        for (Attribute& a : denseAttributes(heap, nameIndex)) {
          attributes.push_back(std::move(a));
        }
      }
      break;
    }
    default:
      break;
    }
  }

  if (dataset) {
    Variable v;
    v.name = path;
    v.attributes = std::move(attributes);
    std::vector<std::byte> oldFill;
    std::optional<std::vector<std::byte>> fill;
    for (const Message& m : messages) {
      switch (m.type) {
      case 0x01:
        v.dims = parseDataspace(b, m.offset);
        break;
      case 0x03:
        v.type = parseDatatype(b, m.offset);
        break;
      case 0x04: { // fill value (old)
        const auto value = b.span(m.offset + 4, b.u32(m.offset));
        oldFill.assign(value.begin(), value.end());
        break;
      }
      case 0x05:
        fill = parseFillValue(b, m.offset);
        break;
      case 0x08:
        parseLayout(b, m.offset, v);
        break;
      case 0x0B:
        v.filters = parseFilters(b, m.offset);
        break;
      default:
        break;
      }
    }
    v.fillValue = fill ? *fill : oldFill;
    if (v.fillValue.size() != v.type.size) {
      v.fillValue.assign(v.type.size, std::byte{0});
    }
    _variables.push_back(std::move(v));
    return;
  }
  if (path.empty()) {
    _global = std::move(attributes);
  }
  for (const auto& [name, target] : links) {
    visit(target, path.empty() ? name : path + "/" + name, visited);
  }
}

std::vector<std::pair<std::string, uint64_t>> File::denseLinks(uint64_t heapAddress, uint64_t nameIndex) const {
  const Bytes b(_data, _offsetSize, _lengthSize);
  const Heap heap = readHeap(b, heapAddress);
  std::vector<std::pair<std::string, uint64_t>> links;
  // Type 5 records (link names): name hash (4) + heap ID.
  for (const auto& record : btree2Records(b, nameIndex, 5)) {
    if (record.size() < size_t(4 + heap.idLength)) {
      fail("link name record shorter than a heap ID");
    }
    const auto object = heapObject(b, heap, record.subspan(4, heap.idLength));
    links.push_back(parseLink(Bytes(object, _offsetSize, _lengthSize), 0));
  }
  return links;
}

std::vector<Attribute> File::denseAttributes(uint64_t heapAddress, uint64_t nameIndex) const {
  const Bytes b(_data, _offsetSize, _lengthSize);
  const Heap heap = readHeap(b, heapAddress);
  if (heap.idLength != 8) {
    fail("attribute heap with " + std::to_string(heap.idLength) + "-byte IDs (expected 8)");
  }
  std::vector<Attribute> attributes;
  // Type 8 records (attribute names): heap ID (8), message flags (1), creation order (4), hash (4).
  for (const auto& record : btree2Records(b, nameIndex, 8)) {
    if (record.size() < 9) {
      fail("attribute name record too short");
    }
    if (uint8_t(record[8]) & 0x01) {
      fail("shared attributes are not supported");
    }
    attributes.push_back(attributeMessage(heapObject(b, heap, record.subspan(0, 8))));
  }
  return attributes;
}

// IV.A.2.m Attribute message (versions 1-3).
Attribute File::attributeMessage(std::span<const std::byte> message) const {
  const Bytes b(message, _offsetSize, _lengthSize);
  const uint8_t version = b.u8(0);
  const uint16_t nameSize = b.u16(2), typeSize = b.u16(4), spaceSize = b.u16(6);
  Attribute a;
  uint64_t typeAt = 0, spaceAt = 0, dataAt = 0;
  if (version == 1) {
    a.name = b.name(8, nameSize);
    typeAt = 8 + pad8(nameSize);
    spaceAt = typeAt + pad8(typeSize);
    dataAt = spaceAt + pad8(spaceSize);
  } else if (version == 2 || version == 3) {
    if (b.u8(1) & 0x03) {
      fail("attribute with a shared datatype / dataspace is not supported");
    }
    const uint64_t nameAt = version == 3 ? 9 : 8; // version 3: + name character set
    a.name = b.name(nameAt, nameSize);
    typeAt = nameAt + nameSize;
    spaceAt = typeAt + typeSize;
    dataAt = spaceAt + spaceSize;
  } else {
    fail("attribute message version " + std::to_string(version));
  }
  a.type = parseDatatype(b, typeAt);
  a.dims = parseDataspace(b, spaceAt);
  const auto data = b.span(dataAt, a.count() * a.type.size);
  a.data.assign(data.begin(), data.end());
  return a;
}

// III.A.1 Version-1 B-tree of raw data chunks: every chunk (offset, size, filter mask, address).
void File::chunks(const Variable& v, uint64_t node, std::vector<Chunk>& out, int depth) const {
  const Bytes b(_data, _offsetSize, _lengthSize);
  const uint64_t O = uint64_t(_offsetSize);
  if (depth > 32) {
    fail("chunk B-tree nested too deep");
  }
  if (!b.signature(node, "TREE")) {
    fail("no chunk B-tree node at " + hex(node));
  }
  const uint8_t type = b.u8(node + 4), level = b.u8(node + 5);
  const uint16_t entries = b.u16(node + 6);
  if (type != 1) {
    fail("B-tree node at " + hex(node) + " is not a chunk index (type " + std::to_string(type) + ")");
  }
  const size_t rank = v.chunk.size();
  const uint64_t keySize = 8 + 8 * uint64_t(rank + 1); // size, filter mask, offsets (+ the element-size dimension)
  uint64_t p = node + 8 + 2 * O;                        // after the sibling addresses
  for (uint16_t e = 0; e < entries; ++e) {
    const uint64_t child = b.offset(p + keySize);
    if (level == 0) {
      Chunk c;
      c.size = b.u32(p);
      c.filterMask = b.u32(p + 4);
      for (size_t d = 0; d < rank; ++d) {
        c.offset.push_back(b.u(p + 8 + 8 * d, 8));
      }
      c.address = child;
      out.push_back(std::move(c));
    } else {
      chunks(v, child, out, depth + 1);
    }
    p += keySize + O;
  }
}

bool File::read(const Variable& v, std::span<const uint64_t> start, std::span<const uint64_t> count,
                std::vector<std::byte>& out, std::string& error) const {
  try {
    const Bytes b(_data, _offsetSize, _lengthSize);
    const size_t rank = v.dims.size();
    std::vector<uint64_t> s(rank, 0), c(v.dims);
    if (!start.empty() || !count.empty()) {
      if (start.size() != rank || count.size() != rank) {
        fail("hyperslab of rank " + std::to_string(start.size()) + " for a variable of rank " + std::to_string(rank));
      }
      s.assign(start.begin(), start.end());
      c.assign(count.begin(), count.end());
    }
    const size_t elementSize = v.type.size;
    uint64_t n = 1;
    for (size_t d = 0; d < rank; ++d) {
      if (s[d] > v.dims[d] || c[d] > v.dims[d] - s[d]) {
        fail("hyperslab past the dimensions [" + dimsText(v.dims) + "]");
      }
      n *= c[d];
    }
    if (elementSize == 0 || n > kMaxReadBytes / elementSize) {
      fail("element size " + std::to_string(elementSize) + " x " + std::to_string(n) + " elements");
    }
    out.resize(size_t(n) * elementSize);
    for (uint64_t i = 0; i < n; ++i) {
      std::memcpy(out.data() + i * elementSize, v.fillValue.data(), elementSize);
    }
    const std::vector<uint64_t> origin(rank, 0);
    const uint64_t totalBytes = v.count() * elementSize;
    if (n > 0) {
      switch (v.layout) {
      case Variable::Layout::Compact:
        if (v.compactData.size() < totalBytes) {
          fail("compact data shorter than the variable");
        }
        copyBlock(v.compactData.data(), origin, v.dims, s, c, elementSize, out.data());
        break;
      case Variable::Layout::Contiguous:
        if (v.address != kUndefined) {
          if (v.storageSize < totalBytes) {
            fail("contiguous storage shorter than the variable");
          }
          copyBlock(b.span(v.address, totalBytes).data(), origin, v.dims, s, c, elementSize, out.data());
        }
        break;
      case Variable::Layout::Chunked: {
        if (v.address == kUndefined) {
          break; // no chunk written: all fill
        }
        if (v.chunk.size() != rank) {
          fail("chunk rank " + std::to_string(v.chunk.size()) + " for a variable of rank " + std::to_string(rank));
        }
        uint64_t chunkElements = 1;
        for (const uint64_t d : v.chunk) {
          chunkElements *= d;
        }
        if (chunkElements == 0 || chunkElements > kMaxReadBytes / elementSize) {
          fail("chunk of " + dimsText(v.chunk) + " elements");
        }
        const size_t chunkBytes = size_t(chunkElements) * elementSize;
        std::vector<Chunk> list;
        chunks(v, v.address, list, 0);
        for (const Chunk& chunk : list) {
          bool inside = true;
          for (size_t d = 0; d < rank && inside; ++d) {
            inside = chunk.offset[d] < s[d] + c[d] && chunk.offset[d] + v.chunk[d] > s[d];
          }
          if (!inside) {
            continue;
          }
          const auto stored = b.span(chunk.address, chunk.size);
          std::vector<std::byte> data(stored.begin(), stored.end());
          for (size_t i = v.filters.size(); i-- > 0;) { // undo the pipeline in reverse
            if (i < 32 && (chunk.filterMask >> i) & 1) {
              continue; // this filter was skipped for this chunk
            }
            if (v.filters[i] == 1) {
              inflate(data, chunkBytes);
            } else if (v.filters[i] == 2) {
              unshuffle(data, elementSize);
            } else {
              fail(filterName(v.filters[i]) + " filter is not supported");
            }
          }
          if (data.size() != chunkBytes) {
            fail("chunk at " + hex(chunk.address) + " holds " + std::to_string(data.size()) + " bytes, expected " +
                 std::to_string(chunkBytes));
          }
          copyBlock(data.data(), chunk.offset, v.chunk, s, c, elementSize, out.data());
        }
        break;
      }
      case Variable::Layout::Unsupported:
        fail(v.layoutProblem.empty() ? "no data layout" : v.layoutProblem + " is not supported");
      }
    }
    if (v.type.bigEndian && elementSize > 1) {
      for (size_t i = 0; i + elementSize <= out.size(); i += elementSize) {
        std::reverse(out.begin() + std::ptrdiff_t(i), out.begin() + std::ptrdiff_t(i + elementSize));
      }
    }
    return true;
  } catch (const std::exception& e) {
    error = v.name + ": " + e.what();
    return false;
  }
}

template <typename T>
bool File::readValuesT(const Variable& v, std::span<const uint64_t> start, std::span<const uint64_t> count,
                       std::vector<T>& out, std::string& error) const {
  if (v.type.cls != Datatype::Class::Integer && v.type.cls != Datatype::Class::Float) {
    error = v.name + ": not numeric (" + v.type.describe() + ")";
    return false;
  }
  std::vector<std::byte> raw;
  if (!read(v, start, count, raw, error)) {
    return false;
  }
  // NetCDF-3 style _Unsigned = "true" on a signed type (newer files use unsigned types).
  const Attribute* unsignedFlag = v.attribute("_Unsigned");
  const bool asUnsigned =
      v.type.cls == Datatype::Class::Integer && (!v.type.isSigned || (unsignedFlag && unsignedFlag->text() == "true"));
  const double wrap = v.type.size < 8 ? double(uint64_t(1) << (8 * v.type.size)) : 0.0;
  // A packed attribute (fill value, valid range) read with the variable's signedness.
  auto packed = [&](const Attribute* a, uint64_t i) -> std::optional<double> {
    std::optional<double> x = a ? a->number(i) : std::nullopt;
    if (x && asUnsigned && *x < 0.0 && a->type.cls == Datatype::Class::Integer) {
      *x += wrap;
    }
    return x;
  };
  auto number = [&](const char* name, double fallback) {
    const Attribute* a = v.attribute(name);
    return a ? a->number().value_or(fallback) : fallback;
  };
  const double scale = number("scale_factor", 1.0), offset = number("add_offset", 0.0);
  const std::optional<double> fill = packed(v.attribute("_FillValue"), 0);
  // valid_range (or valid_min / valid_max): packed units when the attribute has
  // the variable's type class (CF), else unpacked.
  double lo = -std::numeric_limits<double>::infinity(), hi = std::numeric_limits<double>::infinity();
  bool rangePacked = true;
  const Attribute* range = v.attribute("valid_range");
  const Attribute* minimum = range ? range : v.attribute("valid_min");
  const Attribute* maximum = range ? range : v.attribute("valid_max");
  if (minimum) {
    lo = packed(minimum, 0).value_or(lo);
    rangePacked = minimum->type.cls == v.type.cls;
  }
  if (maximum) {
    hi = packed(maximum, range ? 1 : 0).value_or(hi);
    rangePacked = maximum->type.cls == v.type.cls;
  }
  const size_t elementSize = v.type.size, n = raw.size() / elementSize;
  out.resize(n);
  const T nan = std::numeric_limits<T>::quiet_NaN();
  for (size_t i = 0; i < n; ++i) {
    const double r = element(raw.data() + i * elementSize, v.type, asUnsigned);
    if ((fill && r == *fill) || std::isnan(r) || (rangePacked && (r < lo || r > hi))) {
      out[i] = nan;
      continue;
    }
    const double x = r * scale + offset;
    out[i] = !rangePacked && (x < lo || x > hi) ? nan : T(x);
  }
  return true;
}

bool File::readValues(const Variable& v, std::span<const uint64_t> start, std::span<const uint64_t> count,
                      std::vector<float>& out, std::string& error) const {
  return readValuesT(v, start, count, out, error);
}

bool File::readValues(const Variable& v, std::span<const uint64_t> start, std::span<const uint64_t> count,
                      std::vector<double>& out, std::string& error) const {
  return readValuesT(v, start, count, out, error);
}

} // namespace Hdf5
