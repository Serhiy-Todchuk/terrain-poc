#include "pch.h"
#include "GridFill.h"

#include <algorithm>

namespace GridFill {

bool fillGaps(std::vector<float>& value, const std::vector<float>& weight, int nx, int ny) {
  struct Level {
    int nx = 0, ny = 0;
    std::vector<float> v, w;
  };
  std::vector<Level> pyramid;
  pyramid.push_back({nx, ny, value, weight});
  for (float& w : pyramid[0].w) {
    w = std::clamp(w, 0.0f, 1.0f);
  }
  while (pyramid.back().nx > 1 || pyramid.back().ny > 1) {
    const Level& f = pyramid.back();
    Level c{(f.nx + 1) / 2, (f.ny + 1) / 2, {}, {}};
    c.v.assign(size_t(c.nx) * size_t(c.ny), 0.0f);
    c.w.assign(c.v.size(), 0.0f);
    for (int y = 0; y < c.ny; ++y) {
      for (int x = 0; x < c.nx; ++x) {
        float ws = 0.0f, vs = 0.0f;
        for (int dy = 0; dy < 2; ++dy) {
          for (int dx = 0; dx < 2; ++dx) {
            const int fx = 2 * x + dx, fy = 2 * y + dy;
            if (fx < f.nx && fy < f.ny) {
              const size_t k = size_t(fy) * size_t(f.nx) + size_t(fx);
              ws += f.w[k];
              vs += f.w[k] * f.v[k];
            }
          }
        }
        const size_t k = size_t(y) * size_t(c.nx) + size_t(x);
        c.w[k] = std::min(ws, 1.0f);
        c.v[k] = ws > 0.0f ? vs / ws : 0.0f;
      }
    }
    pyramid.push_back(std::move(c));
  }
  if (!(pyramid.back().w[0] > 0.0f)) {
    return false;
  }
  for (size_t l = pyramid.size() - 1; l-- > 0;) {
    Level& f = pyramid[l];
    const Level& c = pyramid[l + 1];
    for (int y = 0; y < f.ny; ++y) {
      const float cy = std::clamp((float(y) + 0.5f) * 0.5f - 0.5f, 0.0f, float(c.ny - 1));
      const int y0 = int(cy), y1 = std::min(y0 + 1, c.ny - 1);
      const float ty = cy - float(y0);
      for (int x = 0; x < f.nx; ++x) {
        const float cx = std::clamp((float(x) + 0.5f) * 0.5f - 0.5f, 0.0f, float(c.nx - 1));
        const int x0 = int(cx), x1 = std::min(x0 + 1, c.nx - 1);
        const float tx = cx - float(x0);
        auto at = [&](int xx, int yy) { return c.v[size_t(yy) * size_t(c.nx) + size_t(xx)]; };
        const float up = (at(x0, y0) * (1.0f - tx) + at(x1, y0) * tx) * (1.0f - ty) +
                         (at(x0, y1) * (1.0f - tx) + at(x1, y1) * tx) * ty;
        const size_t k = size_t(y) * size_t(f.nx) + size_t(x);
        f.v[k] = f.w[k] * f.v[k] + (1.0f - f.w[k]) * up;
        f.w[k] = 1.0f;
      }
    }
  }
  value = std::move(pyramid[0].v);
  return true;
}

void boxBlur(std::vector<float>& v, int nx, int ny, int r) {
  std::vector<float> tmp(v.size());
  const float norm = 1.0f / float(2 * r + 1);
  for (int y = 0; y < ny; ++y) {
    for (int x = 0; x < nx; ++x) {
      float s = 0.0f;
      for (int d = -r; d <= r; ++d) {
        s += v[size_t(y) * size_t(nx) + size_t(std::clamp(x + d, 0, nx - 1))];
      }
      tmp[size_t(y) * size_t(nx) + size_t(x)] = s * norm;
    }
  }
  for (int y = 0; y < ny; ++y) {
    for (int x = 0; x < nx; ++x) {
      float s = 0.0f;
      for (int d = -r; d <= r; ++d) {
        s += tmp[size_t(std::clamp(y + d, 0, ny - 1)) * size_t(nx) + size_t(x)];
      }
      v[size_t(y) * size_t(nx) + size_t(x)] = s * norm;
    }
  }
}

} // namespace GridFill
