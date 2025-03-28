// Copyright 2022 Google LLC
// SPDX-License-Identifier: BSD-2-Clause

#include <vector>

#include "avif/avif.h"
#include "avif_fuzztest_helpers.h"
#include "aviftest_helpers.h"
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace libavif {
namespace testutil {
namespace {

::testing::Environment* const stack_limit_env =
    ::testing::AddGlobalTestEnvironment(
        new FuzztestStackLimitEnvironment("524288"));  // 512 * 1024

void EncodeDecodeValid(AvifImagePtr image, AvifEncoderPtr encoder,
                       AvifDecoderPtr decoder) {
  AvifImagePtr decoded_image(avifImageCreateEmpty(), avifImageDestroy);
  ASSERT_NE(image.get(), nullptr);
  ASSERT_NE(encoder.get(), nullptr);
  ASSERT_NE(decoder.get(), nullptr);
  ASSERT_NE(decoded_image.get(), nullptr);

  AvifRwData encoded_data;
  const avifResult encoder_result =
      avifEncoderWrite(encoder.get(), image.get(), &encoded_data);
  ASSERT_EQ(encoder_result, AVIF_RESULT_OK)
      << avifResultToString(encoder_result);

  const avifResult decoder_result = avifDecoderReadMemory(
      decoder.get(), decoded_image.get(), encoded_data.data, encoded_data.size);
  ASSERT_EQ(decoder_result, AVIF_RESULT_OK)
      << avifResultToString(decoder_result);

  EXPECT_EQ(decoded_image->width, image->width);
  EXPECT_EQ(decoded_image->height, image->height);
  EXPECT_EQ(decoded_image->depth, image->depth);
  EXPECT_EQ(decoded_image->yuvFormat, image->yuvFormat);

  // Verify that an opaque input leads to an opaque output.
  if (avifImageIsOpaque(image.get())) {
    EXPECT_TRUE(avifImageIsOpaque(decoded_image.get()));
  }
  // A transparent image may be heavily compressed to an opaque image. This is
  // hard to verify so do not check it.
}

FUZZ_TEST(EncodeDecodeAvifFuzzTest, EncodeDecodeValid)
    .WithDomains(ArbitraryAvifImage(), ArbitraryAvifEncoder(),
                 ArbitraryAvifDecoder({AVIF_CODEC_CHOICE_AUTO,
                                       AVIF_CODEC_CHOICE_DAV1D}));

}  // namespace
}  // namespace testutil
}  // namespace libavif
