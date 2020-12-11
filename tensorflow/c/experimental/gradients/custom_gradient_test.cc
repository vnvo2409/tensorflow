/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include <memory>

#include "tensorflow/c/eager/abstract_context.h"
#include "tensorflow/c/eager/c_api.h"
#include "tensorflow/c/eager/c_api_unified_experimental.h"
#include "tensorflow/c/eager/c_api_unified_experimental_internal.h"
#include "tensorflow/c/eager/gradients.h"
#include "tensorflow/c/eager/unified_api_testutil.h"
#include "tensorflow/c/experimental/ops/math_ops.h"
#include "tensorflow/c/tf_status_helper.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/test.h"

namespace tensorflow {
namespace gradients {
namespace internal {
namespace {
using std::vector;

int64 ToId(const AbstractTensorHandle* t) {
  return static_cast<int64>(reinterpret_cast<uintptr_t>(t));
}

class CustomGradientTest
    : public ::testing::TestWithParam<std::tuple<const char*, bool, bool>> {
 protected:
  void SetUp() override {
    TF_StatusPtr status(TF_NewStatus());
    TF_SetTracingImplementation(std::get<0>(GetParam()), status.get());
    Status s = StatusFromTF_Status(status.get());
    CHECK_EQ(errors::OK, s.code()) << s.error_message();
  }
};

class PassThroughGradientFunction : public GradientFunction {
 public:
  Status Compute(AbstractContext* ctx,
                 absl::Span<AbstractTensorHandle* const> grad_outputs,
                 absl::Span<AbstractTensorHandle*> grad_inputs) override {
    CHECK_EQ(grad_outputs.size(), 1);
    CHECK_EQ(grad_inputs.size(), 1);
    grad_inputs[0] = grad_outputs[0];
    if (grad_inputs[0]) {
      grad_inputs[0]->Ref();
    }
    return Status::OK();
  }
};

// Computes:
//
// @tf.custom_gradient
// def f(input):
//   def grad(grads):
//     return grads[0]
//   return tf.exp(input), grad
// outputs = [f(inputs[0])]
Status ExpWithPassThroughGrad(AbstractContext* ctx,
                              absl::Span<AbstractTensorHandle* const> inputs,
                              absl::Span<AbstractTensorHandle*> outputs) {
  Tape tape(/*persistent=*/false);
  tape.Watch(inputs[0]);  // Watch x.
  std::vector<AbstractTensorHandle*> exp_outputs(1);
  TF_RETURN_IF_ERROR(ops::Exp(ctx, inputs, absl::MakeSpan(exp_outputs), "Exp"));
  std::unique_ptr<GradientFunction> gradient_function(
      new PassThroughGradientFunction);
  tape.RecordOperation(inputs, exp_outputs, gradient_function.release());
  TF_RETURN_IF_ERROR(tape.ComputeGradient(ctx,
                                          /*targets*/ exp_outputs,
                                          /*sources=*/inputs,
                                          /*output_gradients=*/{},
                                          /*result=*/outputs));
  for (auto exp_output : exp_outputs) {
    exp_output->Unref();
  }
  return Status::OK();
}

TEST_P(CustomGradientTest, ExpWithPassThroughGrad) {
  std::unique_ptr<TF_Status, decltype(&TF_DeleteStatus)> status(
      TF_NewStatus(), TF_DeleteStatus);
  AbstractContextPtr ctx;
  {
    AbstractContext* ctx_raw = nullptr;
    Status s =
        BuildImmediateExecutionContext(std::get<1>(GetParam()), &ctx_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    ctx.reset(ctx_raw);
  }

  AbstractTensorHandlePtr x;
  {
    AbstractTensorHandle* x_raw = nullptr;
    Status s = TestScalarTensorHandle(ctx.get(), 1.0f, &x_raw);
    ASSERT_EQ(errors::OK, s.code()) << s.error_message();
    x.reset(x_raw);
  }

  // Pseudo-code:
  //
  // tape.watch(x)
  // y = exp(x)
  // outputs = tape.gradient(y, x)
  std::vector<AbstractTensorHandle*> outputs(1);
  Status s = RunModel(ExpWithPassThroughGrad, ctx.get(), {x.get()},
                      absl::MakeSpan(outputs),
                      /*use_function=*/!std::get<2>(GetParam()));
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();

  AbstractTensorHandlePtr output_handle(outputs[0]);
  TF_Tensor* result_tensor;
  s = GetValue(outputs[0], &result_tensor);
  std::cout << "TensorId ExpWithPassThroughGrad : " << ToId(outputs[0]) << "\n";
  std::cout << "Tensor Address ExpWithPassThroughGrad : " << outputs[0] << "\n";
  ASSERT_EQ(errors::OK, s.code()) << s.error_message();
  auto result_value = static_cast<float*>(TF_TensorData(result_tensor));
  EXPECT_EQ(*result_value, 1.0);
  // outputs[0]->Unref();
  TF_DeleteTensor(result_tensor);
  result_tensor = nullptr;
}

#ifdef PLATFORM_GOOGLE
INSTANTIATE_TEST_SUITE_P(
    CustomGradientTest, CustomGradientTest,
    ::testing::Combine(::testing::Values("graphdef", "mlir"),
                       /*tfrt*/ ::testing::Values(true, false),
                       /*executing_eagerly*/ ::testing::Values(true, false)));
#else
INSTANTIATE_TEST_SUITE_P(
    CustomGradientTest, CustomGradientTest,
    ::testing::Combine(::testing::Values("graphdef", "mlir"),
                       /*tfrt*/ ::testing::Values(false),
                       /*executing_eagerly*/ ::testing::Values(true, false)));
#endif
}  // namespace
}  // namespace internal
}  // namespace gradients
}  // namespace tensorflow
