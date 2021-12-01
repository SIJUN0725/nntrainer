// SPDX-License-Identifier: Apache-2.0
/**
 * Copyright (C) 2021 Parichay Kapoor <pk.kapoor@samsung.com>
 *
 * @file   mol_attention_layer.cpp
 * @date   11 November 2021
 * @see    https://github.com/nnstreamer/nntrainer
 * @author Parichay Kapoor <pk.kapoor@samsung.com>
 * @bug    No known bugs except for NYI items
 * @brief  This is MoL Attention Layer Class for Neural Network
 *
 */

#include <math.h>

#include <layer_context.h>
#include <mol_attention_layer.h>
#include <nntrainer_error.h>
#include <nntrainer_log.h>
#include <node_exporter.h>

namespace nntrainer {

MoLAttentionLayer::MoLAttentionLayer() : helper_exec(false), wt_idx({0}) {}

MoLAttentionLayer::~MoLAttentionLayer() {}

static constexpr size_t SINGLE_INOUT_IDX = 0;

enum AttentionParams {
  query = 0,
  value = 1,
  state = 2,
  fc_w,
  fc_bias,
  fc_proj_w,
  fc_out,
  fc_tanh,
  fc_proj_out,
  scores,
  prob,
  prob_left,
  prob_right,
  u_neg_div,
  u_pos_div
};

void MoLAttentionLayer::finalize(InitLayerContext &context) {
  if (context.getNumInputs() != 3)
    throw std::runtime_error("MoL Attention layer needs 3 inputs.");

  auto const &all_dims = context.getInputDimensions();
  auto const &query_dim = all_dims[AttentionParams::query];
  auto const &value_dim = all_dims[AttentionParams::value];

  wt_idx[AttentionParams::query] = AttentionParams::query;
  wt_idx[AttentionParams::value] = AttentionParams::value;
  wt_idx[AttentionParams::state] = AttentionParams::state;

  softmax.setActiFunc(ActivationType::ACT_SOFTMAX);
  tanh.setActiFunc(ActivationType::ACT_TANH);
  sigmoid.setActiFunc(ActivationType::ACT_SIGMOID);

  NNTR_THROW_IF(std::get<props::Unit>(mol_props).empty(), std::invalid_argument)
    << "Number of units not provided for layer " << context.getName();
  auto unit = std::get<props::Unit>(mol_props).get();

  NNTR_THROW_IF(std::get<props::MoL_K>(mol_props).empty(),
                std::invalid_argument)
    << "MoL_K property not provided for layer " << context.getName();
  auto mol_k = std::get<props::MoL_K>(mol_props).get();

  auto &weight_regularizer =
    std::get<props::WeightRegularizer>(*layer_impl_props);
  auto &weight_regularizer_constant =
    std::get<props::WeightRegularizerConstant>(*layer_impl_props);
  auto &weight_initializer =
    std::get<props::WeightInitializer>(*layer_impl_props);
  auto &bias_initializer = std::get<props::BiasInitializer>(*layer_impl_props);

  TensorDim fc_w_dim = {query_dim.width(), unit};
  wt_idx[AttentionParams::fc_w] =
    context.requestWeight(fc_w_dim, weight_initializer, weight_regularizer,
                          weight_regularizer_constant, "fc_w", true);
  TensorDim fc_bias_dim = {unit};
  wt_idx[AttentionParams::fc_bias] =
    context.requestWeight(fc_bias_dim, bias_initializer, weight_regularizer,
                          weight_regularizer_constant, "fc_bias", true);

  TensorDim fc_proj_w_dim = {unit, 3 * mol_k};
  wt_idx[AttentionParams::fc_proj_w] =
    context.requestWeight(fc_proj_w_dim, weight_initializer, weight_regularizer,
                          weight_regularizer_constant, "fc_proj_w", true);

  TensorDim fc_out_dim = query_dim;
  fc_out_dim.width(fc_w_dim.width());
  wt_idx[AttentionParams::fc_out] =
    context.requestTensor(fc_out_dim, "fc_out", Tensor::Initializer::NONE,
                          false, TensorLifespan::ITERATION_LIFESPAN);

  TensorDim fc_proj_out_dim = fc_out_dim;
  fc_out_dim.width(fc_proj_w_dim.width());
  wt_idx[AttentionParams::fc_proj_out] = context.requestTensor(
    fc_proj_out_dim, "fc_proj_out", Tensor::Initializer::NONE, false,
    TensorLifespan::ITERATION_LIFESPAN);

  TensorDim scores_dim =
    TensorDim({value_dim.batch(), 1, 1, value_dim.height()});
  wt_idx[AttentionParams::scores] =
    context.requestTensor(scores_dim, "scores", Tensor::Initializer::NONE,
                          false, TensorLifespan::ITERATION_LIFESPAN);

  TensorDim prob_dim = value_dim;
  prob_dim.width(mol_k);
  wt_idx[AttentionParams::prob] =
    context.requestTensor(prob_dim, "prob", Tensor::Initializer::NONE, false,
                          TensorLifespan::ITERATION_LIFESPAN);
  wt_idx[AttentionParams::prob_left] =
    context.requestTensor(prob_dim, "prob_left", Tensor::Initializer::NONE,
                          false, TensorLifespan::ITERATION_LIFESPAN);
  wt_idx[AttentionParams::prob_right] =
    context.requestTensor(prob_dim, "prob_right", Tensor::Initializer::NONE,
                          false, TensorLifespan::ITERATION_LIFESPAN);
  wt_idx[AttentionParams::u_neg_div] =
    context.requestTensor(prob_dim, "u_neg_div", Tensor::Initializer::NONE,
                          false, TensorLifespan::ITERATION_LIFESPAN);
  wt_idx[AttentionParams::u_pos_div] =
    context.requestTensor(prob_dim, "u_pos_div", Tensor::Initializer::NONE,
                          false, TensorLifespan::ITERATION_LIFESPAN);

  context.setOutputDimensions({query_dim});
}

void MoLAttentionLayer::forwarding(RunLayerContext &context, bool training) {
  Tensor &query = context.getInput(wt_idx[AttentionParams::query]);
  Tensor &value = context.getInput(wt_idx[AttentionParams::value]);
  Tensor &state = context.getInput(wt_idx[AttentionParams::state]);

  Tensor &output = context.getOutput(SINGLE_INOUT_IDX);
  Tensor &fc_w = context.getWeight(wt_idx[AttentionParams::fc_w]);
  Tensor &fc_bias = context.getWeight(wt_idx[AttentionParams::fc_bias]);
  Tensor &fc_proj_w = context.getWeight(wt_idx[AttentionParams::fc_proj_w]);
  Tensor &fc_out = context.getTensor(wt_idx[AttentionParams::fc_out]);
  Tensor &fc_tanh = context.getTensor(wt_idx[AttentionParams::fc_tanh]);
  Tensor &fc_proj_out = context.getTensor(wt_idx[AttentionParams::fc_proj_out]);
  Tensor &scores = context.getTensor(wt_idx[AttentionParams::scores]);
  Tensor &prob = context.getTensor(wt_idx[AttentionParams::prob]);
  Tensor &prob_left = context.getTensor(wt_idx[AttentionParams::prob_left]);
  Tensor &prob_right = context.getTensor(wt_idx[AttentionParams::prob_right]);
  Tensor &u_neg_div = context.getTensor(wt_idx[AttentionParams::u_neg_div]);
  Tensor &u_pos_div = context.getTensor(wt_idx[AttentionParams::u_pos_div]);

  const TensorDim &input_dim = query.getDim();
  unsigned int batch = input_dim.batch();
  auto mol_k = std::get<props::MoL_K>(mol_props).get();

  /** reset helper state */
  helper_exec = false;

  fc_out = query.dot(fc_w);
  fc_out.add_i(fc_bias);

  tanh.run_fn(fc_out, fc_tanh);

  fc_proj_out = fc_tanh.dot(fc_proj_w);

  Tensor kappa_src, beta_src, alpha_src;
  kappa_src.copy_with_stride(
    fc_proj_out.getSharedDataTensor({batch, 1, 1, mol_k}, 0, false));
  beta_src.copy_with_stride(
    fc_proj_out.getSharedDataTensor({batch, 1, 1, mol_k}, mol_k, false));
  alpha_src.copy_with_stride(
    fc_proj_out.getSharedDataTensor({batch, 1, 1, mol_k}, mol_k * 2, false));

  kappa_src.apply_i(&expf);
  beta_src.apply_i(&expf);
  Tensor kappa = kappa_src;
  Tensor beta = beta_src;

  Tensor alpha;
  softmax.run_fn(alpha_src, alpha);

  fc_proj_out.getSharedDataTensor({batch, 1, 1, mol_k}, 0, false)
    .copy_with_stride(kappa);
  fc_proj_out.getSharedDataTensor({batch, 1, 1, mol_k}, mol_k, false)
    .copy_with_stride(beta);
  fc_proj_out.getSharedDataTensor({batch, 1, 1, mol_k}, mol_k * 2, false)
    .copy_with_stride(alpha);

  Tensor m = state.add(kappa);

  /** @todo cache u_base, u_pos, u_neg */
  Tensor u_base = Tensor(TensorDim({batch, 1, value.height(), mol_k}));
  for (unsigned int b = 0; b < batch; b++) {
    for (unsigned int h = 0; h < u_base.height(); h++) {
      float *u_data = u_base.getAddress(b, 0, h, 0);
      std::fill(u_data, u_data + u_base.width(), h + 1);
    }
  }

  Tensor u_pos = u_base.add(0.5);
  u_base.add_i(-0.5);
  Tensor u_neg = u_base;

  Tensor beta_eps = beta.add(1e-8f);

  Tensor u_pos_m = u_pos.subtract(m);
  u_pos_m.divide(beta_eps, u_pos_div);
  sigmoid.run_fn(u_pos_div, prob_left);

  Tensor u_neg_m = u_neg.subtract(m);
  u_neg_m.divide(beta_eps, u_neg_div);
  sigmoid.run_fn(u_neg_div, prob_right);

  prob_left.subtract(prob_right, prob);

  Tensor prob_scaled = prob.multiply(alpha);
  prob_scaled.sum(3, scores);

  scores.dotBatched(value, output);
}

void MoLAttentionLayer::calcDerivativeHelper(RunLayerContext &context,
                                             Tensor &dstate) {
  Tensor &query = context.getInput(wt_idx[AttentionParams::query]);
  Tensor &value = context.getInput(wt_idx[AttentionParams::value]);

  Tensor &derivative = context.getIncomingDerivative(SINGLE_INOUT_IDX);

  Tensor &fc_proj_out = context.getTensor(wt_idx[AttentionParams::fc_proj_out]);
  Tensor &dfc_proj_out =
    context.getTensor(wt_idx[AttentionParams::fc_proj_out]);
  Tensor &scores = context.getTensor(wt_idx[AttentionParams::scores]);
  Tensor &prob = context.getTensor(wt_idx[AttentionParams::prob]);
  Tensor &prob_left = context.getTensor(wt_idx[AttentionParams::prob_left]);
  Tensor &prob_right = context.getTensor(wt_idx[AttentionParams::prob_right]);
  Tensor &u_neg_div = context.getTensor(wt_idx[AttentionParams::u_neg_div]);
  Tensor &u_pos_div = context.getTensor(wt_idx[AttentionParams::u_pos_div]);

  const TensorDim &input_dim = query.getDim();
  unsigned int batch = input_dim.batch();
  auto mol_k = std::get<props::MoL_K>(mol_props).get();

  Tensor kappa, beta, alpha;
  kappa.copy_with_stride(
    fc_proj_out.getSharedDataTensor({batch, 1, 1, mol_k}, 0, false));
  beta.copy_with_stride(
    fc_proj_out.getSharedDataTensor({batch, 1, 1, mol_k}, mol_k, false));
  alpha.copy_with_stride(
    fc_proj_out.getSharedDataTensor({batch, 1, 1, mol_k}, mol_k * 2, false));

  Tensor dscores = Tensor(TensorDim({value.batch(), 1, 1, value.height()}));
  dscores.dot_batched_deriv_wrt_1(value, derivative);
  dscores.reshape(TensorDim({scores.batch(), 1, scores.width(), 1}));

  Tensor dprob_scaled = Tensor(TensorDim({batch, 1, value.height(), mol_k}));
  dprob_scaled.setZero();
  dprob_scaled.add_i(dscores);

  Tensor dalpha = dprob_scaled.multiply(prob).sum(2);
  Tensor dprob = dprob_scaled.multiply(alpha);

  Tensor dprob_left = dprob;
  Tensor dprob_right = dprob.multiply(-1);

  Tensor beta_eps = beta.add(1e-8f);
  Tensor du_neg_div, du_pos_div;
  sigmoid.run_prime_fn(prob_right, du_neg_div, dprob_right);
  Tensor du_neg_m = du_neg_div.divide(beta_eps);
  Tensor dm_neg = du_neg_m.multiply(-1).sum(2);
  Tensor dbeta_eps_neg = du_neg_m.multiply(u_neg_div).multiply(-1).sum(2);

  sigmoid.run_prime_fn(prob_left, du_pos_div, dprob_left);
  Tensor du_pos_m = du_pos_div.divide(beta_eps);
  Tensor dm_pos = du_pos_m.multiply(-1).sum(2);
  Tensor dbeta_eps_pos = du_pos_m.multiply(u_pos_div).multiply(-1).sum(2);

  Tensor dbeta_eps = dbeta_eps_neg.add(dbeta_eps_pos);
  dm_neg.add(dm_pos, dstate);
  Tensor dkappa = dstate;
  Tensor dbeta = dbeta_eps;

  Tensor dalpha_src;
  softmax.run_prime_fn(alpha, dalpha_src, dalpha);

  Tensor dkappa_src = dkappa.multiply(kappa);
  Tensor dbeta_src = dbeta.multiply(beta);

  /** dfc_proj_out shares memory with fc_proj_out */
  dfc_proj_out.getSharedDataTensor({batch, 1, 1, mol_k}, 0, false)
    .copy_with_stride(dkappa_src);
  dfc_proj_out.getSharedDataTensor({batch, 1, 1, mol_k}, mol_k, false)
    .copy_with_stride(dbeta_src);
  dfc_proj_out.getSharedDataTensor({batch, 1, 1, mol_k}, mol_k * 2, false)
    .copy_with_stride(dalpha_src);

  /** update the helper state */
  helper_exec = true;
}

void MoLAttentionLayer::calcDerivative(RunLayerContext &context) {
  Tensor &dquery =
    context.getOutgoingDerivative(wt_idx[AttentionParams::query]);
  Tensor &dvalue =
    context.getOutgoingDerivative(wt_idx[AttentionParams::value]);
  Tensor &dstate =
    context.getOutgoingDerivative(wt_idx[AttentionParams::state]);

  Tensor &derivative = context.getIncomingDerivative(SINGLE_INOUT_IDX);

  Tensor &fc_w = context.getWeight(wt_idx[AttentionParams::fc_w]);
  Tensor &fc_proj_w = context.getWeight(wt_idx[AttentionParams::fc_proj_w]);
  Tensor &fc_out = context.getTensor(wt_idx[AttentionParams::fc_out]);
  Tensor &fc_tanh = context.getTensor(wt_idx[AttentionParams::fc_tanh]);
  Tensor &dfc_proj_out =
    context.getTensor(wt_idx[AttentionParams::fc_proj_out]);
  Tensor &scores = context.getTensor(wt_idx[AttentionParams::scores]);

  scores.dot_batched_deriv_wrt_1(dvalue, derivative);

  if (!helper_exec)
    calcDerivativeHelper(context, dstate);

  Tensor dfc_tanh = Tensor(fc_out.getDim());
  dfc_tanh.dot_deriv_wrt_1(fc_proj_w, dfc_proj_out);

  Tensor dfc_out;
  tanh.run_prime_fn(fc_tanh, dfc_out, dfc_tanh);
  dquery.dot_deriv_wrt_1(fc_w, dfc_out);
}

void MoLAttentionLayer::calcGradient(RunLayerContext &context) {
  Tensor &query = context.getInput(wt_idx[AttentionParams::query]);
  Tensor &dstate =
    context.getOutgoingDerivative(wt_idx[AttentionParams::state]);

  Tensor &fc_proj_w = context.getWeight(wt_idx[AttentionParams::fc_proj_w]);
  Tensor &dfc_w = context.getWeightGrad(wt_idx[AttentionParams::fc_w]);
  Tensor &dfc_bias = context.getWeightGrad(wt_idx[AttentionParams::fc_bias]);
  Tensor &dfc_proj_w =
    context.getWeightGrad(wt_idx[AttentionParams::fc_proj_w]);
  Tensor &fc_out = context.getTensor(wt_idx[AttentionParams::fc_out]);
  Tensor &fc_tanh = context.getTensor(wt_idx[AttentionParams::fc_tanh]);
  Tensor &dfc_proj_out =
    context.getTensor(wt_idx[AttentionParams::fc_proj_out]);

  if (!helper_exec)
    calcDerivativeHelper(context, dstate);

  Tensor dfc_tanh = Tensor(fc_out.getDim());
  fc_tanh.dot_deriv_wrt_2(dfc_proj_w, dfc_proj_out);
  dfc_tanh.dot_deriv_wrt_1(fc_proj_w, dfc_proj_out);

  Tensor dfc_out;
  tanh.run_prime_fn(fc_tanh, dfc_out, dfc_tanh);
  query.dot_deriv_wrt_2(dfc_w, dfc_out);
  dfc_out.sum({0, 1, 2}, dfc_bias);
}

void MoLAttentionLayer::setProperty(const std::vector<std::string> &values) {
  auto remain_props = loadProperties(values, mol_props);
  AttentionLayer::setProperty(remain_props);
}

void MoLAttentionLayer::setBatch(RunLayerContext &context, unsigned int batch) {
  context.updateTensor(wt_idx[AttentionParams::fc_out], batch);
  context.updateTensor(wt_idx[AttentionParams::fc_proj_out], batch);
  context.updateTensor(wt_idx[AttentionParams::scores], batch);
  context.updateTensor(wt_idx[AttentionParams::prob], batch);
  context.updateTensor(wt_idx[AttentionParams::prob_left], batch);
  context.updateTensor(wt_idx[AttentionParams::prob_right], batch);
  context.updateTensor(wt_idx[AttentionParams::u_neg_div], batch);
  context.updateTensor(wt_idx[AttentionParams::u_pos_div], batch);
}

void MoLAttentionLayer::exportTo(Exporter &exporter,
                                 const ExportMethods &method) const {
  AttentionLayer::exportTo(exporter, method);
  LayerImpl::exportTo(exporter, method);
  exporter.saveResult(mol_props, method, this);
}

} /* namespace nntrainer */