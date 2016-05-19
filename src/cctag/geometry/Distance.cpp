#include <immintrin.h>
#include "Distance.hpp"

namespace cctag {
namespace numerical {

// Compute (point-polar) distance between a point and an ellipse represented by its 3x3 matrix.
// TODO@lilian: f is always equal to 1, remove it
// NOTE: Q IS SYMMTERIC!; Eigen stores matrices column-wise by default.
float distancePointEllipseScalar(const Eigen::Vector3f& p, const Eigen::Matrix3f& Q, const float f )
{
  using Vector6f = Eigen::Matrix<float, 6, 1>;
  Vector6f aux( 6 );

  float x = p.x();
  float y = p.y();

  aux( 0 ) = x*x;
  aux( 1 ) = 2 * x * y;
  aux( 2 ) = 2* x;
  aux( 3 ) = y * y;
  aux( 4 ) = 2* y;
  aux( 5 ) = 1;

  float tmp1  = p( 0 ) * Q( 0, 0 ) + p( 1 ) * Q( 0, 1 ) + p( 2 ) * Q( 0, 2 );
  float tmp2  = p( 0 ) * Q( 0, 1 ) + p( 1 ) * Q( 1, 1 ) + p( 2 ) * Q( 1, 2 );
  float denom = tmp1 * tmp1 + tmp2 * tmp2;

  Vector6f qL(6);
  qL( 0 ) = Q( 0, 0 ) ; qL( 1 ) = Q( 0, 1 ) ; qL( 2 ) = Q( 0, 2 ) ;
  qL( 3 ) = Q( 1, 1 ) ; qL( 4 ) = Q( 1, 2 ) ;
  qL( 5 ) = Q( 2, 2 );
  return boost::math::pow<2>( aux.dot(qL) ) / denom;
}

// Packed computation, 8 points at a time
__m256 distance_point_ellipse_avx2(const Eigen::Matrix3f& Q, __m256 x, __m256 y, __m256 w)
{
  __m256 denom, dot;
  {
    __m256 q01 = _mm256_broadcast_ss(&Q(0,1));
  
    __m256 tmp11 = _mm256_mul_ps(x, _mm256_broadcast_ss(&Q(0,0)));
    __m256 tmp12 = _mm256_mul_ps(y, q01);
    __m256 tmp13 = _mm256_mul_ps(w, _mm256_broadcast_ss(&Q(0,2)));
    __m256 tmp1  = _mm256_add_ps(tmp11, _mm256_add_ps(tmp12, tmp13));

    __m256 tmp21 = _mm256_mul_ps(x, q01);
    __m256 tmp22 = _mm256_mul_ps(y, _mm256_broadcast_ss(&Q(1,1)));
    __m256 tmp23 = _mm256_mul_ps(w, _mm256_broadcast_ss(&Q(1,2)));
    __m256 tmp2  = _mm256_add_ps(tmp21, _mm256_add_ps(tmp22, tmp23));

    tmp1 = _mm256_mul_ps(tmp1, tmp1);
    tmp2 = _mm256_mul_ps(tmp2, tmp2);
    denom = _mm256_add_ps(tmp1, tmp2);
  }
  
  // Dot product of aux with qL
  {
    __m256 twox = _mm256_mul_ps(x, _mm256_set1_ps(2.f));
    __m256 twoy = _mm256_mul_ps(x, _mm256_set1_ps(2.f));
    
    dot =  _mm256_mul_ps(                                   // dot = x*x * Q(0,0)
      _mm256_broadcast_ss(&Q(0,0)), _mm256_mul_ps(x, x));
    dot = _mm256_add_ps(dot, _mm256_mul_ps(                 // dot += 2*x*y * Q(0,1)
      _mm256_broadcast_ss(&Q(0,1)), _mm256_mul_ps(twox, y)));
    dot = _mm256_add_ps(dot, _mm256_mul_ps(                 // dot += 2*x * Q(0,2)
      _mm256_broadcast_ss(&Q(0,2)), twox));
    dot = _mm256_add_ps(dot, _mm256_mul_ps(                 // dot += y*y * Q(1,1)
      _mm256_broadcast_ss(&Q(1,1)), _mm256_mul_ps(y, y)));
    dot = _mm256_add_ps(dot, _mm256_mul_ps(                 // dot += 2*y * Q(1,2)
      _mm256_broadcast_ss(&Q(1,2)), twoy));
    dot = _mm256_add_ps(dot,                                // dot += 1 * Q(2,2)
      _mm256_broadcast_ss(&Q(2,2)));
  }
  
  return _mm256_div_ps(dot, denom);
}

// Returns mask of valid distances based on n (valid range 1..7)
std::pair<__m256, __m256> distance_point_ellipse_avx2(const Eigen::Matrix3f& Q, const Eigen::Vector3f* pts, const size_t n)
{
  static_assert(sizeof(pts[0]) == 12, "Invalid Vector3f size");
  static const __m256i index = _mm256_set_epi32(21, 18, 15, 12,  9,  6,  3,  0);
  static const __m256i masks[8] =
  {
    _mm256_set_epi32(-1, -1, -1, -1, -1, -1, -1, -1),
    _mm256_set_epi32( 0, -1, -1, -1, -1, -1, -1, -1),
    _mm256_set_epi32( 0,  0, -1, -1, -1, -1, -1, -1),
    _mm256_set_epi32( 0,  0,  0, -1, -1, -1, -1, -1),
    _mm256_set_epi32( 0,  0,  0,  0, -1, -1, -1, -1),
    _mm256_set_epi32( 0,  0,  0,  0,  0, -1, -1, -1),
    _mm256_set_epi32( 0,  0,  0,  0,  0,  0, -1, -1),
    _mm256_set_epi32( 0,  0,  0,  0,  0,  0,  0, -1)
  };
  
  if (n<1 || n>8)
    throw std::logic_error("distance_point_ellipse_avx2: invalid count");
  
  // We use 1 for masked-out values so we avoid division by zero
  __m256 ones = _mm256_set1_ps(1.f);
  
  __m256 x = _mm256_mask_i32gather_ps(ones, &pts[0](0), _mm256_load_si256(&index),
    _mm256_castsi256_ps(_mm256_load_si256(&masks[8-n])), 4);
  __m256 y = _mm256_mask_i32gather_ps(ones, &pts[0](1), _mm256_load_si256(&index),
    _mm256_castsi256_ps(_mm256_load_si256(&masks[8-n])), 4);
  __m256 w = _mm256_mask_i32gather_ps(ones, &pts[0](2), _mm256_load_si256(&index),
    _mm256_castsi256_ps(_mm256_load_si256(&masks[8-n])), 4);
}

}
}

