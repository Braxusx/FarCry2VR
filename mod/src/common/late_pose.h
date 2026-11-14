#pragma once
#include "pose_frame.h"
#include <array>
#include <cmath>
#include <limits>

namespace fc2vr::presentation {
using Matrix=std::array<float,16>; // row vectors; translation in indices 12..14
using Packed=std::array<float,12>; // three rows of transpose(Matrix), exactly 48 bytes
using Vec3=fc2vr::tracking::Vec3;
using GameBasis=fc2vr::tracking::GameBasis;
inline Matrix Identity() { return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; }

namespace detail {
using Wide=std::array<double,16>;
inline bool Finite(const Matrix& m) { for(float v:m) if(!std::isfinite(v)) return false;return true; }
inline bool Finite(Vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
inline Wide Widen(const Matrix& m) { Wide result{};for(unsigned i=0;i<16;++i) result[i]=m[i];return result; }
inline bool Narrow(const Wide& m,Matrix& output) {
    Matrix result{};
    for(unsigned i=0;i<16;++i) {
        if(!std::isfinite(m[i]) || std::fabs(m[i])>(std::numeric_limits<float>::max)()) return false;
        result[i]=static_cast<float>(m[i]);
    }
    output=result;return true;
}
inline Matrix Invalid() { Matrix m{};m.fill(std::numeric_limits<float>::quiet_NaN());return m; }
inline Wide Product(const Wide& a,const Wide& b) {
    Wide result{};
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c)
        for(unsigned k=0;k<4;++k) result[r*4+c]+=a[r*4+k]*b[k*4+c];
    return result;
}
inline bool Affine(const Matrix& m) {
    return Finite(m) && std::fabs(m[3])<1e-6f && std::fabs(m[7])<1e-6f
        && std::fabs(m[11])<1e-6f && std::fabs(m[15]-1)<1e-6f;
}
inline bool Invert(const Wide& input,Wide& output) {
    double augmented[4][8]{};
    double scale[4]{};
    for(unsigned r=0;r<4;++r) {
        for(unsigned c=0;c<4;++c) {
            const double value=input[r*4+c];if(!std::isfinite(value)) return false;
            augmented[r][c]=value;
            if(std::fabs(value)>scale[r]) scale[r]=std::fabs(value);
        }
        if(scale[r]==0) return false;
        augmented[r][r+4]=1;
    }
    for(unsigned c=0;c<4;++c) {
        unsigned pivot=c;
        for(unsigned r=c+1;r<4;++r)
            if(std::fabs(augmented[r][c])/scale[r]>std::fabs(augmented[pivot][c])/scale[pivot]) pivot=r;
        if(std::fabs(augmented[pivot][c])<=scale[pivot]*1e-12) return false;
        if(pivot!=c) {
            for(unsigned k=0;k<8;++k) { const double value=augmented[pivot][k];augmented[pivot][k]=augmented[c][k];augmented[c][k]=value; }
            const double value=scale[pivot];scale[pivot]=scale[c];scale[c]=value;
        }
        const double divisor=augmented[c][c];
        for(unsigned k=0;k<8;++k) augmented[c][k]/=divisor;
        for(unsigned r=0;r<4;++r) if(r!=c) {
            const double factor=augmented[r][c];
            for(unsigned k=0;k<8;++k) augmented[r][k]-=factor*augmented[c][k];
        }
    }
    Wide result{};
    for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c) {
        const double value=augmented[r][c+4];if(!std::isfinite(value)) return false;result[r*4+c]=value;
    }
    output=result;return true;
}
inline bool Rigid(const Matrix& m) {
    if(!Affine(m)) return false;
    for(unsigned r=0;r<3;++r) for(unsigned c=r;c<3;++c) {
        double dot=0;for(unsigned k=0;k<3;++k) dot+=double(m[r*4+k])*m[c*4+k];
        if(std::fabs(dot-(r==c?1.0:0.0))>0.001) return false;
    }
    const double determinant=double(m[0])*(double(m[5])*m[10]-double(m[6])*m[9])
        -double(m[1])*(double(m[4])*m[10]-double(m[6])*m[8])
        +double(m[2])*(double(m[4])*m[9]-double(m[5])*m[8]);
    return determinant>0.999 && determinant<1.001;
}
} // namespace detail

inline Matrix Multiply(const Matrix& a,const Matrix& b) {
    Matrix output;
    return detail::Narrow(detail::Product(detail::Widen(a),detail::Widen(b)),output) ? output : detail::Invalid();
}
// General 4x4 inverse, including affine transforms. Failure leaves output alone.
inline bool Inverse(const Matrix& input,Matrix& output) {
    detail::Wide inverse{};
    return detail::Invert(detail::Widen(input),inverse) && detail::Narrow(inverse,output);
}

