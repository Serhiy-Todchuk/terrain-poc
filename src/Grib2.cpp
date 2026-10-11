#include "pch.h"
#include "Grib2.h"

#include <cmath>
#include <limits>

namespace Grib2 {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

// Big-endian readers. GRIB2 signed integers use sign-and-magnitude.
uint32_t u8(const uint8_t* p) { return p[0]; }
uint32_t u16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }
uint32_t u32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint64_t u64(const uint8_t* p) { return (uint64_t(u32(p)) << 32) | u32(p + 4); }
int32_t s16(const uint8_t* p) {
  const uint32_t v = u16(p);
  return (v & 0x8000u) ? -int32_t(v & 0x7fffu) : int32_t(v);
}
int32_t s32(const uint8_t* p) {
  const uint32_t v = u32(p);
  return (v & 0x80000000u) ? -int32_t(v & 0x7fffffffu) : int32_t(v);
}
int32_t s8(const uint8_t* p) { return (p[0] & 0x80u) ? -int32_t(p[0] & 0x7fu) : int32_t(p[0]); }
float f32(const uint8_t* p) {
  const uint32_t v = u32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// Lambert conformal conic on a sphere (Snyder, "Map Projections", eq. 15-1..15-4);
// y is measured from the pole, the origin term cancels in grid offsets.
struct Lambert {
  double n = 0.0, f = 0.0, r = 0.0, lov = 0.0;
  explicit Lambert(const LambertGrid& g) {
    const double p1 = g.latin1 * kDeg, p2 = g.latin2 * kDeg;
    n = std::abs(g.latin1 - g.latin2) < 1e-9
            ? std::sin(p1)
            : std::log(std::cos(p1) / std::cos(p2)) /
                  std::log(std::tan(kPi / 4 + p2 / 2) / std::tan(kPi / 4 + p1 / 2));
    f = std::cos(p1) * std::pow(std::tan(kPi / 4 + p1 / 2), n) / n;
    r = g.earthRadius;
    lov = g.lov;
  }
  void xy(double lonDeg, double latDeg, double& x, double& y) const {
    const double rho = r * f / std::pow(std::tan(kPi / 4 + latDeg * kDeg / 2), n);
    const double theta = n * std::remainder(lonDeg - lov, 360.0) * kDeg;
    x = rho * std::sin(theta);
    y = -rho * std::cos(theta);
  }
};

int64_t unixSeconds(int year, int month, int day, int hour, int minute, int second) {
  using namespace std::chrono;
  const sys_days d = year_month_day{std::chrono::year{year}, std::chrono::month{unsigned(month)},
                                    std::chrono::day{unsigned(day)}};
  return int64_t(d.time_since_epoch().count()) * 86400 + hour * 3600 + minute * 60 + second;
}

int forecastToMinutes(int unit, int64_t value) {
  switch (unit) {
  case 0: return int(value);             // minute
  case 1: return int(value * 60);        // hour
  case 2: return int(value * 1440);      // day
  case 10: return int(value * 180);      // 3 hours
  case 11: return int(value * 360);      // 6 hours
  case 12: return int(value * 720);      // 12 hours
  case 13: return int(value / 60);       // second
  default: return -1;
  }
}

// State carried across the (repeatable) sections 3..7 of one message.
struct MessageState {
  int discipline = 0;
  int64_t referenceTime = 0;
  bool haveGrid = false, haveProduct = false, haveRepresentation = false;
  LambertGrid grid;
  int numPoints = 0;
  int category = 0, parameter = 0, levelType = 0, forecastMinutes = 0;
  double levelValue = 0.0;
  // data representation 5.0
  int numPacked = 0;
  float reference = 0.0f;
  int binaryScale = 0, decimalScale = 0, bits = 0;
  // bitmap (section 6)
  std::vector<uint8_t> bitmap; // empty: all points present
};

bool parseGrid(const uint8_t* s, uint32_t len, MessageState& m, std::string& error) {
  if (len < 14) {
    error = "section 3 too short";
    return false;
  }
  const uint32_t templ = u16(s + 12);
  if (templ != 30) {
    error = "unsupported grid definition template 3." + std::to_string(templ) + " (only 3.30 Lambert conformal)";
    return false;
  }
  if (len < 81) {
    error = "section 3 (template 3.30) too short";
    return false;
  }
  LambertGrid g;
  const uint32_t shape = u8(s + 14);
  switch (shape) {
  case 0: g.earthRadius = 6367470.0; break;
  case 1: g.earthRadius = double(u32(s + 16)) / std::pow(10.0, s8(s + 15)); break;
  case 6: g.earthRadius = 6371229.0; break;
  case 8: g.earthRadius = 6371200.0; break;
  default:
    error = "unsupported shape of the earth " + std::to_string(shape) + " (spherical shapes 0/1/6/8 only)";
    return false;
  }
  g.nx = int(u32(s + 30));
  g.ny = int(u32(s + 34));
  g.la1 = s32(s + 38) * 1e-6;
  g.lo1 = s32(s + 42) * 1e-6;
  g.uvGridRelative = (u8(s + 46) & 0x08u) != 0;
  g.lov = s32(s + 51) * 1e-6;
  g.dx = u32(s + 55) * 1e-3;
  g.dy = u32(s + 59) * 1e-3;
  const uint32_t centre = u8(s + 63);
  g.scanMode = uint8_t(u8(s + 64));
  g.latin1 = s32(s + 65) * 1e-6;
  g.latin2 = s32(s + 69) * 1e-6;
  if (centre & 0x80u) {
    error = "south-pole Lambert projection is not supported";
    return false;
  }
  if (g.scanMode & 0x10u) {
    error = "boustrophedon scanning (scan mode bit 4) is not supported";
    return false;
  }
  if (g.nx <= 1 || g.ny <= 1 || int64_t(g.nx) * g.ny != int64_t(u32(s + 6)) || g.dx <= 0.0 || g.dy <= 0.0) {
    error = "inconsistent Lambert grid " + std::to_string(g.nx) + "x" + std::to_string(g.ny);
    return false;
  }
  m.grid = g;
  m.numPoints = g.nx * g.ny;
  m.haveGrid = true;
  return true;
}

bool parseProduct(const uint8_t* s, uint32_t len, MessageState& m, std::string& error) {
  if (len < 34) {
    error = "section 4 too short";
    return false;
  }
  const uint32_t templ = u16(s + 7);
  if (templ != 0 && templ != 8) {
    error = "unsupported product definition template 4." + std::to_string(templ);
    return false;
  }
  m.category = int(u8(s + 9));
  m.parameter = int(u8(s + 10));
  m.forecastMinutes = forecastToMinutes(int(u8(s + 17)), u32(s + 18));
  m.levelType = int(u8(s + 22));
  const uint32_t scaled = u32(s + 24);
  m.levelValue = scaled == 0xffffffffu ? 0.0 : double(s32(s + 24)) / std::pow(10.0, s8(s + 23));
  m.haveProduct = true;
  return true;
}

bool parseRepresentation(const uint8_t* s, uint32_t len, MessageState& m, std::string& error) {
  if (len < 11) {
    error = "section 5 too short";
    return false;
  }
  const uint32_t templ = u16(s + 9);
  if (templ != 0) {
    error = "unsupported data representation template 5." + std::to_string(templ) + " (only 5.0 simple packing)";
    return false;
  }
  if (len < 21) {
    error = "section 5 (template 5.0) too short";
    return false;
  }
  m.numPacked = int(u32(s + 5));
  m.reference = f32(s + 11);
  m.binaryScale = s16(s + 15);
  m.decimalScale = s16(s + 17);
  m.bits = int(u8(s + 19));
  if (m.bits > 32) {
    error = "simple packing with " + std::to_string(m.bits) + " bits per value";
    return false;
  }
  m.haveRepresentation = true;
  return true;
}

bool unpack(const uint8_t* s, uint32_t len, const MessageState& m, Field& field, std::string& error) {
  const int n = m.numPoints;
  const size_t available = size_t(len - 5) * 8;
  if (m.bits > 0 && available < size_t(m.numPacked) * size_t(m.bits)) {
    error = "section 7 holds fewer bits than " + std::to_string(m.numPacked) + " packed values";
    return false;
  }
  const double scaleE = std::ldexp(1.0, m.binaryScale);
  const double scaleD = std::pow(10.0, -m.decimalScale);
  const uint8_t* data = s + 5;

  // Values in storage order (missing points = NaN).
  std::vector<float> stored(size_t(n), std::numeric_limits<float>::quiet_NaN());
  int packedIndex = 0;
  size_t bitPos = 0;
  for (int k = 0; k < n; ++k) {
    if (!m.bitmap.empty() && !((m.bitmap[size_t(k) >> 3] >> (7 - (k & 7))) & 1u)) {
      continue;
    }
    if (packedIndex >= m.numPacked) {
      error = "bitmap marks more points than packed values";
      return false;
    }
    uint64_t x = 0;
    for (int b = 0; b < m.bits; ++b, ++bitPos) {
      x = (x << 1) | ((data[bitPos >> 3] >> (7 - (bitPos & 7))) & 1u);
    }
    stored[size_t(k)] = float((double(m.reference) + double(x) * scaleE) * scaleD);
    ++packedIndex;
  }
  if (packedIndex != m.numPacked) {
    error = "decoded " + std::to_string(packedIndex) + " values, section 5 announced " + std::to_string(m.numPacked);
    return false;
  }

  // Normalise to i east (fastest), j north.
  const LambertGrid& g = m.grid;
  const bool iWest = (g.scanMode & 0x80u) != 0, jNorth = (g.scanMode & 0x40u) != 0,
             jFast = (g.scanMode & 0x20u) != 0;
  field.values.assign(size_t(n), 0.0f);
  for (int k = 0; k < n; ++k) {
    const int is = jFast ? k / g.ny : k % g.nx;
    const int js = jFast ? k % g.ny : k / g.nx;
    const int i = iWest ? g.nx - 1 - is : is;
    const int j = jNorth ? js : g.ny - 1 - js;
    field.values[size_t(j) * size_t(g.nx) + size_t(i)] = stored[size_t(k)];
  }
  return true;
}

} // namespace

