#pragma once

#include <Types.h>

// Absolute value for integers
static inline INT Abs(INT N) {
    return (N < 0) ? -N : N;
}

static inline LONG LAbs(LONG N) {
    return (N < 0) ? -N : N;
}

static inline INT64 LLAbs(INT64 N) {
    return (N < 0) ? -N : N;
}

// Absolute value for floating point numbers
static inline FLOAT FAbsF(FLOAT X) {
    union {
        FLOAT F;
        UINT32 I;
    } U = {X};
    U.I &= 0x7fffffff;
    return U.F;
}

static inline DOUBLE FAbs(DOUBLE X) {
    union {
        DOUBLE D;
        UINT64 I;
    } U = {X};
    U.I &= 0x7fffffffffffffffULL;
    return U.D;
}

// Limit value
#define CLAMP(X, Min, Max) ((X) < (Min) ? (Min) : ((X) > (Max) ? (Max) : (X)))

// Number sign
static inline INT Sign(INT X) {
    return (X > 0) - (X < 0);
}

static inline INT SignF(FLOAT X) {
    return (X > 0.0f) - (X < 0.0f);
}


// Square root (sqrt)
// Fast inverse square root (for float)
static inline FLOAT InvSqrtF(FLOAT X) {
    FLOAT XHalf = 0.5f * X;
    INT I = *(INT*)&X;
    I = 0x5f3759df - (I >> 1);
    X = *(FLOAT*)&I;
    X = X * (1.5f - XHalf * X * X);
    return X;
}

// Square root for float (Newton's method)
static inline FLOAT SqrtF(FLOAT X) {
    if (X <= 0.0f) return 0.0f;
    
    FLOAT Guess = X;
    FLOAT Better = (Guess + X / Guess) * 0.5f;
    
    //Iterations until convergence
    for (INT I = 0; I < 5; I++) {
        Better = (Guess + X / Guess) * 0.5f;
        if (Better > Guess - 0.0001f && Better < Guess + 0.0001f) break;
        Guess = Better;
    }
    
    return Better;
}

// Square root for double
static inline DOUBLE Sqrt(DOUBLE X) {
    if (X <= 0.0) return 0.0;
    
    DOUBLE Guess = X;
    DOUBLE Better;
    
    for (INT I = 0; I < 8; I++) {
        Better = (Guess + X / Guess) * 0.5;
        if (Better > Guess - 0.0000001 && Better < Guess + 0.0000001) break;
        Guess = Better;
    }
    
    return Better;
}

// Square root for integers (integer)
static inline UINT32 ISqrt(UINT32 X) {
    if (X <= 1) return X;
    
    UINT32 Guess = X / 2;
    UINT32 Better;
    
    for (INT I = 0; I < 10; I++) {
        Better = (Guess + X / Guess) / 2;
        if (Better >= Guess) break;
        Guess = Better;
    }
    
    return Guess;
}

//=============================================================================
//Power functions
//=============================================================================

// Fast exponentiation (integer)
static inline INT IPow(INT Base, INT Exp) {
    INT Result = 1;
    while (Exp > 0) {
        if (Exp & 1) Result *= Base;
        Base *= Base;
        Exp >>= 1;
    }
    return Result;
}

// Degree for float
static inline FLOAT PowF(FLOAT Base, INT Exp) {
    FLOAT Result = 1.0f;
    INT Negative = (Exp < 0);
    
    if (Negative) Exp = -Exp;
    
    while (Exp > 0) {
        if (Exp & 1) Result *= Base;
        Base *= Base;
        Exp >>= 1;
    }
    
    return Negative ? 1.0f / Result : Result;
}

