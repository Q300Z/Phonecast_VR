#include "phonecast/core/tracking/PlanarPnpSolver.h"
#include "phonecast/core/tracking/EigenInterop.h"

#include <Eigen/Core>
#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <Eigen/SVD>

#include <algorithm>
#include <cmath>

namespace phonecast::core::tracking {

namespace {

void NormalizePoints2D(
    const std::array<Eigen::Vector2d, 4>& pts,
    std::array<Eigen::Vector2d, 4>& outPts,
    Eigen::Matrix3d& outT,
    Eigen::Matrix3d& outTinv) {
    Eigen::Vector2d centroid(0.0, 0.0);
    for (const auto& p : pts) {
        centroid += p;
    }
    centroid /= 4.0;

    double meanDist = 0.0;
    for (const auto& p : pts) {
        meanDist += (p - centroid).norm();
    }
    meanDist /= 4.0;
    if (meanDist < 1e-8) {
        meanDist = 1e-8;
    }

    const double scale = std::sqrt(2.0) / meanDist;

    outT << scale, 0.0, -scale * centroid.x(),
            0.0, scale, -scale * centroid.y(),
            0.0, 0.0, 1.0;

    outTinv << 1.0 / scale, 0.0, centroid.x(),
               0.0, 1.0 / scale, centroid.y(),
               0.0, 0.0, 1.0;

    for (size_t i = 0; i < 4; ++i) {
        outPts[i] = (pts[i] - centroid) * scale;
    }
}

void RunLhmIteration(
    const std::array<Eigen::Vector3d, 4>& modelPts,
    const std::array<Eigen::Vector3d, 4>& rays,
    Eigen::Matrix3d& R,
    Eigen::Vector3d& t,
    uint32_t maxIters,
    double tol) {
    std::array<Eigen::Matrix3d, 4> G_i;
    Eigen::Matrix3d G = Eigen::Matrix3d::Zero();

    for (size_t i = 0; i < 4; ++i) {
        const Eigen::Vector3d& v = rays[i];
        G_i[i] = Eigen::Matrix3d::Identity() - v * v.transpose();
        G += G_i[i];
    }

    if (std::abs(G.determinant()) < 1e-9) {
        return;
    }
    const Eigen::Matrix3d G_inv = G.inverse();

    auto computeT = [&](const Eigen::Matrix3d& rot) -> Eigen::Vector3d {
        Eigen::Vector3d sumGRp = Eigen::Vector3d::Zero();
        for (size_t i = 0; i < 4; ++i) {
            sumGRp += G_i[i] * rot * modelPts[i];
        }
        return -G_inv * sumGRp;
    };

    t = computeT(R);

    for (uint32_t iter = 0; iter < maxIters; ++iter) {
        std::array<Eigen::Vector3d, 4> p_prime;
        Eigen::Vector3d p_prime_bar = Eigen::Vector3d::Zero();

        for (size_t i = 0; i < 4; ++i) {
            const Eigen::Vector3d q_i = R * modelPts[i] + t;
            p_prime[i] = rays[i] * (rays[i].dot(q_i));
            p_prime_bar += p_prime[i];
        }
        p_prime_bar /= 4.0;

        // Model centroid is 0 by construction
        Eigen::Matrix3d M = Eigen::Matrix3d::Zero();
        for (size_t i = 0; i < 4; ++i) {
            M += (p_prime[i] - p_prime_bar) * modelPts[i].transpose();
        }

        Eigen::JacobiSVD<Eigen::Matrix3d> svd(M, Eigen::ComputeFullU | Eigen::ComputeFullV);
        const Eigen::Matrix3d U = svd.matrixU();
        const Eigen::Matrix3d V = svd.matrixV();

        Eigen::Matrix3d S = Eigen::Matrix3d::Identity();
        if ((U * V.transpose()).determinant() < 0.0) {
            S(2, 2) = -1.0;
        }

        const Eigen::Matrix3d R_new = U * S * V.transpose();
        const Eigen::Vector3d t_new = computeT(R_new);

        const double deltaR = (R_new - R).norm();
        const double deltaT = (t_new - t).norm();

        R = R_new;
        t = t_new;

        if (deltaR < tol && deltaT < tol) {
            break;
        }
    }
}

float ComputeReprojectionRmse(
    const std::array<Eigen::Vector3d, 4>& modelPts,
    const std::array<Vector2, 4>& measuredCorners,
    const Eigen::Matrix3d& R,
    const Eigen::Vector3d& t,
    const CameraIntrinsics& k) {
    double sumSqErr = 0.0;
    for (size_t i = 0; i < 4; ++i) {
        const Eigen::Vector3d camP = R * modelPts[i] + t;
        if (camP.z() <= 0.01) {
            return 1e6F; // Behind camera
        }
        const double u = (camP.x() / camP.z()) * static_cast<double>(k.fx) + static_cast<double>(k.cx);
        const double v = (camP.y() / camP.z()) * static_cast<double>(k.fy) + static_cast<double>(k.cy);

        const double du = u - static_cast<double>(measuredCorners[i].x);
        const double dv = v - static_cast<double>(measuredCorners[i].y);
        sumSqErr += (du * du + dv * dv);
    }
    return static_cast<float>(std::sqrt(sumSqErr / 4.0));
}

} // namespace

PlanarPnpSolver::PlanarPnpSolver() noexcept : params_() {}

PlanarPnpSolver::PlanarPnpSolver(Params params) noexcept : params_(params) {}


PlanarPnpSolver::Result PlanarPnpSolver::Solve(
    const ScreenCorners2D& corners,
    const ScreenMetrics& metrics,
    const CameraIntrinsics& intrinsics) const {
    Result res{};
    if (!corners.valid || !metrics.IsValid() || !intrinsics.IsValid()) {
        return res;
    }
    for (size_t i = 0; i < 4; ++i) {
        if (!std::isfinite(corners.corners[i].x) || !std::isfinite(corners.corners[i].y)) {
            return res;
        }
    }

    const double w_m = static_cast<double>(metrics.WidthMeters());
    const double h_m = static_cast<double>(metrics.HeightMeters());

    // 1. Model coordinates in object space (origin at display center)
    // Clockwise order: TL, TR, BR, BL
    const std::array<Eigen::Vector3d, 4> modelPts = {
        Eigen::Vector3d(-w_m * 0.5,  h_m * 0.5, 0.0), // Top-Left
        Eigen::Vector3d( w_m * 0.5,  h_m * 0.5, 0.0), // Top-Right
        Eigen::Vector3d( w_m * 0.5, -h_m * 0.5, 0.0), // Bottom-Right
        Eigen::Vector3d(-w_m * 0.5, -h_m * 0.5, 0.0)  // Bottom-Left
    };

    // 2. Normalized image coordinates
    std::array<Eigen::Vector2d, 4> normImagePts;
    std::array<Eigen::Vector3d, 4> rayVectors;
    for (size_t i = 0; i < 4; ++i) {
        const double nx = (static_cast<double>(corners.corners[i].x) - static_cast<double>(intrinsics.cx)) /
                          static_cast<double>(intrinsics.fx);
        const double ny = (static_cast<double>(corners.corners[i].y) - static_cast<double>(intrinsics.cy)) /
                          static_cast<double>(intrinsics.fy);
        normImagePts[i] = Eigen::Vector2d(nx, ny);
        rayVectors[i] = Eigen::Vector3d(nx, ny, 1.0).normalized();
    }

    // 3. Hartley isotropic normalization
    const std::array<Eigen::Vector2d, 4> model2D = {
        Eigen::Vector2d(modelPts[0].x(), modelPts[0].y()),
        Eigen::Vector2d(modelPts[1].x(), modelPts[1].y()),
        Eigen::Vector2d(modelPts[2].x(), modelPts[2].y()),
        Eigen::Vector2d(modelPts[3].x(), modelPts[3].y())
    };

    std::array<Eigen::Vector2d, 4> normModelPts;
    Eigen::Matrix3d T_p;
    Eigen::Matrix3d T_p_inv;
    NormalizePoints2D(model2D, normModelPts, T_p, T_p_inv);

    std::array<Eigen::Vector2d, 4> normImgPts;
    Eigen::Matrix3d T_m;
    Eigen::Matrix3d T_m_inv;
    NormalizePoints2D(normImagePts, normImgPts, T_m, T_m_inv);

    // 4. Construct 8x9 DLT matrix
    Eigen::Matrix<double, 8, 9> A;
    for (size_t i = 0; i < 4; ++i) {
        const double X = normModelPts[i].x();
        const double Y = normModelPts[i].y();
        const double u = normImgPts[i].x();
        const double v = normImgPts[i].y();

        A.row(2 * i)     << -X, -Y, -1.0, 0.0, 0.0, 0.0, u * X, u * Y, u;
        A.row(2 * i + 1) << 0.0, 0.0, 0.0, -X, -Y, -1.0, v * X, v * Y, v;
    }

    Eigen::JacobiSVD<Eigen::Matrix<double, 8, 9>> svdA(A, Eigen::ComputeFullV);
    const Eigen::VectorXd h = svdA.matrixV().col(8);

    Eigen::Matrix3d H_norm;
    H_norm << h(0), h(1), h(2),
              h(3), h(4), h(5),
              h(6), h(7), h(8);

    const Eigen::Matrix3d H_calib = T_m_inv * H_norm * T_p;

    // 5. Initial Pose extraction
    const Eigen::Vector3d h1 = H_calib.col(0);
    const Eigen::Vector3d h2 = H_calib.col(1);
    const Eigen::Vector3d h3 = H_calib.col(2);

    const double denom = h1.norm() + h2.norm();
    if (!std::isfinite(denom) || denom < 1e-9) {
        return res;
    }
    const double lambda = 2.0 / denom;
    Eigen::Vector3d r1 = lambda * h1;
    Eigen::Vector3d r2 = lambda * h2;
    Eigen::Vector3d t1 = lambda * h3;

    // Cheirality: target must be in front of camera
    if (t1.z() < 0.0) {
        r1 = -r1;
        r2 = -r2;
        t1 = -t1;
    }

    const Eigen::Vector3d r3 = r1.cross(r2);
    Eigen::Matrix3d R_approx;
    R_approx << r1, r2, r3;

    Eigen::JacobiSVD<Eigen::Matrix3d> svdR(R_approx, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Eigen::Matrix3d S = Eigen::Matrix3d::Identity();
    if ((svdR.matrixU() * svdR.matrixV().transpose()).determinant() < 0.0) {
        S(2, 2) = -1.0;
    }
    Eigen::Matrix3d R1 = svdR.matrixU() * S * svdR.matrixV().transpose();

    // 6. Refine Primary Pose via LHM
    RunLhmIteration(modelPts, rayVectors, R1, t1, params_.maxLhmIterations, static_cast<double>(params_.lhmTolerance));

    // 7. Generate Planar Dual Solution via Analytical Normal Reflection
    const Eigen::Vector3d n1 = R1.col(2);
    const Eigen::Vector3d v_c = t1.normalized();
    const Eigen::Vector3d n2 = -n1 + 2.0 * (n1.dot(v_c)) * v_c;

    Eigen::Vector3d rotAxis = n1.cross(v_c);
    Eigen::Matrix3d R2 = R1;
    Eigen::Vector3d t2 = t1;

    if (rotAxis.norm() > 1e-5) {
        rotAxis.normalize();
        const double angle = std::acos(std::clamp(n1.dot(n2), -1.0, 1.0));
        const Eigen::AngleAxisd refl(angle, rotAxis);
        R2 = refl.toRotationMatrix() * R1;

        // Refine Secondary Pose via LHM
        RunLhmIteration(modelPts, rayVectors, R2, t2, params_.maxLhmIterations, static_cast<double>(params_.lhmTolerance));
    }

    // 8. Error Evaluation
    const float rmse1 = ComputeReprojectionRmse(modelPts, corners.corners, R1, t1, intrinsics);
    const float rmse2 = ComputeReprojectionRmse(modelPts, corners.corners, R2, t2, intrinsics);

    auto toPose6D = [](const Eigen::Matrix3d& rot, const Eigen::Vector3d& trans, float conf) -> Pose6D {
        const Eigen::Quaterniond q(rot);
        Pose6D p{};
        p.position = Vector3{static_cast<float>(trans.x()), static_cast<float>(trans.y()), static_cast<float>(trans.z())};
        p.orientation = Quaternion{static_cast<float>(q.w()), static_cast<float>(q.x()),
                                   static_cast<float>(q.y()), static_cast<float>(q.z())};
        p.valid = true;
        p.confidence = conf;
        p.state = TrackingState::Tracking;
        return p;
    };

    res.primaryPose = toPose6D(R1, t1, 1.0F / (1.0F + rmse1));
    res.secondaryPose = toPose6D(R2, t2, 1.0F / (1.0F + rmse2));
    res.primaryReprojectionRmsePixels = rmse1;
    res.secondaryReprojectionRmsePixels = rmse2;
    res.success = (t1.z() >= static_cast<double>(params_.minPlaneDistanceMeters) &&
                   t1.z() <= static_cast<double>(params_.maxPlaneDistanceMeters));

    return res;
}

Pose6D PlanarPnpSolver::Disambiguate(
    const Result& result,
    const Quaternion& imuOrientation,
    const Vector3& cameraGravity) noexcept {
    if (!result.success || !result.primaryPose.valid) {
        return result.primaryPose;
    }
    if (!result.secondaryPose.valid) {
        return result.primaryPose;
    }

    // If secondary pose is clearly an algebraic artifact with much worse reprojection, keep primary
    if (result.secondaryReprojectionRmsePixels > 2.5F * result.primaryReprojectionRmsePixels + 2.0F) {
        return result.primaryPose;
    }

    // Gravity vector in phone body frame:
    // Earth gravity is [0, 0, -1]. imuOrientation maps body -> Earth.
    // Therefore body gravity is g_B = imuOrientation* * [0, 0, -1] * imuOrientation
    const Vector3 g_B = imuOrientation.Conjugate().Rotate(Vector3{0.0F, 0.0F, -1.0F});

    // Transform predicted gravity into camera frame for each candidate
    const Vector3 g_cam1 = result.primaryPose.orientation.Rotate(g_B);
    const Vector3 g_cam2 = result.secondaryPose.orientation.Rotate(g_B);

    const Vector3 g_cam_ref = cameraGravity.Normalized();
    const float score1 = g_cam1.Dot(g_cam_ref);
    const float score2 = g_cam2.Dot(g_cam_ref);

    // If one candidate is significantly better aligned with gravity, choose it
    if (score2 > score1 + 0.05F) {
        return result.secondaryPose;
    }
    if (score1 > score2 + 0.05F) {
        return result.primaryPose;
    }

    // Otherwise, tie-break on reprojection RMSE
    if (result.secondaryReprojectionRmsePixels + 0.1F < result.primaryReprojectionRmsePixels) {
        return result.secondaryPose;
    }
    return result.primaryPose;
}

} // namespace phonecast::core::tracking
