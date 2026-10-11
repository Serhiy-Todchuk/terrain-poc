#pragma once
#include "Config.h"

#include <CesiumGeospatial/Cartographic.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <optional>

// Earth-fixed (ECEF) camera with heading/pitch relative to the local
// east-north-up frame (roll is always zero, which keeps the horizon level).
class Camera {
public:
  glm::dvec3 position{6378137.0, 0.0, 0.0};
  double heading = 0.0; // radians, 0 = north, clockwise
  double pitch = 0.0;   // radians, negative looks down
  double fovY = 1.0471975511965976;
  double aspect = 16.0 / 9.0;
  double nearPlane = 0.5;

  void apply(const CameraSpec& spec);

  glm::dmat4 enuFrame() const;  // columns: east, north, up, origin
  glm::dvec3 direction() const;
  glm::dvec3 up() const;
  glm::dvec3 right() const;
  glm::dmat4 viewMatrix() const; // ECEF -> view space (right-handed, looks down -Z)
  glm::dmat4 projectionMatrix() const; // reversed-Z, infinite far plane, D3D clip space

  // Sets heading/pitch so the camera looks along an ECEF direction.
  void lookAlong(const glm::dvec3& direction);

  CesiumGeospatial::Cartographic cartographic() const;
  double heightAboveEllipsoid() const;
  double altitudeMsl() const; // above NAVD88 / mean sea level

  // World-space ray through a pixel (origin = camera position).
  glm::dvec3 rayDirection(double px, double py, double width, double height) const;

  // "--camera" style description of the current pose.
  std::string describe() const;

  // Navigation primitives (all in ECEF).
  void orbit(const glm::dvec3& pivot, double deltaHeading, double deltaPitch);
  void moveTowards(const glm::dvec3& target, double factor, double minDistance);
  void translate(const glm::dvec3& delta);
};

// Ray / ellipsoid intersection at a given height above the WGS84 ellipsoid.
std::optional<glm::dvec3> intersectEllipsoid(const glm::dvec3& origin, const glm::dvec3& direction, double height);
