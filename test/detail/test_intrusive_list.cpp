#include <coro/detail/intrusive_list.h>
#include <gtest/gtest.h>

using namespace coro::detail;

struct Node : IntrusiveListNode {
    int value;
    explicit Node(int v) : value(v) {}
};

// ── helpers ──────────────────────────────────────────────────────────────────

static std::vector<int> drain(IntrusiveList<IntrusiveListNode*>& list) {
    std::vector<int> out;
    while (auto* raw = list.pop_front())
        out.push_back(static_cast<Node*>(raw)->value);
    return out;
}

// ── basic ops ────────────────────────────────────────────────────────────────

TEST(IntrusiveList, PushBackPopFront) {
    Node a(1), b(2), c(3);
    IntrusiveList<IntrusiveListNode*> list;
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);
    EXPECT_EQ(list.size(), 3u);
    EXPECT_EQ(drain(list), (std::vector<int>{1, 2, 3}));
    EXPECT_TRUE(list.empty());
}

TEST(IntrusiveList, PopBack) {
    Node a(1), b(2), c(3);
    IntrusiveList<IntrusiveListNode*> list;
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);
    EXPECT_EQ(static_cast<Node*>(list.pop_back())->value, 3);
    EXPECT_EQ(static_cast<Node*>(list.pop_back())->value, 2);
    EXPECT_EQ(static_cast<Node*>(list.pop_back())->value, 1);
    EXPECT_EQ(list.pop_back(), nullptr);
}

TEST(IntrusiveList, Remove) {
    Node a(1), b(2), c(3);
    IntrusiveList<IntrusiveListNode*> list;
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);
    list.remove(&b);
    EXPECT_EQ(list.size(), 2u);
    EXPECT_EQ(drain(list), (std::vector<int>{1, 3}));
}

TEST(IntrusiveList, NodeUnlinkedAfterPop) {
    Node a(1);
    IntrusiveList<IntrusiveListNode*> list;
    list.push_back(&a);
    list.pop_front();
    EXPECT_FALSE(a.is_linked());
}

// ── push_back_block ───────────────────────────────────────────────────────────

TEST(IntrusiveList, PushBackBlockBasic) {
    Node a(1), b(2), c(3), d(4);
    IntrusiveList<IntrusiveListNode*> dst, src;
    dst.push_back(&a);
    dst.push_back(&b);
    src.push_back(&c);
    src.push_back(&d);

    dst.push_back_block(std::move(src));

    EXPECT_TRUE(src.empty());
    EXPECT_EQ(src.size(), 0u);
    EXPECT_EQ(dst.size(), 4u);
    EXPECT_EQ(drain(dst), (std::vector<int>{1, 2, 3, 4}));
}

TEST(IntrusiveList, PushBackBlockIntoEmpty) {
    Node a(1), b(2);
    IntrusiveList<IntrusiveListNode*> dst, src;
    src.push_back(&a);
    src.push_back(&b);

    dst.push_back_block(std::move(src));

    EXPECT_TRUE(src.empty());
    EXPECT_EQ(dst.size(), 2u);
    EXPECT_EQ(drain(dst), (std::vector<int>{1, 2}));
}

TEST(IntrusiveList, PushBackBlockFromEmpty) {
    Node a(1);
    IntrusiveList<IntrusiveListNode*> dst, src;
    dst.push_back(&a);

    dst.push_back_block(std::move(src));

    EXPECT_EQ(dst.size(), 1u);
    EXPECT_EQ(drain(dst), (std::vector<int>{1}));
}

TEST(IntrusiveList, PushBackBlockBothEmpty) {
    IntrusiveList<IntrusiveListNode*> dst, src;
    dst.push_back_block(std::move(src));
    EXPECT_TRUE(dst.empty());
}

// ── pop_back_block ────────────────────────────────────────────────────────────

TEST(IntrusiveList, PopBackBlockAll) {
    Node a(1), b(2), c(3);
    IntrusiveList<IntrusiveListNode*> list;
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);

    auto taken = list.pop_back_block(3);

    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0u);
    EXPECT_EQ(taken.size(), 3u);
    EXPECT_EQ(drain(taken), (std::vector<int>{1, 2, 3}));
}

TEST(IntrusiveList, PopBackBlockHalf) {
    Node a(1), b(2), c(3), d(4);
    IntrusiveList<IntrusiveListNode*> list;
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);
    list.push_back(&d);

    auto taken = list.pop_back_block(2);

    EXPECT_EQ(list.size(), 2u);
    EXPECT_EQ(taken.size(), 2u);
    EXPECT_EQ(drain(list),  (std::vector<int>{1, 2}));
    EXPECT_EQ(drain(taken), (std::vector<int>{3, 4}));
}

TEST(IntrusiveList, PopBackBlockOne) {
    Node a(1), b(2), c(3);
    IntrusiveList<IntrusiveListNode*> list;
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);

    auto taken = list.pop_back_block(1);

    EXPECT_EQ(list.size(), 2u);
    EXPECT_EQ(taken.size(), 1u);
    EXPECT_EQ(drain(list),  (std::vector<int>{1, 2}));
    EXPECT_EQ(drain(taken), (std::vector<int>{3}));
}

TEST(IntrusiveList, PopBackBlockMoreThanSize) {
    Node a(1), b(2);
    IntrusiveList<IntrusiveListNode*> list;
    list.push_back(&a);
    list.push_back(&b);

    auto taken = list.pop_back_block(10);

    EXPECT_TRUE(list.empty());
    EXPECT_EQ(taken.size(), 2u);
    EXPECT_EQ(drain(taken), (std::vector<int>{1, 2}));
}

TEST(IntrusiveList, PopBackBlockZero) {
    Node a(1);
    IntrusiveList<IntrusiveListNode*> list;
    list.push_back(&a);

    auto taken = list.pop_back_block(0);

    EXPECT_TRUE(taken.empty());
    EXPECT_EQ(list.size(), 1u);
}

TEST(IntrusiveList, PopBackBlockFromEmpty) {
    IntrusiveList<IntrusiveListNode*> list;
    auto taken = list.pop_back_block(5);
    EXPECT_TRUE(taken.empty());
}

TEST(IntrusiveList, PopBackBlockNodesUnlinkedInResult) {
    Node a(1), b(2), c(3), d(4);
    IntrusiveList<IntrusiveListNode*> list;
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);
    list.push_back(&d);

    auto taken = list.pop_back_block(2);

    // Boundary nodes should have clean prev/next after the split.
    EXPECT_EQ(static_cast<Node*>(list.pop_back())->value, 2);   // new tail
    EXPECT_EQ(static_cast<Node*>(taken.pop_front())->value, 3); // new head of taken
}

// ── round-trip: pop_back_block then push_back_block ───────────────────────────

TEST(IntrusiveList, RoundTrip) {
    Node a(1), b(2), c(3), d(4), e(5);
    IntrusiveList<IntrusiveListNode*> list;
    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);
    list.push_back(&d);
    list.push_back(&e);

    // Split: take back 3, keep front 2.
    auto stolen = list.pop_back_block(3);
    EXPECT_EQ(list.size(),   2u);
    EXPECT_EQ(stolen.size(), 3u);

    // Merge back.
    list.push_back_block(std::move(stolen));
    EXPECT_EQ(list.size(), 5u);
    EXPECT_EQ(drain(list), (std::vector<int>{1, 2, 3, 4, 5}));
}
