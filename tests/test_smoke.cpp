// M0 冒烟测试：验证 Catch2 v3 单测基建可用。
// 后续里程碑的编码探测/换行归一/查找引擎/会话草稿序列化等核心逻辑单测都建立在该基建上。
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Catch2 基建可用", "[smoke]") {
    REQUIRE(1 + 1 == 2);
}
