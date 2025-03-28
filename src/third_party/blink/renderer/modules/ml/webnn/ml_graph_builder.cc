// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/ml/webnn/ml_graph_builder.h"

#include <algorithm>

#include "base/numerics/checked_math.h"
#include "components/ml/webnn/features.h"
#include "third_party/blink/renderer/bindings/core/v8/script_promise_resolver.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_ml_clamp_options.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_ml_conv_2d_options.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_ml_conv_transpose_2d_options.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_ml_elu_options.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_ml_gemm_options.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_ml_leaky_relu_options.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_ml_operand_descriptor.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_ml_pad_options.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_ml_pool_2d_options.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_ml_reduce_options.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_ml_resample_2d_options.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_ml_split_options.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_ml_transpose_options.h"
#include "third_party/blink/renderer/core/dom/dom_exception.h"
#include "third_party/blink/renderer/modules/ml/buildflags.h"
#include "third_party/blink/renderer/modules/ml/ml_context.h"
#include "third_party/blink/renderer/modules/ml/webnn/ml_activation.h"
#include "third_party/blink/renderer/modules/ml/webnn/ml_graph.h"
#include "third_party/blink/renderer/modules/ml/webnn/ml_graph_utils.h"
#include "third_party/blink/renderer/modules/ml/webnn/ml_operand.h"
#include "third_party/blink/renderer/platform/bindings/exception_state.h"

#if BUILDFLAG(BUILD_WEBNN_WITH_XNNPACK)
#include "third_party/blink/renderer/modules/ml/webnn/ml_graph_xnnpack.h"
#endif

#if BUILDFLAG(BUILD_WEBNN_ON_CROS)
#include "third_party/blink/renderer/modules/ml/webnn/ml_graph_cros.h"
#endif

#if !BUILDFLAG(IS_CHROMEOS)
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/renderer/modules/ml/webnn/ml_graph_mojo.h"
#endif

namespace blink {

namespace {

MLGraphBuilder::BackendForTesting* g_backend_for_testing = nullptr;

bool IsFloatingPointType(V8MLOperandType::Enum operand_type) {
  switch (operand_type) {
    case V8MLOperandType::Enum::kFloat32:
    case V8MLOperandType::Enum::kFloat16:
      return true;
    case V8MLOperandType::Enum::kInt32:
    case V8MLOperandType::Enum::kUint32:
    case V8MLOperandType::Enum::kInt8:
    case V8MLOperandType::Enum::kUint8:
      return false;
  }
}

blink::V8MLOperandType::Enum ComponentOperandTypeToBlink(
    webnn::Operand::DataType type) {
  switch (type) {
    case webnn::Operand::DataType::kFloat32:
      return blink::V8MLOperandType::Enum::kFloat32;
    case webnn::Operand::DataType::kFloat16:
      return blink::V8MLOperandType::Enum::kFloat16;
    case webnn::Operand::DataType::kInt32:
      return blink::V8MLOperandType::Enum::kInt32;
    case webnn::Operand::DataType::kUint32:
      return blink::V8MLOperandType::Enum::kUint32;
    case webnn::Operand::DataType::kInt8:
      return blink::V8MLOperandType::Enum::kInt8;
    case webnn::Operand::DataType::kUint8:
      return blink::V8MLOperandType::Enum::kUint8;
  }
  NOTREACHED_NORETURN();
}

webnn::Operand::DataType BlinkOperandTypeToComponent(
    blink::V8MLOperandType::Enum type) {
  switch (type) {
    case blink::V8MLOperandType::Enum::kFloat32:
      return webnn::Operand::DataType::kFloat32;
    case blink::V8MLOperandType::Enum::kFloat16:
      return webnn::Operand::DataType::kFloat16;
    case blink::V8MLOperandType::Enum::kInt32:
      return webnn::Operand::DataType::kInt32;
    case blink::V8MLOperandType::Enum::kUint32:
      return webnn::Operand::DataType::kUint32;
    case blink::V8MLOperandType::Enum::kInt8:
      return webnn::Operand::DataType::kInt8;
    case blink::V8MLOperandType::Enum::kUint8:
      return webnn::Operand::DataType::kUint8;
  }
  NOTREACHED_NORETURN();
}

webnn::Operand ConvertToComponentOperand(const blink::MLOperand* ml_operand) {
  return webnn::Operand(BlinkOperandTypeToComponent(ml_operand->Type()),
                        ml_operand->Dimensions());
}

webnn::InputOperandLayout BlinkInputOperandLayoutToComponent(
    blink::V8MLInputOperandLayout::Enum type) {
  switch (type) {
    case blink::V8MLInputOperandLayout::Enum::kNchw:
      return webnn::InputOperandLayout::kNchw;
    case blink::V8MLInputOperandLayout::Enum::kNhwc:
      return webnn::InputOperandLayout::kNhwc;
  }
  NOTREACHED_NORETURN();
}

webnn::Conv2dFilterOperandLayout BlinkConv2dFilterLayoutToComponent(
    blink::V8MLConv2dFilterOperandLayout::Enum type) {
  switch (type) {
    case blink::V8MLConv2dFilterOperandLayout::Enum::kOihw:
      return webnn::Conv2dFilterOperandLayout::kOihw;
    case blink::V8MLConv2dFilterOperandLayout::Enum::kHwio:
      return webnn::Conv2dFilterOperandLayout::kHwio;
    case blink::V8MLConv2dFilterOperandLayout::Enum::kOhwi:
      return webnn::Conv2dFilterOperandLayout::kOhwi;
    case blink::V8MLConv2dFilterOperandLayout::Enum::kIhwo:
      return webnn::Conv2dFilterOperandLayout::kIhwo;
  }
  NOTREACHED_NORETURN();
}

webnn::RoundingType BlinkRoundingTypeToComponent(
    blink::V8MLRoundingType::Enum type) {
  switch (type) {
    case blink::V8MLRoundingType::Enum::kFloor:
      return webnn::RoundingType::kFloor;
    case blink::V8MLRoundingType::Enum::kCeil:
      return webnn::RoundingType::kCeil;
  }
  NOTREACHED_NORETURN();
}

base::expected<webnn::Conv2dAttributes, String> ConvertToConv2dAttributes(
    const blink::MLConv2dOptions* options) {
  CHECK(options);
  webnn::Conv2dAttributes attributes;
  // If padding is not present, the values are assumed to be [0,0,0,0].
  auto padding = options->getPaddingOr({0, 0, 0, 0});
  if (padding.size() != 4) {
    return base::unexpected("The length of padding should be 4.");
  }
  // The order of padding array is [beginning_height, ending_height,
  // beginning_width, ending_width].
  attributes.padding = webnn::Padding2d{
      .beginning =
          webnn::Size2d<uint32_t>{.height = padding[0], .width = padding[2]},
      .ending =
          webnn::Size2d<uint32_t>{.height = padding[1], .width = padding[3]}};

  // If strides is not present, the values are assumed to be [1,1].
  auto strides = options->getStridesOr({1, 1});
  if (strides.size() != 2) {
    return base::unexpected("The length of strides should be 2.");
  }
  attributes.strides =
      webnn::Size2d<uint32_t>{.height = strides[0], .width = strides[1]};

  // If dilations is not present, the values are assumed to be [1,1].
  auto dilations = options->getDilationsOr({1, 1});
  if (dilations.size() != 2) {
    return base::unexpected("The length of dilations should be 2.");
  }
  attributes.dilations =
      webnn::Size2d<uint32_t>{.height = dilations[0], .width = dilations[1]};
  attributes.auto_pad = BlinkAutoPadToComponent(options->autoPad().AsEnum());
  attributes.groups = options->groups();
  attributes.input_layout =
      BlinkInputOperandLayoutToComponent(options->inputLayout().AsEnum());
  attributes.filter_layout =
      BlinkConv2dFilterLayoutToComponent(options->filterLayout().AsEnum());
  if (options->hasBias()) {
    attributes.bias_operand = ConvertToComponentOperand(options->bias());
  }
  return attributes;
}

base::expected<webnn::Pool2dAttributes, std::string> ConvertToPool2dAttributes(
    const blink::MLPool2dOptions* options) {
  CHECK(options);
  webnn::Pool2dAttributes attributes;
  if (options->hasWindowDimensions()) {
    auto& window_dimensions = options->windowDimensions();
    if (window_dimensions.size() != 2) {
      return base::unexpected("The length of window dimensions should be 2.");
    }
    attributes.window_dimensions = webnn::Size2d<uint32_t>{
        .height = window_dimensions[0], .width = window_dimensions[1]};
  }

  // If padding is not present, the values are assumed to be [0,0,0,0].
  auto padding = options->getPaddingOr({0, 0, 0, 0});
  if (padding.size() != 4) {
    return base::unexpected("The length of padding should be 4.");
  }
  attributes.padding = webnn::Padding2d{
      .beginning =
          webnn::Size2d<uint32_t>{.height = padding[0], .width = padding[2]},
      .ending =
          webnn::Size2d<uint32_t>{.height = padding[1], .width = padding[3]}};

  // If strides is not present, the values are assumed to be [1,1].
  auto strides = options->getStridesOr({1, 1});
  if (strides.size() != 2) {
    return base::unexpected("The length of strides should be 2.");
  }
  attributes.strides =
      webnn::Size2d<uint32_t>{.height = strides[0], .width = strides[1]};

  // If dilations is not present, the values are assumed to be [1,1].
  auto dilations = options->getDilationsOr({1, 1});
  if (dilations.size() != 2) {
    return base::unexpected("The length of dilations should be 2.");
  }
  attributes.dilations =
      webnn::Size2d<uint32_t>{.height = dilations[0], .width = dilations[1]};
  attributes.auto_pad = BlinkAutoPadToComponent(options->autoPad().AsEnum());
  attributes.layout =
      BlinkInputOperandLayoutToComponent(options->layout().AsEnum());
  attributes.rounding_type =
      BlinkRoundingTypeToComponent(options->roundingType().AsEnum());
  if (options->hasOutputSizes()) {
    // TODO(ningxin.hu@intel.com): report a DevTools warning message if rounding
    // type is provided but ignored.
    auto& output_size = options->outputSizes();
    if (output_size.size() != 2) {
      return base::unexpected("The length of output sizes should be 2.");
    }
    attributes.output_sizes = webnn::Size2d<uint32_t>{.height = output_size[0],
                                                      .width = output_size[1]};
  }
  return attributes;
}

webnn::GemmAttributes ConvertToGemmAttributes(
    const blink::MLGemmOptions* options) {
  CHECK(options);
  webnn::GemmAttributes attributes;
  if (options->hasC()) {
    attributes.c_operand = ConvertToComponentOperand(options->c());
  }
  attributes.alpha = options->alpha();
  attributes.beta = options->beta();
  attributes.a_transpose = options->aTranspose();
  attributes.b_transpose = options->bTranspose();
  return attributes;
}

bool ValidateClampOptions(const MLClampOptions* options,
                          ExceptionState& exception_state) {
  // The generated code of MLClampOptions uses blink::ToRestrictedFloat to
  // convert the min/max value to a single precision float. It will throw on
  // non-finite values.
  if (options->hasMinValue() && options->hasMaxValue()) {
    if (options->minValue() > options->maxValue()) {
      exception_state.ThrowDOMException(
          DOMExceptionCode::kDataError,
          String::Format("The min value (%f) should be less than or equal to "
                         "the max value (%f).",
                         options->minValue(), options->maxValue()));
      return false;
    }
  }
  return true;
}

absl::optional<Vector<uint32_t>> BroadcastShapes(
    const Vector<uint32_t>& dims_lhs,
    const Vector<uint32_t>& dims_rhs,
    bool bidirectional = true) {
  auto output_shape = webnn::BroadcastShapes(
      base::make_span(dims_lhs), base::make_span(dims_rhs), bidirectional);
  if (!output_shape) {
    return absl::nullopt;
  }
  return Vector<uint32_t>(output_shape.value());
}

MLOperand* BuildElementWiseBinary(MLGraphBuilder* builder,
                                  MLOperator::OperatorKind kind,
                                  const MLOperand* a,
                                  const MLOperand* b,
                                  ExceptionState& exception_state) {
  if (a->Type() != b->Type()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      "The input types don't match.");
    return nullptr;
  }
  absl::optional<Vector<uint32_t>> dims_output =
      BroadcastShapes(a->Dimensions(), b->Dimensions());
  if (!dims_output) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        "The input shapes are not broadcastable.");
    return nullptr;
  }
  auto* binary = MakeGarbageCollected<MLOperator>(builder, kind);
  auto output = MLOperand::ValidateAndCreateOutput(builder, a->Type(),
                                                   dims_output.value(), binary);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  binary->Connect({a, b}, {output.value()});
  return output.value();
}