void LambertGrid::project(double lonDeg, double latDeg, double& i, double& j) const {
  const Lambert lcc(*this);
  double x1, y1, x, y;
  lcc.xy(lo1, la1, x1, y1);
  lcc.xy(lonDeg, latDeg, x, y);
  // The first stored point sits at the west/east end and south/north row
  // depending on the scanning mode.
  const double i0 = (scanMode & 0x80u) ? nx - 1 : 0, j0 = (scanMode & 0x40u) ? 0 : ny - 1;
  i = i0 + (x - x1) / dx;
  j = j0 + (y - y1) / dy;
}

double LambertGrid::rotation(double lonDeg) const {
  // The meridian through a point is the line to the pole: in projected
  // coordinates it is turned by theta = n (lon - LoV) against the y axis.
  return Lambert(*this).n * std::remainder(lonDeg - lov, 360.0) * kDeg;
}

float Field::sample(double i, double j) const {
  const int nx = grid.nx, ny = grid.ny;
  if (!(i >= -0.5 && j >= -0.5 && i <= nx - 0.5 && j <= ny - 0.5)) {
    return std::numeric_limits<float>::quiet_NaN();
  }
  i = std::clamp(i, 0.0, double(nx - 1));
  j = std::clamp(j, 0.0, double(ny - 1));
  const int i0 = std::min(int(i), nx - 2), j0 = std::min(int(j), ny - 2);
  const double fi = i - i0, fj = j - j0;
  const double w[4] = {(1 - fi) * (1 - fj), fi * (1 - fj), (1 - fi) * fj, fi * fj};
  const float v[4] = {at(i0, j0), at(i0 + 1, j0), at(i0, j0 + 1), at(i0 + 1, j0 + 1)};
  double sum = 0.0, weight = 0.0;
  for (int k = 0; k < 4; ++k) {
    if (!std::isnan(v[k]) && w[k] > 0.0) {
      sum += w[k] * v[k];
      weight += w[k];
    }
  }
  return weight > 1e-6 ? float(sum / weight) : std::numeric_limits<float>::quiet_NaN();
}

