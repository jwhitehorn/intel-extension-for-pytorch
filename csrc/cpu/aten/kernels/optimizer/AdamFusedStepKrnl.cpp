#include <aten/optimizer/optimizer.h>
#include "vec/vec.h"

#include <torch/all.h>
#include <torch/csrc/autograd/function.h>
namespace torch_ipex {
namespace cpu {

namespace {

using namespace at::vec;

template <typename scalar_t, typename grad_t>
void adam_fused_step_kernel(
    const at::Tensor& param,
    const at::Tensor& exp_avg,
    const at::Tensor& exp_avg_sq,
    const at::Tensor& max_exp_avg_sq,
    const at::Tensor& grad,
    const at::Tensor& param2,
    bool amsgrad,
    double beta2_double,
    double learning_rate_double,
    double weight_decay_double,
    double eps_double,
    double step_size_double,
    double bias_correction2_sqrt_double,
    double exp_avg_grad_coefficient_double,
    double exp_avg_sq_grad_coefficient_double) {
  scalar_t* param_data = param.data_ptr<scalar_t>();
  scalar_t* exp_avg_data = exp_avg.data_ptr<scalar_t>();
  scalar_t* exp_avg_sq_data = exp_avg_sq.data_ptr<scalar_t>();
  scalar_t* max_exp_avg_sq_data = max_exp_avg_sq.data_ptr<scalar_t>();
  scalar_t* grad_data = grad.data_ptr<scalar_t>();

  // cast all scalar value to the same dtype with parameters
  scalar_t beta2 = scalar_t(beta2_double);
  scalar_t learning_rate = scalar_t(learning_rate_double);
  scalar_t weight_decay = scalar_t(weight_decay_double);
  scalar_t eps = scalar_t(eps_double);
  scalar_t step_size = scalar_t(step_size_double);
  scalar_t bias_correction2_sqrt = scalar_t(bias_correction2_sqrt_double);
  scalar_t exp_avg_grad_coefficient = scalar_t(exp_avg_grad_coefficient_double);
  scalar_t exp_avg_sq_grad_coefficient =
      scalar_t(exp_avg_sq_grad_coefficient_double);

  using Vec = at::vec::Vectorized<scalar_t>;
  int64_t grain_size = 512;

  // update momentum vt and mt
  // also accumulate sum of param_norm and rtw_norm
  at::parallel_for(
      0, param.numel(), grain_size, [&](int64_t begin, int64_t end) {
        // local pointers
        scalar_t* param_ptr = param_data + begin;
        scalar_t* exp_avg_ptr = exp_avg_data + begin;
        scalar_t* exp_avg_sq_ptr = exp_avg_sq_data + begin;
        scalar_t* grad_ptr = grad_data + begin;
        scalar_t* max_exp_avg_sq_ptr = max_exp_avg_sq_data + begin;

        const int64_t size = end - begin;

        int64_t d = 0;
        for (; d < size - (size % Vec::size()); d += Vec::size()) {
          Vec param_vec = Vec::loadu(param_ptr + d);
          Vec grad_vec = Vec::loadu(grad_ptr + d);
          if (weight_decay != 0.f) {
            // only accumulate weight decay when weight_decay != 0 to avoid NaN
            // propagation from param to grad
            grad_vec += param_vec * Vec(weight_decay);
          }

          Vec exp_avg_vec = Vec::loadu(exp_avg_ptr + d);
          // exp_avg.lerp_(grad, 1 - beta1)
          // exactly match
          // https://github.com/pytorch/pytorch/blob/d04957c0c682d766987cad07dce20986ca4a5b78/aten/src/ATen/native/cpu/LerpKernel.cpp#L99-L110
          const Vec lerp_weight = Vec(exp_avg_grad_coefficient);
          auto mask = lerp_weight.abs() < Vec(0.5);
          auto coeff = Vec::blendv(lerp_weight - Vec(1), lerp_weight, mask);
          auto base = Vec::blendv(grad_vec, exp_avg_vec, mask);
          exp_avg_vec = fmadd(coeff, grad_vec - exp_avg_vec, base);

          Vec exp_avg_sq_vec = Vec::loadu(exp_avg_sq_ptr + d) * Vec(beta2) +
              Vec(exp_avg_sq_grad_coefficient) * grad_vec * grad_vec;
          exp_avg_vec.store(exp_avg_ptr + d);
          exp_avg_sq_vec.store(exp_avg_sq_ptr + d);

          Vec denom_vec;
          if (amsgrad) {
            Vec max_exp_avg_sq_vec =
                maximum(Vec::loadu(max_exp_avg_sq_ptr + d), exp_avg_sq_vec);
            max_exp_avg_sq_vec.store(max_exp_avg_sq_ptr + d);
            denom_vec = max_exp_avg_sq_vec.sqrt() / Vec(bias_correction2_sqrt) +
                Vec(eps);
          } else {
            denom_vec =
                exp_avg_sq_vec.sqrt() / Vec(bias_correction2_sqrt) + Vec(eps);
          }
          param_vec = param_vec - Vec(step_size) * exp_avg_vec / denom_vec;
          param_vec.store(param_ptr + d);
        }
        for (; d < size; d++) {
          scalar_t grad_val = grad_ptr[d];
          if (weight_decay != 0.f) {
            // only accumulate weight decay when weight_decay != 0 to avoid NaN
            // propagation from param to grad
            grad_val += param_ptr[d] * weight_decay;
          }
          // exp_avg.lerp_(grad, 1 - beta1)
          // exactly match
          // https://github.com/pytorch/pytorch/blob/d04957c0c682d766987cad07dce20986ca4a5b78/aten/src/ATen/native/cpu/LerpKernel.cpp#L99-L110
          auto is_lerp_weight_small = std::abs(exp_avg_grad_coefficient) < 0.5;
          if (is_lerp_weight_small) {
            exp_avg_ptr[d] = exp_avg_ptr[d] +
                exp_avg_grad_coefficient * (grad_val - exp_avg_ptr[d]);
          } else {
            exp_avg_ptr[d] = grad_val -
                (grad_val - exp_avg_ptr[d]) * (1 - exp_avg_grad_coefficient);
          }
          exp_avg_sq_ptr[d] = exp_avg_sq_ptr[d] * beta2 +
              exp_avg_sq_grad_coefficient * grad_val * grad_val;
          scalar_t demon_val;
          if (amsgrad) {
            max_exp_avg_sq_ptr[d] =
                std::max(max_exp_avg_sq_ptr[d], exp_avg_sq_ptr[d]);
            demon_val =
                std::sqrt(max_exp_avg_sq_ptr[d]) / bias_correction2_sqrt + eps;
          } else {
            demon_val =
                std::sqrt(exp_avg_sq_ptr[d]) / bias_correction2_sqrt + eps;
          }
          param_ptr[d] = param_ptr[d] - step_size * exp_avg_ptr[d] / demon_val;
        }
      });
}

template <>
void adam_fused_step_kernel<at::BFloat16, at::BFloat16>(
    const at::Tensor& param,
    const at::Tensor& exp_avg,
    const at::Tensor& exp_avg_sq,
    const at::Tensor& max_exp_avg_sq,
    const at::Tensor& grad,
    const at::Tensor& param2,
    bool amsgrad,
    double beta2_double,
    double learning_rate_double,
    double weight_decay_double,
    double eps_double,
    double step_size_double,
    double bias_correction2_sqrt_double,
    double exp_avg_grad_coefficient_double,
    double exp_avg_sq_grad_coefficient_double) {
  TORCH_CHECK(
      param.scalar_type() == at::kBFloat16,
      "adam_fused_step_kernel: expect param to be at::BFloat16");
  TORCH_CHECK(
      grad.scalar_type() == at::kBFloat16,
      "adam_fused_step_kernel: expect grad to be at::BFloat16");
  TORCH_CHECK(
      exp_avg.scalar_type() == at::kFloat,
      "adam_fused_step_kernel: expect exp_avg to be float32");
  TORCH_CHECK(
      exp_avg_sq.scalar_type() == at::kFloat,
      "adam_fused_step_kernel: expect exp_avg_sq to be float32");
  TORCH_CHECK(
      max_exp_avg_sq.scalar_type() == at::kFloat,
      "adam_fused_step_kernel: expect max_exp_avg_sq to be float32");
  TORCH_CHECK(
      param2.scalar_type() == at::kBFloat16,
      "adam_fused_step_kernel: expect param2 to be at::BFloat16");

  at::BFloat16* param_data = param.data_ptr<at::BFloat16>();
  float* exp_avg_data = exp_avg.data_ptr<float>();
  float* exp_avg_sq_data = exp_avg_sq.data_ptr<float>();
  float* max_exp_avg_sq_data = max_exp_avg_sq.data_ptr<float>();
  at::BFloat16* grad_data = grad.data_ptr<at::BFloat16>();
  at::BFloat16* param2_data = param2.data_ptr<at::BFloat16>();

  // cast all scalar value to float for computation
  float beta2 = float(beta2_double);
  float learning_rate = float(learning_rate_double);
  float weight_decay = float(weight_decay_double);
  float eps = float(eps_double);
  float step_size = float(step_size_double);
  float bias_correction2_sqrt = float(bias_correction2_sqrt_double);
  float exp_avg_grad_coefficient = float(exp_avg_grad_coefficient_double);
  float exp_avg_sq_grad_coefficient = float(exp_avg_sq_grad_coefficient_double);

  using bVec = at::vec::Vectorized<at::BFloat16>;
  using fVec = at::vec::Vectorized<float>;

  int64_t grain_size = 512;

  at::parallel_for(
      0, param.numel(), grain_size, [&](int64_t begin, int64_t end) {
        // local pointers
        at::BFloat16* param_ptr = param_data + begin;
        float* exp_avg_ptr = exp_avg_data + begin;
        float* exp_avg_sq_ptr = exp_avg_sq_data + begin;
        float* max_exp_avg_sq_ptr = max_exp_avg_sq_data + begin;
        at::BFloat16* grad_ptr = grad_data + begin;
        at::BFloat16* param2_ptr = param2_data + begin;

        const int64_t size = end - begin;

        int64_t d = 0;
        for (; d < size - (size % bVec::size()); d += bVec::size()) {
          // load grad vec
          bVec grad_bvec = bVec::loadu(grad_ptr + d);
          fVec grad_fvec, grad_fvec2;
          std::tie(grad_fvec, grad_fvec2) = convert_bfloat16_float(grad_bvec);
          // load param vec
          bVec param_bvec = bVec::loadu(param_ptr + d);
          bVec param2_bvec = bVec::loadu(param2_ptr + d);
          fVec param_fvec, param_fvec2;
          std::tie(param_fvec, param_fvec2) =
              at::vec::pack_bfloat16_float(param_bvec, param2_bvec);
          // weight decay
          if (weight_decay != 0.f) {
            // only accumulate weight decay when weight_decay != 0 to avoid NaN
            // propagation from param to grad
            grad_fvec = grad_fvec + param_fvec * fVec(weight_decay);
            grad_fvec2 = grad_fvec2 + param_fvec2 * fVec(weight_decay);
          }

          // update exp_avg, exp_avg_sq
          // exp_avg.lerp_(grad, 1 - beta1)
          // exactly match
          // https://github.com/pytorch/pytorch/blob/d04957c0c682d766987cad07dce20986ca4a5b78/aten/src/ATen/native/cpu/LerpKernel.cpp#L99-L110
          fVec exp_avg_fvec = fVec::loadu(exp_avg_ptr + d);
          fVec exp_avg_fvec2 = fVec::loadu(exp_avg_ptr + d + fVec::size());
          fVec lerp_weight = fVec(exp_avg_grad_coefficient);
          auto mask = lerp_weight.abs() < fVec(0.5);
          auto coeff = fVec::blendv(lerp_weight - fVec(1), lerp_weight, mask);
          auto base = fVec::blendv(grad_fvec, exp_avg_fvec, mask);
          exp_avg_fvec = fmadd(coeff, grad_fvec - exp_avg_fvec, base);
          auto base2 = fVec::blendv(grad_fvec2, exp_avg_fvec2, mask);
          exp_avg_fvec2 = fmadd(coeff, grad_fvec2 - exp_avg_fvec2, base2);
          exp_avg_fvec.store(exp_avg_ptr + d);
          exp_avg_fvec2.store(exp_avg_ptr + d + fVec::size());

          fVec exp_avg_sq_fvec = fVec::loadu(exp_avg_sq_ptr + d) * fVec(beta2) +
              fVec(exp_avg_sq_grad_coefficient) * grad_fvec * grad_fvec;
          fVec exp_avg_sq_fvec2 =
              fVec::loadu(exp_avg_sq_ptr + d + fVec::size()) * fVec(beta2) +
              fVec(exp_avg_sq_grad_coefficient) * grad_fvec2 * grad_fvec2;
          exp_avg_sq_fvec.store(exp_avg_sq_ptr + d);
          exp_avg_sq_fvec2.store(exp_avg_sq_ptr + d + fVec::size());
          // amsgrad
          fVec denom_fvec, denom_fvec2;
          if (amsgrad) {
            fVec max_exp_avg_sq_fvec =
                maximum(fVec::loadu(max_exp_avg_sq_ptr + d), exp_avg_sq_fvec);
            fVec max_exp_avg_sq_fvec2 = maximum(
                fVec::loadu(max_exp_avg_sq_ptr + d + fVec::size()),
                exp_avg_sq_fvec2);
            max_exp_avg_sq_fvec.store(max_exp_avg_sq_ptr + d);
            max_exp_avg_sq_fvec2.store(max_exp_avg_sq_ptr + d + fVec::size());
            denom_fvec =
                max_exp_avg_sq_fvec.sqrt() / fVec(bias_correction2_sqrt) +
                fVec(eps);
            denom_fvec2 =
                max_exp_avg_sq_fvec2.sqrt() / fVec(bias_correction2_sqrt) +
                fVec(eps);
          } else {
            denom_fvec = exp_avg_sq_fvec.sqrt() / fVec(bias_correction2_sqrt) +
                fVec(eps);
            denom_fvec2 =
                exp_avg_sq_fvec2.sqrt() / fVec(bias_correction2_sqrt) +
                fVec(eps);
          }
          // update param
          param_fvec = param_fvec - fVec(step_size) * exp_avg_fvec / denom_fvec;
          param_fvec2 =
              param_fvec2 - fVec(step_size) * exp_avg_fvec2 / denom_fvec2;
          std::tie(param_bvec, param2_bvec) =
              at::vec::unpack_float_bfloat16(param_fvec, param_fvec2);
          param_bvec.store(param_ptr + d);
          param2_bvec.store(param2_ptr + d);
        }
        for (; d < size; d++) {
          float param_val =
              at::vec::pack_bfloat16_float(param_ptr[d], param2_ptr[d]);
          float grad_val = grad_ptr[d];
          if (weight_decay != 0.f) {
            // only accumulate weight decay when weight_decay != 0 to avoid NaN
            // propagation from param to grad
            grad_val = grad_val + param_val * weight_decay;
          }
          // exp_avg.lerp_(grad, 1 - beta1)
          // exactly match
          // https://github.com/pytorch/pytorch/blob/d04957c0c682d766987cad07dce20986ca4a5b78/aten/src/ATen/native/cpu/LerpKernel.cpp#L99-L110
          auto is_lerp_weight_small = std::abs(exp_avg_grad_coefficient) < 0.5;
          if (is_lerp_weight_small) {
            exp_avg_ptr[d] = exp_avg_ptr[d] +
                exp_avg_grad_coefficient * (grad_val - exp_avg_ptr[d]);
          } else {
            exp_avg_ptr[d] = grad_val -
                (grad_val - exp_avg_ptr[d]) * (1 - exp_avg_grad_coefficient);
          }
          exp_avg_sq_ptr[d] = exp_avg_sq_ptr[d] * beta2 +
              exp_avg_sq_grad_coefficient * grad_val * grad_val;
          float demon_val;
          if (amsgrad) {
            max_exp_avg_sq_ptr[d] =
                std::max(max_exp_avg_sq_ptr[d], exp_avg_sq_ptr[d]);
            demon_val =
                std::sqrt(max_exp_avg_sq_ptr[d]) / bias_correction2_sqrt + eps;
          } else {
            demon_val =
                std::sqrt(exp_avg_sq_ptr[d]) / bias_correction2_sqrt + eps;
          }
          param_val = param_val - step_size * exp_avg_ptr[d] / demon_val;
          std::tie(param_ptr[d], param2_ptr[d]) =
              at::vec::unpack_float_bfloat16(param_val);
        }
      });
}

template <>
void adam_fused_step_kernel<float, at::BFloat16>(
    const at::Tensor& param,
    const at::Tensor& exp_avg,
    const at::Tensor& exp_avg_sq,
    const at::Tensor& max_exp_avg_sq,
    const at::Tensor& grad,
    const at::Tensor& param2,
    bool amsgrad,
    double beta2_double,
    double learning_rate_double,
    double weight_decay_double,
    double eps_double,
    double step_size_double,
    double bias_correction2_sqrt_double,
    double exp_avg_grad_coefficient_double,
    double exp_avg_sq_grad_coefficient_double) {
  TORCH_CHECK(
      param.scalar_type() == at::kFloat,
      "adam_fused_step_kernel: expect param to be at::Float");
  TORCH_CHECK(
      grad.scalar_type() == at::kBFloat16,
      "adam_fused_step_kernel: expect grad to be at::BFloat16");
  TORCH_CHECK(
      exp_avg.scalar_type() == at::kFloat,
      "adam_fused_step_kernel: expect exp_avg to be float32");
  TORCH_CHECK(
      exp_avg_sq.scalar_type() == at::kFloat,
      "adam_fused_step_kernel: expect exp_avg_sq to be float32");
  TORCH_CHECK(
      max_exp_avg_sq.scalar_type() == at::kFloat,
      "adam_fused_step_kernel: expect max_exp_avg_sq to be float32");
  TORCH_CHECK(
      param2.scalar_type() == at::kBFloat16,
      "adam_fused_step_kernel: expect param2 to be at::BFloat16");

  float* param_data = param.data_ptr<float>();
  float* exp_avg_data = exp_avg.data_ptr<float>();
  float* exp_avg_sq_data = exp_avg_sq.data_ptr<float>();
  float* max_exp_avg_sq_data = max_exp_avg_sq.data_ptr<float>();
  at::BFloat16* grad_data = grad.data_ptr<at::BFloat16>();
  at::BFloat16* param2_data = param2.data_ptr<at::BFloat16>();

  // cast all scalar value to float for computation
  float beta2 = float(beta2_double);
  float learning_rate = float(learning_rate_double);
  float weight_decay = float(weight_decay_double);
  float eps = float(eps_double);
  float step_size = float(step_size_double);
  float bias_correction2_sqrt = float(bias_correction2_sqrt_double);
  float exp_avg_grad_coefficient = float(exp_avg_grad_coefficient_double);
  float exp_avg_sq_grad_coefficient = float(exp_avg_sq_grad_coefficient_double);

  using bVec = at::vec::Vectorized<at::BFloat16>;
  using fVec = at::vec::Vectorized<float>;

  int64_t grain_size = 512;

  at::parallel_for(
      0, param.numel(), grain_size, [&](int64_t begin, int64_t end) {
        // local pointers
        float* param_ptr = param_data + begin;
        float* exp_avg_ptr = exp_avg_data + begin;
        float* exp_avg_sq_ptr = exp_avg_sq_data + begin;
        float* max_exp_avg_sq_ptr = max_exp_avg_sq_data + begin;
        at::BFloat16* grad_ptr = grad_data + begin;
        at::BFloat16* param2_ptr = param2_data + begin;

        const int64_t size = end - begin;

        int64_t d = 0;
        for (; d < size - (size % bVec::size()); d += bVec::size()) {
          // load grad vec
          bVec grad_bvec = bVec::loadu(grad_ptr + d);
          fVec grad_fvec, grad_fvec2;
          std::tie(grad_fvec, grad_fvec2) = convert_bfloat16_float(grad_bvec);
          // load param vec
          fVec param_fvec = fVec::loadu(param_ptr + d);
          fVec param_fvec2 = fVec::loadu(param_ptr + d + fVec::size());
          // weight decay
          if (weight_decay != 0.f) {
            // only accumulate weight decay when weight_decay != 0 to avoid NaN
            // propagation from param to grad
            grad_fvec = grad_fvec + param_fvec * fVec(weight_decay);
            grad_fvec2 = grad_fvec2 + param_fvec2 * fVec(weight_decay);
          }
          // update exp_avg, exp_avg_sq
          // exp_avg.lerp_(grad, 1 - beta1)
          // exactly match
          // https://github.com/pytorch/pytorch/blob/d04957c0c682d766987cad07dce20986ca4a5b78/aten/src/ATen/native/cpu/LerpKernel.cpp#L99-L110
          fVec exp_avg_fvec = fVec::loadu(exp_avg_ptr + d);
          fVec exp_avg_fvec2 = fVec::loadu(exp_avg_ptr + d + fVec::size());
          fVec lerp_weight = fVec(exp_avg_grad_coefficient);
          auto mask = lerp_weight.abs() < fVec(0.5);
          auto coeff = fVec::blendv(lerp_weight - fVec(1), lerp_weight, mask);
          auto base = fVec::blendv(grad_fvec, exp_avg_fvec, mask);
          exp_avg_fvec = fmadd(coeff, grad_fvec - exp_avg_fvec, base);
          auto base2 = fVec::blendv(grad_fvec2, exp_avg_fvec2, mask);
          exp_avg_fvec2 = fmadd(coeff, grad_fvec2 - exp_avg_fvec2, base2);
          exp_avg_fvec.store(exp_avg_ptr + d);
          exp_avg_fvec2.store(exp_avg_ptr + d + fVec::size());

          fVec exp_avg_sq_fvec = fVec::loadu(exp_avg_sq_ptr + d) * fVec(beta2) +
              fVec(exp_avg_sq_grad_coefficient) * grad_fvec * grad_fvec;
          fVec exp_avg_sq_fvec2 =
              fVec::loadu(exp_avg_sq_ptr + d + fVec::size()) * fVec(beta2) +
              fVec(exp_avg_sq_grad_coefficient) * grad_fvec2 * grad_fvec2;
          exp_avg_sq_fvec.store(exp_avg_sq_ptr + d);
          exp_avg_sq_fvec2.store(exp_avg_sq_ptr + d + fVec::size());
          // amsgrad
          fVec denom_fvec, denom_fvec2;
          if (amsgrad) {
            fVec max_exp_avg_sq_fvec =
                maximum(fVec::loadu(max_exp_avg_sq_ptr + d), exp_avg_sq_fvec);
            fVec max_exp_avg_sq_fvec2 = maximum(
                fVec::loadu(max_exp_avg_sq_ptr + d + fVec::size()),
                exp_avg_sq_fvec2);
            max_exp_avg_sq_fvec.store(max_exp_avg_sq_ptr + d);
            max_exp_avg_sq_fvec2.store(max_exp_avg_sq_ptr + d + fVec::size());
            denom_fvec =
                max_exp_avg_sq_fvec.sqrt() / fVec(bias_correction2_sqrt) +
                fVec(eps);
            denom_fvec2 =
                max_exp_avg_sq_fvec2.sqrt() / fVec(bias_correction2_sqrt) +
                fVec(eps);
          } else {
            denom_fvec = exp_avg_sq_fvec.sqrt() / fVec(bias_correction2_sqrt) +
                fVec(eps);
            denom_fvec2 =
                exp_avg_sq_fvec2.sqrt() / fVec(bias_correction2_sqrt) +
                fVec(eps);
          }
          // update param
          param_fvec = param_fvec - fVec(step_size) * exp_avg_fvec / denom_fvec;
          param_fvec2 =
              param_fvec2 - fVec(step_size) * exp_avg_fvec2 / denom_fvec2;
          param_fvec.store(param_ptr + d);
          param_fvec2.store(param_ptr + d + fVec::size());
          // sync float param to bfloat16
          bVec param2_bvec = convert_float_bfloat16(param_fvec, param_fvec2);
          param2_bvec.store(param2_ptr + d);
        }
        for (; d < size; d++) {
          float grad_val = grad_ptr[d];
          if (weight_decay != 0.f) {
            // only accumulate weight decay when weight_decay != 0 to avoid NaN
            // propagation from param to grad
            grad_val = grad_val + param_ptr[d] * weight_decay;
          }
          // exp_avg.lerp_(grad, 1 - beta1)
          // exactly match
          // https://github.com/pytorch/pytorch/blob/d04957c0c682d766987cad07dce20986ca4a5b78/aten/src/ATen/native/cpu/LerpKernel.cpp#L99-L110
          auto is_lerp_weight_small = std::abs(exp_avg_grad_coefficient) < 0.5;
          if (is_lerp_weight_small) {
            exp_avg_ptr[d] = exp_avg_ptr[d] +
                exp_avg_grad_coefficient * (grad_val - exp_avg_ptr[d]);
          } else {
            exp_avg_ptr[d] = grad_val -
                (grad_val - exp_avg_ptr[d]) * (1 - exp_avg_grad_coefficient);
          }
          exp_avg_sq_ptr[d] = exp_avg_sq_ptr[d] * beta2 +
              exp_avg_sq_grad_coefficient * grad_val * grad_val;
          float demon_val;
          if (amsgrad) {
            max_exp_avg_sq_ptr[d] =
                std::max(max_exp_avg_sq_ptr[d], exp_avg_sq_ptr[d]);
            demon_val =
                std::sqrt(max_exp_avg_sq_ptr[d]) / bias_correction2_sqrt + eps;
          } else {
            demon_val =
                std::sqrt(exp_avg_sq_ptr[d]) / bias_correction2_sqrt + eps;
          }
          param_ptr[d] = param_ptr[d] - step_size * exp_avg_ptr[d] / demon_val;
          param2_ptr[d] = at::BFloat16(param_ptr[d]);
        }
      });
}

void adam_fused_step_kernel_impl(
    const at::Tensor& param_,
    const at::Tensor& exp_avg_,
    const at::Tensor& exp_avg_sq_,
    const at::Tensor& max_exp_avg_sq_,
    const at::Tensor& grad_,
    const at::Tensor& param2_,
    bool amsgrad,
    double step,
    double beta1,
    double beta2,
    double learning_rate,
    double weight_decay,
    double eps) {
  auto param = param_.contiguous();
  auto exp_avg = exp_avg_.contiguous();
  auto exp_avg_sq = exp_avg_sq_.contiguous();
  auto max_exp_avg_sq = max_exp_avg_sq_.contiguous();
  auto grad = grad_.contiguous();
  auto param2 = param2_.contiguous();

  auto grad_dtype = grad_.scalar_type();
  auto param_dtype = param_.scalar_type();

  // make sure all scalar args are computationed with double precision
  double bias_correction1 = 1 - std::pow(beta1, step);
  double step_size = learning_rate / bias_correction1;
  double bias_correction2 = 1 - std::pow(beta2, step);
  double bias_correction2_sqrt = std::sqrt(bias_correction2);
  double exp_avg_grad_coefficient = 1 - beta1;
  double exp_avg_sq_grad_coefficient = 1 - beta2;

  if (at::ScalarType::Float == grad_dtype) {
    adam_fused_step_kernel<float, float>(
        param,
        exp_avg,
        exp_avg_sq,
        max_exp_avg_sq,
        grad,
        param2,
        amsgrad,
        beta2,
        learning_rate,
        weight_decay,
        eps,
        step_size,
        bias_correction2_sqrt,
        exp_avg_grad_coefficient,
        exp_avg_sq_grad_coefficient);
  } else if (at::ScalarType::Double == grad_dtype) {
    adam_fused_step_kernel<double, double>(
        param,
        exp_avg,
        exp_avg_sq,
        max_exp_avg_sq,
        grad,
        param2,
        amsgrad,
        beta2,
        learning_rate,
        weight_decay,
        eps,
        step_size,
        bias_correction2_sqrt,
        exp_avg_grad_coefficient,
        exp_avg_sq_grad_coefficient);
  } else if (
      at::ScalarType::BFloat16 == grad_dtype &&
      at::ScalarType::BFloat16 == param_dtype) {
    adam_fused_step_kernel<at::BFloat16, at::BFloat16>(
        param,
        exp_avg,
        exp_avg_sq,
        max_exp_avg_sq,
        grad,
        param2,
        amsgrad,
        beta2,
        learning_rate,
        weight_decay,
        eps,
        step_size,
        bias_correction2_sqrt,
        exp_avg_grad_coefficient,
        exp_avg_sq_grad_coefficient);
  } else if (
      at::ScalarType::BFloat16 == grad_dtype &&
      at::ScalarType::Float == param_dtype) {
    adam_fused_step_kernel<float, at::BFloat16>(
        param,
        exp_avg,
        exp_avg_sq,
        max_exp_avg_sq,
        grad,
        param2,
        amsgrad,
        beta2,
        learning_rate,
        weight_decay,
        eps,
        step_size,
        bias_correction2_sqrt,
        exp_avg_grad_coefficient,
        exp_avg_sq_grad_coefficient);
  } else {
    TORCH_CHECK(false, "expect bfloat16 or float or double param");
  }

  if (!param_.is_contiguous()) {
    param_.copy_(param);
  }
  if (!exp_avg_.is_contiguous()) {
    exp_avg_.copy_(exp_avg);
  }
  if (!exp_avg_sq_.is_contiguous()) {
    exp_avg_sq_.copy_(exp_avg_sq);
  }
  if (!max_exp_avg_sq_.is_contiguous()) {
    max_exp_avg_sq_.copy_(max_exp_avg_sq);
  }
  if (!param2_.is_contiguous()) {
    param2_.copy_(param2);
  }
}

} // anonymous namespace

IPEX_REGISTER_DISPATCH(
    adam_fused_step_kernel_stub,
    &adam_fused_step_kernel_impl);

} // namespace cpu
} // namespace torch_ipex