MLOperand* BuildElementWiseUnary(MLGraphBuilder* builder,
                                 MLOperator::OperatorKind kind,
                                 const MLOperand* input,
                                 ExceptionState& exception_state) {
  // The input type must be one of the floating point types. Although this
  // constraint is not specified in current WebNN spec, there is a feature
  // request for that: https://github.com/webmachinelearning/webnn/issues/283
  if (!IsFloatingPointType(input->Type())) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        "The input type must be one of the floating point types.");
    return nullptr;
  }
  // According to WebNN spec:
  // https://www.w3.org/TR/webnn/#api-mlgraphbuilder-unary, the shape of the
  // output tensor is the same as the shape of input tensor.
  Vector<uint32_t> dims_output = input->Dimensions();
  auto* unary = MakeGarbageCollected<MLOperator>(builder, kind);
  auto output = MLOperand::ValidateAndCreateOutput(builder, input->Type(),
                                                   dims_output, unary);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  unary->Connect({input}, {output.value()});
  return output.value();
}

MLOperand* BuildReduce(MLGraphBuilder* builder,
                       MLOperator::OperatorKind kind,
                       const MLOperand* input,
                       const MLReduceOptions* options,
                       ExceptionState& exception_state) {
  // According to WebNN spec:
  // https://www.w3.org/TR/webnn/#api-mlgraphbuilder-reduce,
  // When axes is not specified, it’s set to [0, ..., N-1], where N is
  // the rank of the input tensor.
  const auto input_rank = input->Dimensions().size();
  Vector<uint32_t> default_axes(input_rank);
  for (wtf_size_t i = 0; i < input_rank; i++) {
    default_axes[i] = i;
  }
  const auto axes = options->getAxesOr(std::move(default_axes));
  auto validation_result = webnn::ValidateAxes(axes, input_rank);
  if (!validation_result.has_value()) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        String::FromUTF8(validation_result.error()));
    return nullptr;
  }

  Vector<uint32_t> output_shape;
  if (options->keepDimensions()) {
    output_shape = input->Dimensions();
    for (auto axis : axes) {
      output_shape[axis] = 1;
    }
  } else {
    for (wtf_size_t i = 0; i < input_rank; i++) {
      if (!axes.Contains(i)) {
        output_shape.push_back(input->Dimensions()[i]);
      }
    }
  }

  // Currently, WebNN doesn't support using empty dimensions to represent a
  // scalar. An issue has been filed to track it -
  // https://github.com/webmachinelearning/webnn/issues/390. As a workaround, we
  // set output_shape to {1} to represent a scalar output.
  if (output_shape.size() == 0) {
    output_shape.push_back(1);
  }

  auto* reduce = MakeGarbageCollected<MLOperator>(builder, kind, options);
  auto output = MLOperand::ValidateAndCreateOutput(builder, input->Type(),
                                                   output_shape, reduce);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  reduce->Connect({input}, {output.value()});
  return output.value();
}

MLOperand* BuildPool2d(MLGraphBuilder* builder,
                       MLOperator::OperatorKind kind,
                       const MLOperand* input,
                       const MLPool2dOptions* options,
                       ExceptionState& exception_state) {
  auto pool2d_attributes = ConvertToPool2dAttributes(options);
  if (!pool2d_attributes.has_value()) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        WTF::String::FromUTF8(pool2d_attributes.error()));
    return nullptr;
  }

  auto validated_output = webnn::ValidatePool2dAndInferOutput(
      ConvertToComponentOperand(input), std::move(pool2d_attributes.value()));
  if (!validated_output.has_value()) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        WTF::String::FromUTF8(validated_output.error()));
    return nullptr;
  }
  // Create pool2d operator and its output operand. Connect the pool2d operator
  // to its input and output operands.
  auto* pool2d = MakeGarbageCollected<MLOperator>(builder, kind, options);
  auto output = MLOperand::ValidateAndCreateOutput(
      builder, input->Type(), Vector<uint32_t>(validated_output->dimensions),
      pool2d);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  pool2d->Connect({input}, {output.value()});
  return output.value();
}