// Reference-space axes/position are already scaled to game units by the caller.
// Construct in root-relative world coordinates before narrowing: adding a small
// grip motion to a kilometre-scale world position first would discard precision.
inline Matrix Controller(const GameBasis& game,Vec3 right,Vec3 up,Vec3 back,Vec3 grip,Vec3 rootOrigin) {
    if(!detail::Finite(game.R)||!detail::Finite(game.U)||!detail::Finite(game.F)||!detail::Finite(game.P)
        ||!detail::Finite(right)||!detail::Finite(up)||!detail::Finite(back)||!detail::Finite(grip)
        ||!detail::Finite(rootOrigin)) return detail::Invalid();
    const float basis[3][3]={{game.R.x,game.R.y,game.R.z},{game.U.x,game.U.y,game.U.z},{-game.F.x,-game.F.y,-game.F.z}};
    const Vec3 rows[4]={right,up,back,grip};
    detail::Wide result{};
    for(unsigned row=0;row<4;++row) {
        const double local[3]={rows[row].x,rows[row].y,rows[row].z};
        for(unsigned column=0;column<3;++column)
            for(unsigned k=0;k<3;++k) result[row*4+column]+=local[k]*basis[k][column];
    }
    result[12]+=double(game.P.x)-rootOrigin.x;
    result[13]+=double(game.P.y)-rootOrigin.y;
    result[14]+=double(game.P.z)-rootOrigin.z;result[15]=1;
    Matrix output;
    return detail::Narrow(result,output) ? output : detail::Invalid();
}

// The two controller transforms MUST use the same root-relative world origin.
// Bounds apply to controller grip displacement (<=0.5 game metres) and angular
// correction (<=60 degrees), before conversion through root rotation/scale.
// Root translation must have been removed; root scale is preserved.
// Failure leaves output untouched, so the caller can retain the native copy.
inline bool Delta(const Matrix& oldController,const Matrix& newController,const Matrix& rootRotation,Matrix& output) {
    if(!detail::Rigid(oldController)||!detail::Rigid(newController)||!detail::Affine(rootRotation)
        ||std::fabs(rootRotation[12])>1e-6f||std::fabs(rootRotation[13])>1e-6f||std::fabs(rootRotation[14])>1e-6f) return false;
    double distance2=0;
    for(unsigned i=12;i<15;++i) { const double difference=double(newController[i])-oldController[i];distance2+=difference*difference; }
    if(distance2>0.25) return false;
    detail::Wide inverseOld{},inverseRoot{};
    if(!detail::Invert(detail::Widen(oldController),inverseOld)
        ||!detail::Invert(detail::Widen(rootRotation),inverseRoot)) return false;
    const auto world=detail::Product(inverseOld,detail::Widen(newController));
    const double cosine=(world[0]+world[5]+world[10]-1)*.5;
    if(!std::isfinite(cosine)||cosine<0.5-1e-7||cosine>1.001) return false;
    auto model=detail::Product(detail::Product(detail::Widen(rootRotation),world),inverseRoot);
    model[3]=model[7]=model[11]=0;model[15]=1;
    return detail::Narrow(model,output);
}

inline Matrix Unpack(const Packed& packed) {
    Matrix output=Identity();
    for(unsigned column=0;column<3;++column) for(unsigned row=0;row<4;++row)
        output[row*4+column]=packed[column*4+row];
    return output;
}
inline bool Pack(const Matrix& matrix,Packed& output) {
    if(!detail::Affine(matrix)) return false;
    Packed result{};
    for(unsigned column=0;column<3;++column) for(unsigned row=0;row<4;++row)
        result[column*4+row]=matrix[row*4+column];
    output=result;return true;
}
// Native skin matrices are inverseBind*boneModel, so right multiplication
// transports the bone while retaining its inverse bind. Exactly twelve floats
// are written; the native routine's incidental 16-byte overrun is not repeated.
inline bool ApplyPacked(const Packed& source,const Matrix& modelDelta,Packed& output) {
    if(!detail::Affine(modelDelta)) return false;
    const auto unpacked=Unpack(source);
    if(!detail::Finite(unpacked)) return false;
    const auto wide=detail::Product(detail::Widen(unpacked),detail::Widen(modelDelta));
    Matrix transformed;
    return detail::Narrow(wide,transformed) && Pack(transformed,output);
}
} // namespace fc2vr::presentation