// Trigonometric functions (Taylor series approximation)
static inline FLOAT SinF(FLOAT X) {
    // Reducing the angle to [-π, π]
    const FLOAT Pi = 3.14159265f;
    const FLOAT TwoPi = 2.0f * Pi;
    
    while (X > Pi) X -= TwoPi;
    while (X < -Pi) X += TwoPi;
    
    // Taylor series: sin(x) = x - x^3/6 + x^5/120 - x^7/5040
    FLOAT X2 = X * X;
    FLOAT X3 = X2 * X;
    FLOAT X5 = X3 * X2;
    FLOAT X7 = X5 * X2;
    
    return X - X3 / 6.0f + X5 / 120.0f - X7 / 5040.0f;
}

static inline FLOAT CosF(FLOAT X) {
    // Reducing the angle to [-π, π]
    const FLOAT Pi = 3.14159265f;
    const FLOAT TwoPi = 2.0f * Pi;
    
    while (X > Pi) X -= TwoPi;
    while (X < -Pi) X += TwoPi;
    
    // Taylor series: cos(x) = 1 - x^2/2 + x^4/24 - x^6/720
    FLOAT X2 = X * X;
    FLOAT X4 = X2 * X2;
    FLOAT X6 = X4 * X2;
    
    return 1.0f - X2 / 2.0f + X4 / 24.0f - X6 / 720.0f;
}

static inline FLOAT TanF(FLOAT X) {
    return SinF(X) / CosF(X);
}

//Exponent and logarithms
static inline FLOAT ExpF(FLOAT X) {
    FLOAT Result = 1.0f;
    FLOAT Term = 1.0f;
    
    // Taylor series: e^x = 1 + x + x^2/2! + x^3/3! +...
    for (INT I = 1; I < 10; I++) {
        Term *= X / I;
        Result += Term;
    }
    
    return Result;
}

static inline FLOAT LogF(FLOAT X) {
    if (X <= 0.0f) return -1.0f;
    
    // Natural logarithm via approximation
    UINT32 Bits = *(UINT32*)&X;
    INT Exponent = ((Bits >> 23) & 0xFF) - 127;
    FLOAT Mantissa = 1.0f + ((Bits & 0x7FFFFF) / (FLOAT)(1 << 23));
    
    // log(x) = log(1.m * 2^e) = log(1.m) + e * log(2)
    const FLOAT Log2 = 0.69314718f;
    
    // Approximation log(1.m)
    FLOAT M1 = Mantissa - 1.0f;
    FLOAT LogM = M1 - M1*M1/2 + M1*M1*M1/3 - M1*M1*M1*M1/4;
    
    return LogM + Exponent * Log2;
}

static FLOAT ATan2F(FLOAT Y, FLOAT X) {
    if (X == 0.0f) {
        if (Y > 0) return 1.57079633f;
        if (Y < 0) return -1.57079633f;
        return 0.0f;
    }
    
    FLOAT AbsY = Y < 0 ? -Y : Y;
    FLOAT Angle;
    
    if (AbsY < X) {
        Angle = AbsY / X;
        Angle = Angle * (Angle * (Angle * (Angle * 0.013480470f + 0.057477314f) + 0.121239071f) + 0.195635944f) + 0.166667327f;
        Angle = Angle * (1.0f / X);
    } else {
        Angle = X / AbsY;
        Angle = 1.57079633f - (Angle * (Angle * (Angle * (Angle * 0.013480470f + 0.057477314f) + 0.121239071f) + 0.195635944f) + 0.166667327f);
    }
    
    if (X < 0) Angle = 3.14159265f - Angle;
    if (Y < 0) Angle = -Angle;
    
    return Angle;
}

static FLOAT ACosF(FLOAT X) {
    if (X >= 1.0f) return 0.0f;
    if (X <= -1.0f) return 3.14159265f;
    
    FLOAT AbsX = X < 0 ? -X : X;
    FLOAT Angle = (1.0f - AbsX) * (AbsX * (AbsX * (AbsX * (AbsX * 0.100005697f + 0.126314997f) + 0.272230074f) + 0.477236359f) + 0.647689137f) + 1.57079633f;
    
    if (X < 0) Angle = 3.14159265f - Angle;
    return Angle;
}