// The current WebNN spec doesn't define the calculation formula of the output
// size for resample2d. An issue has been filed to track it -
// https://github.com/webmachinelearning/webnn/issues/360.
base::expected<uint32_t, String> CalculateResample2dOutputSize(
    const uint32_t input_size,
    const float scale) {
  // Calculate the output size in double precision floating point number that
  // ensures values of type uint32_t can be exactly represented.
  // https://en.wikipedia.org/wiki/Double-precision_floating-point_format#Precision_limitations_on_integer_values
  // The max value of checked_output_size should be 3 * UINT_MAX + 1,
  // which is smaller than the max safe integer value for double type.
  auto checked_output_size = base::MakeCheckedNum<double>(input_size) * scale;

  // Check if the value is valid for rounding to uint32_t type.
  if (!checked_output_size.IsValid<uint32_t>()) {
    return base::unexpected("The scale is too large.");
  }
  const uint32_t output_size =
      base::ClampFloor<uint32_t>(double(checked_output_size.ValueOrDie()));
  if (output_size == 0) {
    return base::unexpected("The scale is too small.");
  }
  return output_size;
}
}  // namespace

// static
MLGraphBuilder* MLGraphBuilder::Create(MLContext* context) {
  return MakeGarbageCollected<MLGraphBuilder>(context);
}

MLGraphBuilder::MLGraphBuilder(MLContext* context) : ml_context_(context) {}

MLGraphBuilder::~MLGraphBuilder() = default;

void MLGraphBuilder::Trace(Visitor* visitor) const {
  visitor->Trace(ml_context_);
  ScriptWrappable::Trace(visitor);
}

MLContext* MLGraphBuilder::GetContext() const {
  return ml_context_.Get();
}

// static
absl::optional<webnn::PaddingSizes>
MLGraphBuilder::CalculateConvTransposed2dPadding(
    V8MLAutoPad::Enum auto_pad,
    const uint32_t input_size,
    const uint32_t filter_size,
    const uint32_t stride,
    const uint32_t dilation,
    const uint32_t output_padding) {
  auto checked_output_size =
      base::MakeCheckedNum<uint32_t>(input_size) * stride;
  auto checked_effective_filter_size =
      (base::MakeCheckedNum<uint32_t>(filter_size) - 1) * dilation + 1;
  auto checked_total_padding = stride * (input_size - 1) +
                               checked_effective_filter_size + output_padding -
                               checked_output_size;
  base::CheckedNumeric<uint32_t> checked_padding_begin, checked_padding_end;
  switch (auto_pad) {
    case V8MLAutoPad::Enum::kSameUpper:
      checked_padding_begin = checked_total_padding / 2;
      checked_padding_end = (checked_total_padding + 1) / 2;
      break;
    case V8MLAutoPad::Enum::kSameLower:
      checked_padding_begin = (checked_total_padding + 1) / 2;
      checked_padding_end = checked_total_padding / 2;
      break;
    case V8MLAutoPad::Enum::kExplicit:
      // The case has been ruled out before the function be called.
      NOTREACHED_NORETURN()
          << "Invalid auto pad value when calculating convTranspose2d padding.";
  }
  uint32_t padding_begin, padding_end;
  if (!checked_padding_begin.AssignIfValid(&padding_begin) ||
      !checked_padding_end.AssignIfValid(&padding_end)) {
    return absl::nullopt;
  }
  return webnn::PaddingSizes({.begin = padding_begin, .end = padding_end});
}

// Calculate the output size for convTranspose2d based on WebNN spec:
// https://www.w3.org/TR/webnn/#api-mlgraphbuilder-convtranspose2d
// Return the calculated output size if no error.
base::expected<uint32_t, String> CalculateConvTranspose2dOutputSize(
    const uint32_t input_size,
    const uint32_t filter_size,
    const uint32_t beginning_padding,
    const uint32_t ending_padding,
    const uint32_t stride,
    const uint32_t dilation,
    const uint32_t output_padding) {
  // Calculate the dilated filter sizes.
  auto checked_effective_filter_size =
      (base::MakeCheckedNum<uint32_t>(filter_size) - 1) * dilation + 1;
  if (!checked_effective_filter_size.IsValid()) {
    return base::unexpected("The effective filter size is too large.");
  }
  auto checked_output_size =
      (base::MakeCheckedNum<uint32_t>(input_size) - 1) * stride +
      checked_effective_filter_size - beginning_padding - ending_padding +
      output_padding;
  // Check if the checked_output_size is valid.
  if (!checked_output_size.IsValid()) {
    return base::unexpected(
        "The stride is too large or the input size is to small for padding.");
  }

  return checked_output_size.ValueOrDie();
}

// static
base::expected<MLGraphBuilder::Size2D, String>
MLGraphBuilder::ValidateAndCalculateConvTranspose2dOutputSizes(
    const uint32_t input_height,
    const uint32_t input_width,
    const uint32_t filter_height,
    const uint32_t filter_width,
    const Vector<uint32_t>& padding,
    const Vector<uint32_t>& strides,
    const Vector<uint32_t>& dilations,
    const Vector<uint32_t>& output_padding,
    const V8MLAutoPad auto_pad) {
  // Validate padding and get its values.
  if (padding.size() != 4) {
    return base::unexpected("The length of padding should be 4.");
  }
  uint32_t padding_beginning_height = padding[0];
  uint32_t padding_ending_height = padding[1];
  uint32_t padding_beginning_width = padding[2];
  uint32_t padding_ending_width = padding[3];

  // Validate strides and get its values.
  if (strides.size() != 2) {
    return base::unexpected("The length of strides should be 2.");
  }
  if (base::ranges::any_of(strides, [](uint32_t x) { return x == 0; })) {
    return base::unexpected("All strides should be greater than 0.");
  }
  const uint32_t stride_height = strides[0];
  const uint32_t stride_width = strides[1];

  // Validate dilations and get its values.
  if (dilations.size() != 2) {
    return base::unexpected("The length of dilations should be 2.");
  }
  if (base::ranges::any_of(dilations, [](uint32_t x) { return x == 0; })) {
    return base::unexpected("All dilations should be greater than 0.");
  }
  const uint32_t dilation_height = dilations[0];
  const uint32_t dilation_width = dilations[1];

  // Validate output padding and get its values.
  if (output_padding.size() != 2) {
    return base::unexpected("The length of outputPadding should be 2.");
  }
  const uint32_t outputPadding_height = output_padding[0];
  const uint32_t outputPadding_width = output_padding[1];
  if (outputPadding_height >= stride_height ||
      outputPadding_width >= stride_width) {
    return base::unexpected(
        "The output padding must be smaller than the stride along the same "
        "dimension.");
  }

  // When the autoPad is other than "explicit", the values in the
  // options.padding array are ignored and the explicit padding values need to
  // be calculated.
  if (auto_pad != V8MLAutoPad::Enum::kExplicit) {
    auto padding_sizes_height =
        MLGraphBuilder::CalculateConvTransposed2dPadding(
            auto_pad.AsEnum(), input_height, filter_height, stride_height,
            dilation_height, outputPadding_height);
    if (!padding_sizes_height) {
      return base::unexpected(
          "Overflow occurred when calculating the padding along the height "
          "dimension.");
    }
    padding_beginning_height = padding_sizes_height->begin;
    padding_ending_height = padding_sizes_height->end;
    auto padding_sizes_width = MLGraphBuilder::CalculateConvTransposed2dPadding(
        auto_pad.AsEnum(), input_width, filter_width, stride_width,
        dilation_width, outputPadding_width);
    if (!padding_sizes_width) {
      return base::unexpected(
          "Overflow occurred when calculating the padding along the width "
          "dimension.");
    }
    padding_beginning_width = padding_sizes_width->begin;
    padding_ending_width = padding_sizes_width->end;
  }

  auto output_height = CalculateConvTranspose2dOutputSize(
      input_height, filter_height, padding_beginning_height,
      padding_ending_height, stride_height, dilation_height,
      outputPadding_height);
  if (!output_height.has_value()) {
    return base::unexpected("Failed to calculate the output height: " +
                            output_height.error());
  }

  auto output_width = CalculateConvTranspose2dOutputSize(
      input_width, filter_width, padding_beginning_width, padding_ending_width,
      stride_width, dilation_width, outputPadding_width);
  if (!output_width.has_value()) {
    return base::unexpected("Failed to calculate the output width: " +
                            output_width.error());
  }

  return Size2D(
      {.height = output_height.value(), .width = output_width.value()});
}

