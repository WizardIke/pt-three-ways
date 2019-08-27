#include "Scene.h"
#include "math/Epsilon.h"
#include "math/OrthoNormalBasis.h"
#include <math/Samples.h>

using dod::Scene;

std::optional<dod::IntersectionRecord>
Scene::intersectSpheres(const Ray &ray, double nearerThan) const {
  double currentNearestDist = nearerThan;
  std::optional<size_t> nearestIndex;
  for (size_t sphereIndex = 0; sphereIndex < spheres_.size(); ++sphereIndex) {
    // Solve t^2*d.d + 2*t*(o-p).d + (o-p).(o-p)-R^2 = 0
    auto op = spheres_[sphereIndex].centre - ray.origin();
    auto b = op.dot(ray.direction());
    auto determinant =
        b * b - op.lengthSquared() + spheres_[sphereIndex].radiusSquared;
    if (determinant < 0)
      continue;

    determinant = sqrt(determinant);
    auto minusT = b - determinant;
    auto plusT = b + determinant;
    if (minusT < Epsilon && plusT < Epsilon)
      continue;

    auto t = minusT > Epsilon ? minusT : plusT;
    if (t < currentNearestDist) {
      nearestIndex = sphereIndex;
      currentNearestDist = t;
    }
  }
  if (!nearestIndex)
    return {};

  auto hitPosition = ray.positionAlong(currentNearestDist);
  auto normal = (hitPosition - spheres_[*nearestIndex].centre).normalised();
  bool inside = normal.dot(ray.direction()) > 0;
  if (inside)
    normal = -normal;
  return IntersectionRecord{
      Hit{currentNearestDist, inside, hitPosition, normal},
      sphereMaterials_[*nearestIndex]};
}

std::optional<dod::IntersectionRecord>
Scene::intersectTriangles(const Ray &ray, double nearerThan) const {
  double currentNearestDist = nearerThan;
  struct Nearest {
    size_t index;
    bool backfacing;
    double u;
    double v;
  };

  std::optional<Nearest> nearest;
  for (size_t i = 0; i < triangleVerts_.size(); ++i) {
    const auto &tv = triangleVerts_[i];
    auto pVec = ray.direction().cross(tv.vVector());
    auto det = tv.uVector().dot(pVec);
    // ray and triangle are parallel if det is close to 0
    if (fabs(det) < Epsilon)
      continue;

    auto invDet = 1.0 / det;
    auto tVec = ray.origin() - tv.vertex(0);
    auto u = tVec.dot(pVec) * invDet;
    if (u < 0.0 || u > 1.0)
      continue;

    auto qVec = tVec.cross(tv.uVector());
    auto v = ray.direction().dot(qVec) * invDet;
    if (v < 0 || u + v > 1)
      continue;

    auto t = tv.vVector().dot(qVec) * invDet;

    if (t > Epsilon && t < currentNearestDist) {
      nearest = Nearest{i, det < Epsilon, u, v};
      currentNearestDist = t;
    }
  }
  if (!nearest)
    return {};
  auto &tn = triangleNormals_[nearest->index];
  auto normalUdelta = tn[1] - tn[0];
  auto normalVdelta = tn[2] - tn[0];
  // TODO: proper barycentric coordinates
  auto normal =
      ((nearest->u * normalUdelta) + (nearest->v * normalVdelta) + tn[0])
          .normalised();
  if (nearest->backfacing)
    normal = -normal;
  return IntersectionRecord{Hit{currentNearestDist, nearest->backfacing,
                                ray.positionAlong(currentNearestDist), normal},
                            triangleMaterials_[nearest->index]};
}

std::optional<dod::IntersectionRecord> Scene::intersect(const Ray &ray) const {
  auto sphereRec =
      intersectSpheres(ray, std::numeric_limits<double>::infinity());
  auto triangleRec = intersectTriangles(
      ray, sphereRec ? sphereRec->hit.distance
                     : std::numeric_limits<double>::infinity());
  return triangleRec ? triangleRec : sphereRec;
}

