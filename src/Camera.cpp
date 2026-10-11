#include "pch.h"
#include "Camera.h"

#include "GeoRef.h"

using namespace CesiumGeospatial;
using CesiumUtility::Math;

void Camera::apply(const CameraSpec& spec) {
  heading = Math::degreesToRadians(spec.heading);
  pitch = Math::degreesToRadians(std::clamp(spec.pitch, -89.9, 89.9));
  const double hEll = GeoRef::navd88ToEllipsoid(spec.alt, spec.lon, spec.lat);
  const glm::dvec3 point = GeoRef::toEcef(spec.lon, spec.lat, hEll);
  if (spec.mode == CameraSpec::Mode::Eye) {
    position = point;
    return;
  }
  // Look-at: place the eye "range" meters back along the view direction.
  // The direction is defined in the target's ENU frame.
  const glm::dmat4 enu = GlobeTransforms::eastNorthUpToFixedFrame(point, GeoRef::ellipsoid());
  const glm::dvec3 e(enu[0]), n(enu[1]), u(enu[2]);
  const glm::dvec3 dir = e * (std::sin(heading) * std::cos(pitch)) + n * (std::cos(heading) * std::cos(pitch)) +
                         u * std::sin(pitch);
  position = point - dir * spec.range;
  lookAlong(dir);
}

glm::dmat4 Camera::enuFrame() const {
  return GlobeTransforms::eastNorthUpToFixedFrame(position, GeoRef::ellipsoid());
}

glm::dvec3 Camera::direction() const {
  const glm::dmat4 enu = enuFrame();
  return glm::dvec3(enu[0]) * (std::sin(heading) * std::cos(pitch)) +
         glm::dvec3(enu[1]) * (std::cos(heading) * std::cos(pitch)) + glm::dvec3(enu[2]) * std::sin(pitch);
}

glm::dvec3 Camera::up() const {
  const glm::dmat4 enu = enuFrame();
  return glm::dvec3(enu[0]) * (-std::sin(heading) * std::sin(pitch)) +
         glm::dvec3(enu[1]) * (-std::cos(heading) * std::sin(pitch)) + glm::dvec3(enu[2]) * std::cos(pitch);
}

glm::dvec3 Camera::right() const { return glm::normalize(glm::cross(direction(), up())); }

glm::dmat4 Camera::viewMatrix() const { return glm::lookAt(position, position + direction(), up()); }

glm::dmat4 Camera::projectionMatrix() const {
  const double f = 1.0 / std::tan(fovY * 0.5);
  glm::dmat4 p(0.0);
  p[0][0] = f / aspect;
  p[1][1] = f;
  p[2][3] = -1.0;      // w = -z_view
  p[3][2] = nearPlane; // z = near  => depth = near / -z_view (1 at near, 0 at infinity)
  return p;
}

void Camera::lookAlong(const glm::dvec3& dirIn) {
  const glm::dvec3 dir = glm::normalize(dirIn);
  const glm::dmat4 enu = enuFrame();
  const double x = glm::dot(dir, glm::dvec3(enu[0]));
  const double y = glm::dot(dir, glm::dvec3(enu[1]));
  const double z = glm::dot(dir, glm::dvec3(enu[2]));
  pitch = std::asin(std::clamp(z, -1.0, 1.0));
  pitch = std::clamp(pitch, Math::degreesToRadians(-89.9), Math::degreesToRadians(89.9));
  if (std::abs(x) > 1e-9 || std::abs(y) > 1e-9) {
    heading = std::atan2(x, y);
  }
}

Cartographic Camera::cartographic() const {
  return GeoRef::ellipsoid().cartesianToCartographic(position).value_or(Cartographic(0, 0, 0));
}

double Camera::heightAboveEllipsoid() const { return cartographic().height; }

double Camera::altitudeMsl() const {
  const Cartographic c = cartographic();
  return c.height - GeoRef::geoidHeight(Math::radiansToDegrees(c.longitude), Math::radiansToDegrees(c.latitude));
}