bool decode(std::span<const std::byte> bytes, std::vector<Field>& out, std::string& error) {
  const uint8_t* d = reinterpret_cast<const uint8_t*>(bytes.data());
  const size_t size = bytes.size();
  size_t pos = 0;
  int messages = 0;
  while (pos + 16 <= size) {
    if (std::memcmp(d + pos, "GRIB", 4) != 0) {
      if (messages > 0 && std::all_of(d + pos, d + size, [](uint8_t c) { return c == 0; })) {
        break; // zero padding after the last message
      }
      error = "no GRIB message at byte " + std::to_string(pos);
      return false;
    }
    if (d[pos + 7] != 2) {
      error = "GRIB edition " + std::to_string(d[pos + 7]) + " (expected 2)";
      return false;
    }
    const uint64_t total = u64(d + pos + 8);
    if (total < 16 + 4 || pos + total > size) {
      error = "truncated GRIB message at byte " + std::to_string(pos);
      return false;
    }
    MessageState m;
    m.discipline = d[pos + 6];
    const size_t end = pos + size_t(total) - 4;
    if (std::memcmp(d + end, "7777", 4) != 0) {
      error = "GRIB message at byte " + std::to_string(pos) + " does not end with 7777";
      return false;
    }
    size_t q = pos + 16;
    while (q + 5 <= end) {
      const uint32_t len = u32(d + q);
      const uint32_t number = d[q + 4];
      if (len < 5 || q + len > end) {
        error = "bad length of section " + std::to_string(number);
        return false;
      }
      const uint8_t* s = d + q;
      switch (number) {
      case 1:
        if (len < 19) {
          error = "section 1 too short";
          return false;
        }
        m.referenceTime = unixSeconds(int(u16(s + 12)), int(s[14]), int(s[15]), int(s[16]), int(s[17]), int(s[18]));
        break;
      case 2: break; // local use
      case 3:
        if (!parseGrid(s, len, m, error)) return false;
        break;
      case 4:
        if (!parseProduct(s, len, m, error)) return false;
        break;
      case 5:
        if (!parseRepresentation(s, len, m, error)) return false;
        break;
      case 6: {
        const uint32_t indicator = len >= 6 ? s[5] : 255;
        if (indicator == 0) {
          if (size_t(len - 6) * 8 < size_t(m.numPoints)) {
            error = "bitmap shorter than the grid";
            return false;
          }
          m.bitmap.assign(s + 6, s + len);
        } else if (indicator == 255) {
          m.bitmap.clear();
        } else if (indicator != 254) { // 254: the previous bitmap of this message applies
          error = "unsupported bitmap indicator " + std::to_string(indicator);
          return false;
        }
        break;
      }
      case 7: {
        if (!m.haveGrid || !m.haveProduct || !m.haveRepresentation) {
          error = "data section before grid / product / representation sections";
          return false;
        }
        Field field;
        field.discipline = m.discipline;
        field.category = m.category;
        field.parameter = m.parameter;
        field.levelType = m.levelType;
        field.levelValue = m.levelValue;
        field.referenceTime = m.referenceTime;
        field.forecastMinutes = m.forecastMinutes;
        field.grid = m.grid;
        if (!unpack(s, len, m, field, error)) {
          return false;
        }
        out.push_back(std::move(field));
        break;
      }
      default:
        error = "unexpected section " + std::to_string(number);
        return false;
      }
      q += len;
    }
    pos += size_t(total);
    ++messages;
  }
  if (messages == 0) {
    error = "no GRIB2 message found";
    return false;
  }
  return true;
}

} // namespace Grib2
