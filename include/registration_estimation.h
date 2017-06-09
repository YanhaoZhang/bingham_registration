#ifndef REGISTRATION_EST_BINGHAM_KF_RGBD
#define REGISTRATION_EST_BINGHAM_KF_RGBD

#include "type_defs.h"

struct RegistrationResult{
    VectorXld Xreg;
    MatrixXld Xregsave;
    double error;
};

RegistrationResult registration_est_kf_rgbd(PointCloud *ptcldMoving, PointCloud *ptcldFixed,
                                            double inlierRatio, int maxIterations, int windowSize,
                                            double toleranceT, double toleranceR,
                                            double uncertaintyR);

 RegistrationResult registration_est_normal(PointCloud *ptcldMoving, PointCloud *ptcldFixed,
                                            PointCloud *normalMoving, PointCloud *normalFixed,
                                            double inlierRatio, int maxIterations, int windowSize,
                                            double toleranceT, double toleranceR,
                                            double uncertaintyR);
#endif