MLOperand* MLGraphBuilder::input(String name,
                                 const MLOperandDescriptor* desc,
                                 ExceptionState& exception_state) {
  // If no dimensions, it represents a scalar. Set dimensions to {1}.
  Vector<uint32_t> dimensions = desc->getDimensionsOr({1});
  auto input_operand = MLOperand::ValidateAndCreateInput(
      this, desc->type().AsEnum(), std::move(dimensions), std::move(name));
  if (!input_operand.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      input_operand.error());
    return nullptr;
  }
  return input_operand.value();
}

MLOperand* MLGraphBuilder::constant(const MLOperandDescriptor* desc,
                                    NotShared<DOMArrayBufferView> buffer_view,
                                    ExceptionState& exception_state) {
  String error_message;
  // If no dimensions, it represents a scalar. Set dimensions to {1}.
  Vector<uint32_t> dimensions = desc->getDimensionsOr({1});
  auto constant_operand = MLOperand::ValidateAndCreateConstant(
      this, desc->type().AsEnum(), std::move(dimensions), buffer_view.Get());
  if (!constant_operand.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      constant_operand.error());
    return nullptr;
  }
  return constant_operand.value();
}

MLOperand* MLGraphBuilder::concat(const HeapVector<Member<MLOperand>>& inputs,
                                  const uint32_t axis,
                                  ExceptionState& exception_state) {
  std::vector<webnn::Operand> input_component_operands;
  input_component_operands.reserve(inputs.size());
  base::ranges::transform(
      inputs, std::back_inserter(input_component_operands),
      [](const auto& input) { return ConvertToComponentOperand(input); });

  auto validated_output =
      webnn::ValidateConcatAndInferOutput(input_component_operands, axis);
  if (!validated_output.has_value()) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        String::FromUTF8(validated_output.error()));
    return nullptr;
  }

  auto* concat = MakeGarbageCollected<MLConcatOperator>(this, axis);
  auto output = MLOperand::ValidateAndCreateOutput(
      this, ComponentOperandTypeToBlink(validated_output->data_type),
      Vector<uint32_t>(validated_output->dimensions), concat);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  concat->Connect((HeapVector<Member<const MLOperand>>)inputs,
                  {output.value()});
  return output.value();
}

MLOperand* MLGraphBuilder::clamp(const MLOperand* input,
                                 const MLClampOptions* options,
                                 ExceptionState& exception_state) {
  if (!ValidateClampOptions(options, exception_state)) {
    return nullptr;
  }
  auto* clamp = MakeGarbageCollected<MLOperator>(
      this, MLOperator::OperatorKind::kClamp, options);
  // According to WebNN spec
  // https://www.w3.org/TR/webnn/#api-mlgraphbuilder-clamp, the output tensor of
  // clamp has the same type and dimensions as its input.
  auto output = MLOperand::ValidateAndCreateOutput(this, input->Type(),
                                                   input->Dimensions(), clamp);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  clamp->Connect({input}, {output.value()});
  return output.value();
}

MLActivation* MLGraphBuilder::clamp(const MLClampOptions* options,
                                    ExceptionState& exception_state) {
  if (!ValidateClampOptions(options, exception_state)) {
    return nullptr;
  }
  // Create the clamp operator that would be used as an activation function.
  return MakeGarbageCollected<MLActivation>(
      this, MLOperator::OperatorKind::kClamp, options);
}

MLOperand* MLGraphBuilder::conv2d(const MLOperand* input,
                                  const MLOperand* filter,
                                  const MLConv2dOptions* options,
                                  ExceptionState& exception_state) {
  auto conv2d_attributes = ConvertToConv2dAttributes(options);
  if (!conv2d_attributes.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      conv2d_attributes.error());
    return nullptr;
  }

  auto validated_output = webnn::ValidateConv2dAndInferOutput(
      ConvertToComponentOperand(input), ConvertToComponentOperand(filter),
      std::move(conv2d_attributes.value()));
  if (!validated_output.has_value()) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        WTF::String::FromUTF8(validated_output.error()));
    return nullptr;
  }
  // Create conv2d operator and its output operand. Connect the conv2d operator
  // to its input and output operands.
  auto* conv2d = MakeGarbageCollected<MLOperator>(
      this, MLOperator::OperatorKind::kConv2d, options);
  HeapVector<Member<const MLOperand>> inputs = {input, filter};
  if (options->hasBias()) {
    inputs.push_back(options->bias());
  }
  auto output = MLOperand::ValidateAndCreateOutput(
      this, ComponentOperandTypeToBlink(validated_output.value().data_type),
      Vector<uint32_t>(validated_output.value().dimensions), conv2d);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  conv2d->Connect(std::move(inputs), {output.value()});
  return output.value();
}

