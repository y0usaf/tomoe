#ifndef TOMOE_SPRING_H
#define TOMOE_SPRING_H
#include <float.h>
#include <math.h>

static inline double spring_curve(double stiffness, double damping_ratio,
        double from, double to, double v0, double elapsed) {
    double omega0 = sqrt(fmax(stiffness, 0));
    double beta = fmax(damping_ratio, 0) * omega0;
    double x0 = from - to;
    double envelope = exp(-beta * elapsed);
    if (fabs(beta - omega0) <= FLT_EPSILON)
        return to + envelope * (x0 + (v0 + beta * x0) * elapsed);
    if (beta < omega0) {
        double omega1 = sqrt(omega0 * omega0 - beta * beta);
        return to + envelope * (x0 * cos(omega1 * elapsed) +
            ((beta * x0 + v0) / omega1) * sin(omega1 * elapsed));
    }
    double omega2 = sqrt(beta * beta - omega0 * omega0);
    return to + envelope * (x0 * cosh(omega2 * elapsed) +
        ((beta * x0 + v0) / omega2) * sinh(omega2 * elapsed));
}

#endif