Vec3 Scene::radiance(std::mt19937 &rng, const Ray &ray, int depth,
                     const RenderParams &renderParams) const {
  int numUSamples = depth == 0 ? renderParams.firstBounceUSamples : 1;
  int numVSamples = depth == 0 ? renderParams.firstBounceVSamples : 1;
  if (depth >= renderParams.maxDepth)
    return Vec3();

  const auto intersectionRecord = intersect(ray);
  if (!intersectionRecord)
    return environment_;

  const auto &mat = intersectionRecord->material;
  const auto &hit = intersectionRecord->hit;
  if (renderParams.preview)
    return mat.diffuse;

  const auto [iorFrom, iorTo] =
      hit.inside ? std::make_pair(mat.indexOfRefraction, 1.0)
                 : std::make_pair(1.0, mat.indexOfRefraction);
  const auto reflectivity =
      mat.reflectivity < 0
          ? hit.normal.reflectance(ray.direction(), iorFrom, iorTo)
          : mat.reflectivity;

  // Sample evenly with random offset.
  std::uniform_real_distribution<> unit(0, 1.0);
  // Create a coordinate system local to the point, where the z is the
  // normal at this point.
  const auto basis = OrthoNormalBasis::fromZ(hit.normal);
  Vec3 result;

  for (auto uSample = 0; uSample < numUSamples; ++uSample) {
    for (auto vSample = 0; vSample < numVSamples; ++vSample) {
      const auto u = (static_cast<double>(uSample) + unit(rng))
                     / static_cast<double>(numUSamples);
      const auto v = (static_cast<double>(vSample) + unit(rng))
                     / static_cast<double>(numVSamples);
      const auto p = unit(rng);

      if (p < reflectivity) {
        auto newRay =
            Ray(hit.position, coneSample(hit.normal.reflect(ray.direction()),
                                         mat.reflectionConeAngleRadians, u, v));

        result += mat.emission + radiance(rng, newRay, depth + 1, renderParams);
      } else {
        auto newRay = Ray(hit.position, hemisphereSample(basis, u, v));

        result +=
            mat.emission
            + mat.diffuse * radiance(rng, newRay, depth + 1, renderParams);
      }
    }
  }
  return result / (numUSamples * numVSamples);
}

void Scene::addTriangle(const Vec3 &v0, const Vec3 &v1, const Vec3 &v2,
                        const Material &material) {
  auto &tv = triangleVerts_.emplace_back(TriangleVertices{v0, v1, v2});
  triangleNormals_.emplace_back(
      TriangleNormals{tv.faceNormal(), tv.faceNormal(), tv.faceNormal()});
  triangleMaterials_.emplace_back(material);
}

void Scene::addSphere(const Vec3 &centre, double radius,
                      const Material &material) {
  spheres_.emplace_back(centre, radius);
  sphereMaterials_.emplace_back(material);
}

void Scene::setEnvironmentColour(const Vec3 &colour) { environment_ = colour; }

ArrayOutput
Scene::render(const Camera &camera, const RenderParams &renderParams,
              const std::function<void(ArrayOutput &output)> &updateFunc) {
  auto width = renderParams.width;
  auto height = renderParams.height;
  ArrayOutput output(width, height);
  std::mt19937 rng(renderParams.seed);

  // TODO no raw loops...maybe return whole "Samples" of an entire screen and
  // accumulate separately? then feeds into a nice multithreaded future based
  // thing?
  // TODO: multi cpus
  for (int sample = 0; sample < renderParams.samplesPerPixel; ++sample) {
    for (auto y = 0; y < height; ++y) {
      for (auto x = 0; x < width; ++x) {
        auto ray = camera.randomRay(x, y, rng);
        output.addSamples(x, y, radiance(rng, ray, 0, renderParams), 1);
      }
    }
    updateFunc(output);
  }
  return output;
}