MLOperand* MLGraphBuilder::convTranspose2d(
    const MLOperand* input,
    const MLOperand* filter,
    const MLConvTranspose2dOptions* options,
    ExceptionState& exception_state) {
  // Validate input operand and set its sizes.
  const auto input_shape = input->Dimensions();
  if (input_shape.size() != 4) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      "The input should be a 4-D tensor.");
    return nullptr;
  }
  // The input layout option specifies the layout format of the input tensor.
  uint32_t input_batches, input_channels, input_height, input_width;
  switch (options->inputLayout().AsEnum()) {
    case V8MLInputOperandLayout::Enum::kNchw:
      // "nchw": [batches, input_channels, height, width]
      input_batches = input_shape[0];
      input_channels = input_shape[1];
      input_height = input_shape[2];
      input_width = input_shape[3];
      break;
    case V8MLInputOperandLayout::Enum::kNhwc:
      // "nhwc": [batches, height, width, input_channels]
      input_batches = input_shape[0];
      input_height = input_shape[1];
      input_width = input_shape[2];
      input_channels = input_shape[3];
      break;
  }

  // Validate filter operand and set its sizes.
  if (filter->Type() != input->Type()) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        "The filter type doesn't match the input type.");
    return nullptr;
  }
  const auto filter_shape = filter->Dimensions();
  if (filter_shape.size() != 4) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      "The filter should be a 4-D tensor.");
    return nullptr;
  }
  // The filter layout specifies the filter layout format.
  uint32_t filter_height, filter_width, output_channels, filter_input_channels;
  switch (options->filterLayout().AsEnum()) {
    case V8MLConvTranspose2dFilterOperandLayout::Enum::kHwoi:
      // "hwoi": [height, width, output_channels, input_channels/groups]
      filter_height = filter_shape[0];
      filter_width = filter_shape[1];
      output_channels = filter_shape[2];
      filter_input_channels = filter_shape[3];
      break;
    case V8MLConvTranspose2dFilterOperandLayout::Enum::kOhwi:
      // "ohwi": [output_channels, height, width, input_channels/groups]
      output_channels = filter_shape[0];
      filter_height = filter_shape[1];
      filter_width = filter_shape[2];
      filter_input_channels = filter_shape[3];
      break;
    case V8MLConvTranspose2dFilterOperandLayout::Enum::kIohw:
      // "iohw": [input_channels/groups, output_channels, height, width]
      filter_input_channels = filter_shape[0];
      output_channels = filter_shape[1];
      filter_height = filter_shape[2];
      filter_width = filter_shape[3];
      break;
  }
  // Validate bias operand if it is present.
  if (options->hasBias()) {
    const auto bias_shape = options->bias()->Dimensions();
    if (bias_shape.size() != 1) {
      exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                        "The bias should be a 1-D tensor.");
      return nullptr;
    }
    if (bias_shape[0] != output_channels) {
      exception_state.ThrowDOMException(
          DOMExceptionCode::kDataError,
          String::Format("The bias shape should be [%u].", output_channels));
      return nullptr;
    }
    if (options->bias()->Type() != input->Type()) {
      exception_state.ThrowDOMException(
          DOMExceptionCode::kDataError,
          "The bias type doesn't match input type.");
      return nullptr;
    }
  }
  // Validate groups.
  if (options->groups() == 0) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      "The groups should be greater than 0.");
    return nullptr;
  }
  if (input_channels % options->groups() != 0 ||
      filter_input_channels != input_channels / options->groups()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      "The groups must evenly divide the input "
                                      "channels to filter input channels.");
    return nullptr;
  }

  // Validate and calculate output sizes.
  uint32_t output_height, output_width;
  if (options->hasOutputSizes()) {
    const auto output_sizes = options->getOutputSizesOr({});
    if (output_sizes.size() != 2) {
      exception_state.ThrowDOMException(
          DOMExceptionCode::kDataError,
          "The length of outputSizes should be 2.");
      return nullptr;
    }
    output_height = output_sizes[0];
    output_width = output_sizes[1];
    if (output_height == 0 || output_width == 0) {
      exception_state.ThrowDOMException(
          DOMExceptionCode::kDataError,
          "All output sizes should be greater than 0.");
      return nullptr;
    }
    // If strides is not present, the values are assumed to be [1,1].
    const auto strides = options->getStridesOr({1, 1});
    const auto calculated_output_sizes =
        ValidateAndCalculateConvTranspose2dOutputSizes(
            input_height, input_width, filter_height, filter_width,
            // If padding is not present, the values are assumed to be
            // [0,0,0,0].
            options->getPaddingOr({0, 0, 0, 0}), strides,
            // If dilations is not present, the values are assumed to be [1, 1].
            options->getDilationsOr({1, 1}),
            // Calculate the output sizes without the output padding.
            {0, 0}, options->autoPad());
    if (!calculated_output_sizes.has_value()) {
      exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                        calculated_output_sizes.error());
      return nullptr;
    }
    auto calculated_output_height = calculated_output_sizes->height;
    auto calculated_output_width = calculated_output_sizes->width;
    if (output_height < calculated_output_height ||
        output_height >= calculated_output_height + strides[0]) {
      exception_state.ThrowDOMException(
          DOMExceptionCode::kDataError,
          "The height of output sizes is invalid.");
      return nullptr;
    }
    if (output_width < calculated_output_width ||
        output_width >= calculated_output_width + strides[1]) {
      exception_state.ThrowDOMException(
          DOMExceptionCode::kDataError,
          "The width of output sizes is invalid.");
      return nullptr;
    }
    ml_context_->LogConsoleWarning(
        "When output sizes are specified, output padding argument is ignored");
  } else {
    const auto output_sizes = ValidateAndCalculateConvTranspose2dOutputSizes(
        input_height, input_width, filter_height, filter_width,
        // If padding is not present, the values are assumed to be [0,0,0,0].
        options->getPaddingOr({0, 0, 0, 0}),
        // If strides is not present, the values are assumed to be [1,1].
        options->getStridesOr({1, 1}),
        // If dilations is not present, the values are assumed to be [1, 1].
        options->getDilationsOr({1, 1}),
        // If outputPadding is not present, the values are assumed to be [0, 0].
        options->getOutputPaddingOr({0, 0}), options->autoPad());
    if (!output_sizes.has_value()) {
      exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                        output_sizes.error());
      return nullptr;
    }
    output_height = output_sizes->height;
    output_width = output_sizes->width;
  }
  // The input layout option specifies the layout format of the output tensor.
  Vector<uint32_t> output_shape;
  switch (options->inputLayout().AsEnum()) {
    case V8MLInputOperandLayout::Enum::kNchw:
      // "nchw": [batches, output_channels, height, width]
      output_shape = {input_batches, output_channels, output_height,
                      output_width};
      break;
    case V8MLInputOperandLayout::Enum::kNhwc:
      // "nhwc": [batches, height, width, output_channels]
      output_shape = {input_batches, output_height, output_width,
                      output_channels};
      break;
  }
  // Create convTranspose2d operator and its output operand. Connect the
  // convTranspose2d operator to its input and output operands.
  auto* convTranspose2d = MakeGarbageCollected<MLOperator>(
      this, MLOperator::OperatorKind::kConvTranspose2d, options);
  HeapVector<Member<const MLOperand>> inputs = {input, filter};
  if (options->hasBias()) {
    inputs.push_back(options->bias());
  }
  auto output = MLOperand::ValidateAndCreateOutput(
      this, input->Type(), std::move(output_shape), convTranspose2d);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  convTranspose2d->Connect(std::move(inputs), {output.value()});
  return output.value();
}

#define BUILD_ELEMENTWISE_BINARY_OP(op, op_kind)                              \
  MLOperand* MLGraphBuilder::op(const MLOperand* a, const MLOperand* b,       \
                                ExceptionState& exception_state) {            \
    return BuildElementWiseBinary(this, MLOperator::OperatorKind::op_kind, a, \
                                  b, exception_state);                        \
  }

BUILD_ELEMENTWISE_BINARY_OP(add, kAdd)
BUILD_ELEMENTWISE_BINARY_OP(sub, kSub)
BUILD_ELEMENTWISE_BINARY_OP(mul, kMul)
BUILD_ELEMENTWISE_BINARY_OP(div, kDiv)
BUILD_ELEMENTWISE_BINARY_OP(min, kMin)
BUILD_ELEMENTWISE_BINARY_OP(max, kMax)
BUILD_ELEMENTWISE_BINARY_OP(pow, kPow)

#define BUILD_ELEMENTWISE_UNARY_OP(op, op_kind)                           \
  MLOperand* MLGraphBuilder::op(const MLOperand* input,                   \
                                ExceptionState& exception_state) {        \
    return BuildElementWiseUnary(this, MLOperator::OperatorKind::op_kind, \
                                 input, exception_state);                 \
  }

BUILD_ELEMENTWISE_UNARY_OP(abs, kAbs)
BUILD_ELEMENTWISE_UNARY_OP(ceil, kCeil)
BUILD_ELEMENTWISE_UNARY_OP(floor, kFloor)
BUILD_ELEMENTWISE_UNARY_OP(neg, kNeg)

#define BUILD_REDUCE_OP(op, op_kind)                                   \
  MLOperand* MLGraphBuilder::op(const MLOperand* input,                \
                                const MLReduceOptions* options,        \
                                ExceptionState& exception_state) {     \
    return BuildReduce(this, MLOperator::OperatorKind::op_kind, input, \
                       options, exception_state);                      \
  }

BUILD_REDUCE_OP(reduceSum, kReduceSum)
BUILD_REDUCE_OP(reduceMean, kReduceMean)

MLOperand* MLGraphBuilder::elu(const MLOperand* input,
                               const MLEluOptions* options,
                               ExceptionState& exception_state) {
  // The current spec doesn't specify the operand type constraints of elu. An
  // issue has been filed to track it:
  // https://github.com/webmachinelearning/webnn/issues/283.
  if (!IsFloatingPointType(input->Type())) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        "The type of input must be one of the floating point types.");
    return nullptr;
  }
  // The current spec doesn't restrict the value of alpha. An issue has been
  // filed to track it: https://github.com/webmachinelearning/webnn/issues/383
  if (options->alpha() <= 0.0f) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        "The value of alpha must be greater than 0.");
    return nullptr;
  }
  auto* elu = MakeGarbageCollected<MLOperator>(
      this, MLOperator::OperatorKind::kElu, options);
  // According to WebNN spec
  // https://www.w3.org/TR/webnn/#api-mlgraphbuilder-elu, the output tensor of
  // elu has the same type and dimensions as its input.
  auto output = MLOperand::ValidateAndCreateOutput(this, input->Type(),
                                                   input->Dimensions(), elu);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  elu->Connect({input}, {output.value()});
  return output.value();
}

MLActivation* MLGraphBuilder::elu(const MLEluOptions* options,
                                  ExceptionState& exception_state) {
  // Create the elu operator that would be used as an activation
  // function.
  return MakeGarbageCollected<MLActivation>(
      this, MLOperator::OperatorKind::kElu, options);
}

MLOperand* MLGraphBuilder::gemm(const MLOperand* a,
                                const MLOperand* b,
                                const MLGemmOptions* options,
                                ExceptionState& exception_state) {
  auto validated_output = webnn::ValidateGemmAndInferOutput(
      ConvertToComponentOperand(a), ConvertToComponentOperand(b),
      ConvertToGemmAttributes(options));
  if (!validated_output.has_value()) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        WTF::String::FromUTF8(validated_output.error()));
    return nullptr;
  }
  auto* gemm = MakeGarbageCollected<MLOperator>(
      this, MLOperator::OperatorKind::kGemm, options);
  HeapVector<Member<const MLOperand>> inputs = {a, b};
  if (options->hasC()) {
    inputs.push_back(options->c());
  }
  auto output = MLOperand::ValidateAndCreateOutput(
      this, ComponentOperandTypeToBlink(validated_output.value().data_type),
      Vector<uint32_t>(validated_output.value().dimensions), gemm);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  gemm->Connect(std::move(inputs), {output.value()});
  return output.value();
}

