#include "QuinticPolynomial.h"

#include <Eigen/LU>
#include <cmath>

using namespace Eigen;

QuinticPolynomial::QuinticPolynomial(double xs, double vxs, double axs,
        double xe, double vxe, double axe, double t):
        a0(xs), a1(vxs) {
    a2 = axs / 2.0;
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double t4 = t2 * t2;
    const double t5 = t4 * t;
    Matrix3d A;
    Vector3d B;
    A << t3, t4, t5, 3 * t2,
    4 * t3, 5 * t4, 6 * t, 12 * t2,
    20 * t3;
    B << xe - a0 - a1 * t - a2 * t2, vxe - a1 - 2 * a2 * t,
    axe - 2 * a2;
    Matrix3d A_inv = A.inverse();
    Vector3d x = A_inv * B;
    a3 = x[0];
    a4 = x[1];
    a5 = x[2];
}

double QuinticPolynomial::calc_point(double t) {
    return ((((a5 * t + a4) * t + a3) * t + a2) * t + a1) * t + a0;
}

double QuinticPolynomial::calc_first_derivative(double t) {
    return (((5 * a5 * t + 4 * a4) * t + 3 * a3) * t + 2 * a2) * t + a1;
}

double QuinticPolynomial::calc_second_derivative(double t) {
    return ((20 * a5 * t + 12 * a4) * t + 6 * a3) * t + 2 * a2;
}

double QuinticPolynomial::calc_third_derivative(double t) {
    return (60 * a5 * t + 24 * a4) * t + 6 * a3;
}