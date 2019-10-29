#include <gtest/gtest.h>

#include <ATen/ATen.h>
#include <ATen/MemoryOverlap.h>

using namespace at;

std::vector<std::vector<int64_t>> sizes = {{1, 2, 3}, {1, 3, 2}, {2, 1, 3}, {3, 1, 2}, {3, 2, 1}, {2, 3, 1}};

TEST(MemoryOverlapTest, TensorExpanded) {
  for (auto size : sizes) {
    Tensor t = at::ones({1}).expand(size);
    EXPECT_FALSE(t.is_contiguous());
    EXPECT_FALSE(t.is_non_overlapping_and_dense());
  }
}

TEST(MemoryOverlapTest, ScalarExpanded) {
  for (auto size : sizes) {
    Tensor t = at::tensor(1).expand(size);
    EXPECT_FALSE(t.is_contiguous());
    EXPECT_FALSE(t.is_non_overlapping_and_dense());
  }
}

TEST(MemoryOverlapTest, NonContiguousTensor) {
  for (auto size : sizes) {
    Tensor t = at::rand(size).transpose(1, 2).transpose(0, 2);
    if (!t.is_contiguous()) {
      EXPECT_TRUE(t.is_non_overlapping_and_dense());
    }
  }
}

TEST(MemoryOverlapTest, NonContiguousExpandedTensor) {
  for (auto size : sizes) {
    Tensor t = at::rand(size).transpose(1, 2).transpose(0, 2);
    if (!t.is_contiguous()) {
      for (auto size_to_add : {1, 2, 3, 4}) {
        auto transpose_size = t.sizes().vec();
        std::vector<int64_t> expanded_size(transpose_size);
        expanded_size.insert(expanded_size.begin(), size_to_add);
        auto expanded = t.expand(expanded_size);
        EXPECT_FALSE(t.is_contiguous());
        if (size_to_add == 1) {
          EXPECT_TRUE(expanded.is_non_overlapping_and_dense());
        } else {
          EXPECT_FALSE(expanded.is_non_overlapping_and_dense());
        }
      }
    }
  }
}

TEST(MemoryOverlapTest, ContiguousTensor) {
  for (auto size : sizes) {
    Tensor t = at::rand(size);
    EXPECT_TRUE(t.is_contiguous());
    EXPECT_TRUE(t.is_non_overlapping_and_dense());
  }
}

TEST(MemoryOverlapTest, ContiguousExpandedTensor) {
  for (auto size : sizes) {
    Tensor t = at::rand(size);
    for (auto size_to_add : {1, 2, 3, 4}) {
      std::vector<int64_t> expanded_size(size);
      expanded_size.insert(expanded_size.begin(), size_to_add);
      auto expanded = t.expand(expanded_size);
      EXPECT_TRUE(t.is_contiguous());
      EXPECT_TRUE(t.is_non_overlapping_and_dense());
    }
  }
}

void TestContiguousTensor() {
  auto a = randn({2, 3}, T);
  auto b = randn({3}, T);
  auto c = randn({2, 1, 5}, T);
  auto d = randn({10, 2, 5, 5}, T);
  auto e = randn({1, 2, 5, 1}, T);

  ASSERT_TRUE(has_internal_overlap(a) == MemOverlap::NO);
  ASSERT_TRUE(has_internal_overlap(b) == MemOverlap::NO);
  ASSERT_TRUE(has_internal_overlap(c) == MemOverlap::NO);
  ASSERT_TRUE(has_internal_overlap(d) == MemOverlap::NO);
  ASSERT_TRUE(has_internal_overlap(e) == MemOverlap::NO);
}

void TestOverlapTensor() {
  auto a = randn({10, 1, 10}, T).expand({10, 10, 10});
  auto b = randn({1, 2}, T).expand({10, 2});
  auto c = randn({4, 1}, T).expand({4, 4});
  auto d = randn({2, 1, 4, 1}, T).expand({2, 4, 4, 1});

  ASSERT_TRUE(has_internal_overlap(a) == MemOverlap::YES);
  ASSERT_TRUE(has_internal_overlap(b) == MemOverlap::YES);
  ASSERT_TRUE(has_internal_overlap(c) == MemOverlap::YES);
  ASSERT_TRUE(has_internal_overlap(d) == MemOverlap::YES);

  /* hard case where there's overlap*/
  auto e = randn({16}, T);
  e.set_(e.storage(), e.storage_offset(), {2, 4, 2, 2}, {8, 2, 2, 1});
  ASSERT_TRUE(has_internal_overlap(e) != MemOverlap::NO);
}

void TestNonOverlapTensor() {

  /* easy non-packed tensor */
  auto a = randn({10, 4, 10}, T).slice(2, 1, 3);
  ASSERT_TRUE(has_internal_overlap(a) == MemOverlap::NO);
  /* easy size 1 dimension with strange stride */
  auto b = randn({3, 1, 5}, T);
  ASSERT_TRUE(has_internal_overlap(b) == MemOverlap::NO);

  /* hard case where there's no overlap*/
  auto c = randn({10}, T);
  c.set_(c.storage(), c.storage_offset(), {2, 3}, {4, 3});
  ASSERT_TRUE(has_internal_overlap(c) != MemOverlap::YES);
}

TEST(MemoryOverlapTest, HasInternalOverlap) {
  manual_seed(123);
  TestContiguousTensor();
  TestOverlapTensor();
  TestNonOverlapTensor();
}