MLOperand* MLGraphBuilder::hardSwish(const MLOperand* input,
                                     ExceptionState& exception_state) {
  // The input type must be one of the floating point types. Although this
  // constraint is not specified in current WebNN spec, there is a feature
  // request for that: https://github.com/webmachinelearning/webnn/issues/283
  if (!IsFloatingPointType(input->Type())) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        "The input type must be one of the floating point types.");
    return nullptr;
  }
  auto* hard_swish = MakeGarbageCollected<MLOperator>(
      this, MLOperator::OperatorKind::kHardSwish);
  // According to WebNN spec
  // https://www.w3.org/TR/webnn/#api-mlgraphbuilder-hard-swish, the output
  // tensor of hard-swish has the same type and dimensions as its input.
  auto output = MLOperand::ValidateAndCreateOutput(
      this, input->Type(), input->Dimensions(), hard_swish);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  hard_swish->Connect({input}, {output.value()});
  return output.value();
}

MLActivation* MLGraphBuilder::hardSwish(ExceptionState& exception_state) {
  // Create the hard-swish operator that would be used as an activation
  // function.
  return MakeGarbageCollected<MLActivation>(
      this, MLOperator::OperatorKind::kHardSwish);
}

MLOperand* MLGraphBuilder::leakyRelu(const MLOperand* input,
                                     const MLLeakyReluOptions* options,
                                     ExceptionState& exception_state) {
  auto* leaky_relu = MakeGarbageCollected<MLOperator>(
      this, MLOperator::OperatorKind::kLeakyRelu, options);
  // According to WebNN spec
  // https://www.w3.org/TR/webnn/#api-mlgraphbuilder-relu, the output tensor of
  // relu has the same type and dimensions as its input.
  auto output = MLOperand::ValidateAndCreateOutput(
      this, input->Type(), input->Dimensions(), leaky_relu);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  leaky_relu->Connect({input}, {output.value()});
  return output.value();
}

MLActivation* MLGraphBuilder::leakyRelu(const MLLeakyReluOptions* options,
                                        ExceptionState& exception_state) {
  // Create the leakyRelu operator that would be used as an activation
  // function.
  return MakeGarbageCollected<MLActivation>(
      this, MLOperator::OperatorKind::kLeakyRelu, options);
}

MLOperand* MLGraphBuilder::matmul(const MLOperand* a,
                                  const MLOperand* b,
                                  ExceptionState& exception_state) {
  auto validated_output = webnn::ValidateMatmulAndInferOutput(
      ConvertToComponentOperand(a), ConvertToComponentOperand(b));
  if (!validated_output.has_value()) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        WTF::String::FromUTF8(validated_output.error()));
    return nullptr;
  }
  // Create matmul operator and its output operand. Connect the matmul operator
  // to its input and output operands.
  auto* matmul =
      MakeGarbageCollected<MLOperator>(this, MLOperator::OperatorKind::kMatmul);
  auto output = MLOperand::ValidateAndCreateOutput(
      this, ComponentOperandTypeToBlink(validated_output.value().data_type),
      Vector<uint32_t>(validated_output.value().dimensions), matmul);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  HeapVector<Member<const MLOperand>> inputs = {a, b};
  matmul->Connect(std::move(inputs), {output.value()});
  return output.value();
}

MLOperand* MLGraphBuilder::pad(const MLOperand* input,
                               const Vector<uint32_t>& beginning_padding,
                               const Vector<uint32_t>& ending_padding,
                               const MLPadOptions* options,
                               ExceptionState& exception_state) {
  auto validated_output = webnn::ValidatePadAndInferOutput(
      ConvertToComponentOperand(input), beginning_padding, ending_padding);
  if (!validated_output.has_value()) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        String::FromUTF8(validated_output.error()));
    return nullptr;
  }

  if (options->mode().AsEnum() != V8MLPaddingMode::Enum::kConstant &&
      fabs(options->value() - 0.0f) > std::numeric_limits<float>::epsilon()) {
    ml_context_->LogConsoleWarning(
        "The pad value is ignored unless the options.mode is set to "
        "constant.");
  }

  auto* pad = MakeGarbageCollected<MLPadOperator>(this, beginning_padding,
                                                  ending_padding, options);
  // According to WebNN spec
  // https://www.w3.org/TR/webnn/#api-mlgraphbuilder-pad, the output
  // tensor of pad has the same type as its input.
  auto output = MLOperand::ValidateAndCreateOutput(
      this, input->Type(), Vector<uint32_t>(validated_output->dimensions), pad);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  pad->Connect({input}, {output.value()});
  return output.value();
}

MLOperand* MLGraphBuilder::averagePool2d(const MLOperand* input,
                                         const MLPool2dOptions* options,
                                         ExceptionState& exception_state) {
  return BuildPool2d(this, MLOperator::OperatorKind::kAveragePool2d, input,
                     options, exception_state);
}

MLOperand* MLGraphBuilder::maxPool2d(const MLOperand* input,
                                     const MLPool2dOptions* options,
                                     ExceptionState& exception_state) {
  return BuildPool2d(this, MLOperator::OperatorKind::kMaxPool2d, input, options,
                     exception_state);
}

MLOperand* MLGraphBuilder::prelu(const MLOperand* input,
                                 const MLOperand* slope,
                                 ExceptionState& exception_state) {
  auto validated_output = webnn::ValidatePreluAndInferOutput(
      ConvertToComponentOperand(input), ConvertToComponentOperand(slope));
  if (!validated_output.has_value()) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        String::FromUTF8(validated_output.error()));
    return nullptr;
  }

  auto* prelu =
      MakeGarbageCollected<MLOperator>(this, MLOperator::OperatorKind::kPRelu);
  auto output = MLOperand::ValidateAndCreateOutput(
      this, ComponentOperandTypeToBlink(validated_output->data_type),
      Vector<uint32_t>(validated_output->dimensions), prelu);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  prelu->Connect({input, slope}, {output.value()});
  return output.value();
}

MLOperand* MLGraphBuilder::relu(const MLOperand* input,
                                ExceptionState& exception_state) {
  auto* relu =
      MakeGarbageCollected<MLOperator>(this, MLOperator::OperatorKind::kRelu);
  // According to WebNN spec
  // https://www.w3.org/TR/webnn/#api-mlgraphbuilder-relu, the output tensor of
  // relu has the same type and dimensions as its input.
  auto output = MLOperand::ValidateAndCreateOutput(this, input->Type(),
                                                   input->Dimensions(), relu);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  relu->Connect({input}, {output.value()});
  return output.value();
}

MLActivation* MLGraphBuilder::relu(ExceptionState& exception_state) {
  // Create the relu operator that would be used as an activation function.
  return MakeGarbageCollected<MLActivation>(this,
                                            MLOperator::OperatorKind::kRelu);
}