glm::dvec3 Camera::rayDirection(double px, double py, double width, double height) const {
  const double ndcX = 2.0 * (px + 0.5) / width - 1.0;
  const double ndcY = 1.0 - 2.0 * (py + 0.5) / height;
  const double tanY = std::tan(fovY * 0.5);
  const double tanX = tanY * aspect;
  const glm::dvec3 d = direction();
  const glm::dvec3 u = up();
  const glm::dvec3 r = glm::normalize(glm::cross(d, u));
  return glm::normalize(d + r * (ndcX * tanX) + u * (ndcY * tanY));
}

std::string Camera::describe() const {
  const Cartographic c = cartographic();
  char buf[160];
  double h = std::fmod(Math::radiansToDegrees(heading), 360.0);
  if (h < 0) {
    h += 360.0;
  }
  snprintf(buf, sizeof(buf), "%.6f,%.6f,%.1f,%.1f,%.1f", Math::radiansToDegrees(c.longitude),
           Math::radiansToDegrees(c.latitude), altitudeMsl(), h, Math::radiansToDegrees(pitch));
  return buf;
}

void Camera::orbit(const glm::dvec3& pivot, double deltaHeading, double deltaPitch) {
  const glm::dvec3 pivotUp = GeoRef::ellipsoid().geodeticSurfaceNormal(pivot);
  glm::dvec3 offset = position - pivot;
  glm::dvec3 dir = direction();

  // Horizontal: rotate around the pivot's local vertical.
  const glm::dmat4 rh = glm::rotate(glm::dmat4(1.0), -deltaHeading, pivotUp);
  offset = glm::dvec3(rh * glm::dvec4(offset, 0.0));
  dir = glm::dvec3(rh * glm::dvec4(dir, 0.0));

  // Vertical: rotate around the camera's right axis, keeping the eye above the
  // pivot's horizon and below its zenith.
  const glm::dvec3 rightAxis = glm::normalize(glm::cross(dir, pivotUp));
  if (glm::length(rightAxis) > 0.5) {
    const glm::dmat4 rv = glm::rotate(glm::dmat4(1.0), deltaPitch, rightAxis);
    const glm::dvec3 newOffset = glm::dvec3(rv * glm::dvec4(offset, 0.0));
    const double elevation = std::asin(std::clamp(glm::dot(glm::normalize(newOffset), pivotUp), -1.0, 1.0));
    if (elevation > Math::degreesToRadians(1.0) && elevation < Math::degreesToRadians(89.5)) {
      offset = newOffset;
      dir = glm::dvec3(rv * glm::dvec4(dir, 0.0));
    }
  }
  position = pivot + offset;
  lookAlong(dir);
}

void Camera::moveTowards(const glm::dvec3& target, double factor, double minDistance) {
  const glm::dvec3 dir = direction();
  glm::dvec3 offset = position - target;
  double dist = glm::length(offset);
  double newDist = std::max(dist * factor, minDistance);
  if (dist > 1e-6) {
    position = target + offset * (newDist / dist);
  }
  lookAlong(dir);
}

void Camera::translate(const glm::dvec3& delta) {
  const glm::dvec3 dir = direction();
  // Keep heading/pitch relative to the local frame, but re-derive from the
  // previous direction so small moves do not drift the view.
  position += delta;
  lookAlong(dir);
}

std::optional<glm::dvec3> intersectEllipsoid(const glm::dvec3& origin, const glm::dvec3& direction, double height) {
  const glm::dvec3 radii = GeoRef::ellipsoid().getRadii() + glm::dvec3(height);
  const glm::dvec3 o = origin / radii;
  const glm::dvec3 d = direction / radii;
  const double a = glm::dot(d, d);
  const double b = 2.0 * glm::dot(o, d);
  const double c = glm::dot(o, o) - 1.0;
  const double disc = b * b - 4 * a * c;
  if (disc < 0.0) {
    return std::nullopt;
  }
  const double sq = std::sqrt(disc);
  double t = (-b - sq) / (2 * a);
  if (t < 0.0) {
    t = (-b + sq) / (2 * a);
  }
  if (t < 0.0) {
    return std::nullopt;
  }
  return origin + direction * t;
}
