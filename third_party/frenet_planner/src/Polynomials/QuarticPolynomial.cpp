#include "QuarticPolynomial.h"

#include <Eigen/LU>
#include <cmath>

using namespace Eigen;

QuarticPolynomial::QuarticPolynomial(double xs, double vxs, double axs,
        double vxe, double axe, double t):
        a0(xs), a1(vxs) {
    a2 = axs / 2.0;
    const double t2 = t * t;
    const double t3 = t2 * t;
    Matrix2d A;
    Vector2d B;
    A << 3 * t2, 4 * t3, 6 * t, 12 * t2;
    B << vxe - a1 - 2 * a2 * t, axe - 2 * a2;
    Matrix2d A_inv = A.inverse();
    Vector2d x = A_inv * B;
    a3 = x[0];
    a4 = x[1];
}

double QuarticPolynomial::calc_point(double t) {
    return (((a4 * t + a3) * t + a2) * t + a1) * t + a0;
}

double QuarticPolynomial::calc_first_derivative(double t) {
    return ((4 * a4 * t + 3 * a3) * t + 2 * a2) * t + a1;
}

double QuarticPolynomial::calc_second_derivative(double t) {
    return (12 * a4 * t + 6 * a3) * t + 2 * a2;
}

double QuarticPolynomial::calc_third_derivative(double t) {
    return 6 * a3 + 24 * a4 * t;
}