MLOperand* MLGraphBuilder::reshape(
    const MLOperand* input,
    const Vector<absl::optional<uint32_t>>& new_shape,
    ExceptionState& exception_state) {
  absl::optional<wtf_size_t> null_dim_index = absl::nullopt;
  base::CheckedNumeric<size_t> checked_newshape_number_of_elements = 1;
  Vector<uint32_t> output_shape;
  if (new_shape.size() == 0) {
    // The empty new shape means reshaping to scalar, set output shape to {1}.
    output_shape = {1};
  } else {
    output_shape.resize(new_shape.size());
    // According to WebNN spec:
    // https://www.w3.org/TR/webnn/#api-mlgraphbuilder-reshape, only one
    // component of new shape can be the special value of null.
    for (wtf_size_t i = 0; i < new_shape.size(); ++i) {
      auto dim = new_shape[i];
      if (!dim) {
        if (null_dim_index) {
          exception_state.ThrowDOMException(
              DOMExceptionCode::kDataError,
              "Only one component of new shape can be null.");
          return nullptr;
        }
        null_dim_index = i;
      } else {
        if (dim.value() == 0) {
          exception_state.ThrowDOMException(
              DOMExceptionCode::kDataError,
              "The value of new shape should not be 0.");
          return nullptr;
        }
        checked_newshape_number_of_elements *= dim.value();
        output_shape[i] = dim.value();
      }
    }
  }
  size_t newshape_number_of_elements;
  if (!checked_newshape_number_of_elements.AssignIfValid(
          &newshape_number_of_elements)) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        "The number of elements implied by new shape is too large.");
    return nullptr;
  }
  DCHECK_NE(newshape_number_of_elements, size_t(0));
  if (null_dim_index) {
    // The size of the dimension with the value of null is computed so that the
    // total size remains constant.
    if (input->NumberOfElements() % newshape_number_of_elements != size_t(0)) {
      exception_state.ThrowDOMException(
          DOMExceptionCode::kDataError,
          String::Format(
              "The number of elements (%zu) in the input tensor can't be "
              "divided evenly by the number of elements (%zu) implied by new "
              "shape.",
              input->NumberOfElements(), newshape_number_of_elements));
      return nullptr;
    }
    // Check whether the quotient of type size_t is in the range of dimension of
    // type uint32_t.
    if (!base::CheckDiv(input->NumberOfElements(), newshape_number_of_elements)
             .AssignIfValid(&output_shape[null_dim_index.value()])) {
      exception_state.ThrowDOMException(
          DOMExceptionCode::kDataError,
          "The size of dimension with the value null is too large.");
      return nullptr;
    }
  } else {
    // The number of elements implied by new shape must be the same as the
    // number of elements in the input tensor.
    if (input->NumberOfElements() != newshape_number_of_elements) {
      exception_state.ThrowDOMException(
          DOMExceptionCode::kDataError,
          String::Format(
              "The number of elements (%zu) implied by new shape doesn't match "
              "the number of elements (%zu) in the input tensor.",
              newshape_number_of_elements, input->NumberOfElements()));
      return nullptr;
    }
  }
  auto* reshape = MakeGarbageCollected<MLOperator>(
      this, MLOperator::OperatorKind::kReshape);
  auto output = MLOperand::ValidateAndCreateOutput(
      this, input->Type(), std::move(output_shape), reshape);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  reshape->Connect({input}, {output.value()});
  return output.value();
}

MLOperand* MLGraphBuilder::resample2d(const MLOperand* input,
                                      const MLResample2dOptions* options,
                                      ExceptionState& exception_state) {
  // According to WebNN spec:
  // https://www.w3.org/TR/webnn/#api-mlgraphbuilder-resample2d, the input
  // must be a 4-D tensor.
  const auto input_shape = input->Dimensions();
  if (input_shape.size() != 4) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      "The input must be a 4-D tensor.");
    return nullptr;
  }

  const auto axes = options->getAxesOr({2, 3});
  if (axes.size() != 2) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      "The length of axes should be 2.");
    return nullptr;
  } else if (!((axes[0] == 0 && axes[1] == 1) ||
               (axes[0] == 1 && axes[1] == 2) ||
               (axes[0] == 2 && axes[1] == 3))) {
    // According to WebNN spec:
    // https://www.w3.org/TR/webnn/#api-mlgraphbuilder-resample2d,
    // the valid values in the sequence are [0, 1], [1, 2] or [2, 3].
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      "The values of axes are invalid.");
    return nullptr;
  }

  Vector<uint32_t> output_shape(input_shape);
  if (options->hasSizes()) {
    if (options->hasScales()) {
      ml_context_->LogConsoleWarning(
          "When sizes and scales are both specified, scales argument is "
          "ignored.");
    }
    if (options->sizes().size() != 2) {
      exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                        "The length of sizes should be 2.");
      return nullptr;
    } else if (std::any_of(options->sizes().begin(), options->sizes().end(),
                           [](uint32_t x) { return x == 0; })) {
      exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                        "All sizes should be greater than 0.");
      return nullptr;
    }
    output_shape[axes[0]] = options->sizes()[0];
    output_shape[axes[1]] = options->sizes()[1];
  } else {
    const auto scales = options->getScalesOr({1.0f, 1.0f});
    if (scales.size() != 2) {
      exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                        "The length of scales should be 2.");
      return nullptr;
    } else if (std::any_of(scales.begin(), scales.end(),
                           [](float x) { return x <= 0.0f; })) {
      exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                        "All scales should be greater than 0.");
      return nullptr;
    }
    auto output_height =
        CalculateResample2dOutputSize(input_shape[axes[0]], scales[0]);
    if (!output_height.has_value()) {
      exception_state.ThrowDOMException(
          DOMExceptionCode::kDataError,
          "Failed to calculate the output height: " + output_height.error());
      return nullptr;
    }
    output_shape[axes[0]] = output_height.value();

    auto output_width =
        CalculateResample2dOutputSize(input_shape[axes[1]], scales[1]);
    if (!output_width.has_value()) {
      exception_state.ThrowDOMException(
          DOMExceptionCode::kDataError,
          "Failed to calculate the output width: " + output_width.error());
      return nullptr;
    }
    output_shape[axes[1]] = output_width.value();
  }
  auto* resample2d = MakeGarbageCollected<MLOperator>(
      this, MLOperator::OperatorKind::kResample2d, options);
  // According to WebNN spec
  // https://www.w3.org/TR/webnn/#api-mlgraphbuilder-resample2d, the output
  // tensor of resample2d has the same type as its input.
  auto output = MLOperand::ValidateAndCreateOutput(
      this, input->Type(), std::move(output_shape), resample2d);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  resample2d->Connect({input}, {output.value()});
  return output.value();
}

MLOperand* MLGraphBuilder::sigmoid(const MLOperand* input,
                                   ExceptionState& exception_state) {
  auto* sigmoid = MakeGarbageCollected<MLOperator>(
      this, MLOperator::OperatorKind::kSigmoid);
  // According to WebNN spec
  // https://webmachinelearning.github.io/webnn/#api-mlgraphbuilder-sigmoid, the
  // output tensor of sigmoid has the same type and dimensions as its input.
  // And the input type must be one of the floating point types.
  if (!IsFloatingPointType(input->Type())) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        "The input type must be one of the floating point types.");
    return nullptr;
  }
  auto output = MLOperand::ValidateAndCreateOutput(
      this, input->Type(), input->Dimensions(), sigmoid);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  sigmoid->Connect({input}, {output.value()});
  return output.value();
}

MLActivation* MLGraphBuilder::sigmoid(ExceptionState& exception_state) {
  // Create the sigmoid operator that would be used as an activation function.
  return MakeGarbageCollected<MLActivation>(this,
                                            MLOperator::OperatorKind::kSigmoid);
}

MLOperand* MLGraphBuilder::slice(const MLOperand* input,
                                 const Vector<uint32_t>& starts,
                                 const Vector<uint32_t>& sizes,
                                 ExceptionState& exception_state) {
  webnn::SliceAttributes attributes;
  attributes.sizes.assign(sizes.begin(), sizes.end());
  attributes.starts.assign(starts.begin(), starts.end());
  auto validated_output = webnn::ValidateSliceAndInferOutput(
      ConvertToComponentOperand(input), attributes);
  if (!validated_output.has_value()) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        WTF::String::FromUTF8(validated_output.error()));
    return nullptr;
  }

  auto* slice = MakeGarbageCollected<MLSliceOperator>(this, starts, sizes);
  auto output = MLOperand::ValidateAndCreateOutput(
      this, ComponentOperandTypeToBlink(validated_output->data_type),
      Vector<uint32_t>(validated_output->dimensions), slice);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  slice->Connect({input}, {output.value()});
  return output.value();
}

MLOperand* MLGraphBuilder::softmax(const MLOperand* input,
                                   ExceptionState& exception_state) {
  auto validated_output =
      webnn::ValidateSoftmaxAndInferOutput(ConvertToComponentOperand(input));
  if (!validated_output.has_value()) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        WTF::String::FromUTF8(validated_output.error()));
    return nullptr;
  }
  auto* softmax = MakeGarbageCollected<MLOperator>(
      this, MLOperator::OperatorKind::kSoftmax);
  auto output = MLOperand::ValidateAndCreateOutput(
      this, ComponentOperandTypeToBlink(validated_output.value().data_type),
      Vector<uint32_t>(validated_output.value().dimensions), softmax);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  softmax->Connect({input}, {output.value()});
  return output.value();
}

HeapVector<Member<const MLOperand>> MLGraphBuilder::split(
    const MLOperand* input,
    const uint32_t splits,
    const MLSplitOptions* options,
    ExceptionState& exception_state) {
  auto validated_outputs = webnn::ValidateSplitAndInferOutput(
      ConvertToComponentOperand(input), {
                                            .splits = splits,
                                            .axis = options->axis(),
                                        });
  if (!validated_outputs.has_value()) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        WTF::String::FromUTF8(validated_outputs.error()));
    return {};
  }

  auto* split = MakeGarbageCollected<MLSplitOperator>(this, splits, options);
  HeapVector<Member<const MLOperand>> outputs;
  for (const auto& validated_output : validated_outputs.value()) {
    auto output = MLOperand::ValidateAndCreateOutput(
        this, ComponentOperandTypeToBlink(validated_output.data_type),
        Vector<uint32_t>(validated_output.dimensions), split);
    if (!output.has_value()) {
      exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                        output.error());
      return {};
    }
    outputs.push_back(output.value());
  }
  split->Connect({input}, outputs);
  return outputs;
}

// There are some backends don't support "split into sizes" variant, e.g.
// XNNPACK, and there is an ongoing discussion in WG:
// https://github.com/webmachinelearning/webnn/issues/392
HeapVector<Member<const MLOperand>> MLGraphBuilder::split(
    const MLOperand* input,
    const Vector<uint32_t>& splits,
    const MLSplitOptions* options,
    ExceptionState& exception_state) {
  auto validated_outputs = webnn::ValidateSplitAndInferOutput(
      ConvertToComponentOperand(input), {
                                            .splits = splits,
                                            .axis = options->axis(),
                                        });
  if (!validated_outputs.has_value()) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        WTF::String::FromUTF8(validated_outputs.error()));
    return {};
  }

  auto* split = MakeGarbageCollected<MLSplitOperator>(this, splits, options);
  HeapVector<Member<const MLOperand>> outputs;
  for (const auto& validated_output : validated_outputs.value()) {
    auto output = MLOperand::ValidateAndCreateOutput(
        this, ComponentOperandTypeToBlink(validated_output.data_type),
        Vector<uint32_t>(validated_output.dimensions), split);
    if (!output.has_value()) {
      exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                        output.error());
      return {};
    }
    outputs.push_back(output.value());
  }
  split->Connect({input}, outputs);
  return outputs;
}

MLOperand* MLGraphBuilder::tanh(const MLOperand* input,
                                ExceptionState& exception_state) {
  // The input type must be one of the floating point types.
  // The current spec doesn't specify the operand type constraints of tanh, an
  // issue has been filed to track it-
  // https://github.com/webmachinelearning/webnn/issues/283.
  if (!IsFloatingPointType(input->Type())) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        "The input type must be one of the floating point types.");
    return nullptr;
  }
  auto* tanh =
      MakeGarbageCollected<MLOperator>(this, MLOperator::OperatorKind::kTanh);
  // According to WebNN spec
  // https://www.w3.org/TR/webnn/#api-mlgraphbuilder-tanh, the output tensor of
  // tanh has the same type and dimensions as its input.
  auto output = MLOperand::ValidateAndCreateOutput(this, input->Type(),
                                                   input->Dimensions(), tanh);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  tanh->Connect({input}, {output.value()});
  return output.value();
}

MLActivation* MLGraphBuilder::tanh(ExceptionState& exception_state) {
  // Create the tanh operator that would be used as an activation function.
  return MakeGarbageCollected<MLActivation>(this,
                                            MLOperator::OperatorKind::kTanh);
}

MLOperand* MLGraphBuilder::transpose(const MLOperand* input,
                                     const MLTransposeOptions* options,
                                     ExceptionState& exception_state) {
  // According to WebNN spec:
  // https://www.w3.org/TR/webnn/#api-mlgraphbuilder-transpose,
  // When permutation is not specified, it’s set to [N-1, ..., 0], where N is
  // the rank of the input tensor.
  auto input_rank = input->Dimensions().size();
  const Vector<uint32_t> permutation =
      options->getPermutationOr(CreateDefaultPermutation(input_rank));
  auto validated_output = webnn::ValidateTransposeAndInferOutput(
      ConvertToComponentOperand(input), permutation);
  if (!validated_output.has_value()) {
    exception_state.ThrowDOMException(
        DOMExceptionCode::kDataError,
        String::FromUTF8(validated_output.error()));
    return nullptr;
  }

  auto* transpose = MakeGarbageCollected<MLOperator>(
      this, MLOperator::OperatorKind::kTranspose, options);
  // According to WebNN spec
  // https://www.w3.org/TR/webnn/#api-mlgraphbuilder-transpose, the output
  // tensor of transpose has the same type as its input.
  auto output = MLOperand::ValidateAndCreateOutput(
      this, ComponentOperandTypeToBlink(validated_output->data_type),
      Vector<uint32_t>(validated_output->dimensions), transpose);
  if (!output.has_value()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kDataError,
                                      output.error());
    return nullptr;
  }
  transpose->Connect({input}, {output.value()});
  return output.value();
}

ScriptPromise MLGraphBuilder::build(ScriptState* script_state,
                                    const MLNamedOperands& named_outputs,
                                    ExceptionState& exception_state) {
  if (!script_state->ContextIsValid()) {
    exception_state.ThrowDOMException(DOMExceptionCode::kInvalidStateError,
                                      "Invalid script state");
    return ScriptPromise();
  }

  auto* resolver = MakeGarbageCollected<ScriptPromiseResolver>(
      script_state, exception_state.GetContext());
  auto promise = resolver->Promise();

  if (g_backend_for_testing) {
    g_backend_for_testing->BuildGraphAsyncImpl(ml_context_, named_outputs,
                                               resolver);
    return promise;
  }

#if BUILDFLAG(BUILD_WEBNN_WITH_XNNPACK)
  if (ml_context_->GetDeviceType() == V8MLDeviceType::Enum::kCpu) {
    MLGraphXnnpack::ValidateAndBuildAsync(ml_context_, named_outputs, resolver);
    return promise;
  }
#endif

#if BUILDFLAG(BUILD_WEBNN_ON_CROS)
  // On ChromeOS, ML model inferencing is off-loaded to ModelLoader service.
  if (ml_context_->GetDeviceType() == V8MLDeviceType::Enum::kCpu) {
    MLGraphCrOS::ValidateAndBuildAsync(ml_context_, named_outputs, resolver);
    return promise;
  }
#endif

#if !BUILDFLAG(IS_CHROMEOS)
  // The runtime enable feature is used to disable the cross process hardware
  // acceleration by default.
  if (base::FeatureList::IsEnabled(
          webnn::features::kEnableMachineLearningNeuralNetworkService) &&
      ml_context_->GetDeviceType() == V8MLDeviceType::Enum::kGpu) {
    // Reject unsupported error on unimplemented platform when getting
    // `WebNNContext` mojo interface with BrowserInterfaceBroker's
    // GetInterface() method before creating `WebNNGraph` message pipe.
    MLContextMojo* ml_context_mojo =
        static_cast<MLContextMojo*>(ml_context_.Get());
    MLGraphMojo::ValidateAndBuildAsync(ml_context_mojo, named_outputs,
                                       resolver);
    return promise;
  }
#endif

  resolver->Reject(MakeGarbageCollected<DOMException>(
      DOMExceptionCode::kNotSupportedError, "Not implemented"));
  return promise;
}

MLGraph* MLGraphBuilder::buildSync(const MLNamedOperands& named_outputs,
                                   ExceptionState& exception_state) {
  if (g_backend_for_testing) {
    return g_backend_for_testing->BuildGraphSyncImpl(ml_context_, named_outputs,
                                                     exception_state);
  }

#if BUILDFLAG(BUILD_WEBNN_WITH_XNNPACK)
  if (ml_context_->GetDeviceType() == V8MLDeviceType::Enum::kCpu) {
    return MLGraphXnnpack::ValidateAndBuildSync(ml_context_, named_outputs,
                                                exception_state);
  }
#endif

  exception_state.ThrowDOMException(DOMExceptionCode::kNotSupportedError,
                                    "Not implemented");
  return nullptr;
}

// static
void MLGraphBuilder::SetBackendForTesting(
    MLGraphBuilder::BackendForTesting* backend_for_testing) {
  g_backend_for_testing = backend_for_testing;
}

}  // namespace blink
