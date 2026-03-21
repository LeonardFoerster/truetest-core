#include <gtest/gtest.h>
#include "threading/ring_buffer.h"
#include <memory>

TEST(RingBuffer, PushPop_SingleElement)
{
    RingBuffer<int, 4> rb;
    EXPECT_TRUE(rb.try_push(42));
    int val = 0;
    EXPECT_TRUE(rb.try_pop(val));
    EXPECT_EQ(val, 42);
}

TEST(RingBuffer, FIFO_Order)
{
    RingBuffer<int, 8> rb;
    rb.try_push(1);
    rb.try_push(2);
    rb.try_push(3);
    int v;
    rb.try_pop(v); EXPECT_EQ(v, 1);
    rb.try_pop(v); EXPECT_EQ(v, 2);
    rb.try_pop(v); EXPECT_EQ(v, 3);
}

TEST(RingBuffer, Empty_PopFails)
{
    RingBuffer<int, 4> rb;
    int v;
    EXPECT_FALSE(rb.try_pop(v));
}

TEST(RingBuffer, Full_PushFails)
{
    RingBuffer<int, 4> rb;
    EXPECT_TRUE(rb.try_push(1));
    EXPECT_TRUE(rb.try_push(2));
    EXPECT_TRUE(rb.try_push(3));
    EXPECT_TRUE(rb.try_push(4));
    EXPECT_FALSE(rb.try_push(5));
}

TEST(RingBuffer, Capacity)
{
    RingBuffer<int, 16> rb;
    EXPECT_EQ(rb.capacity(), 16u);
}

TEST(RingBuffer, Size_Tracking)
{
    RingBuffer<int, 8> rb;
    rb.try_push(1);
    rb.try_push(2);
    rb.try_push(3);
    EXPECT_EQ(rb.size(), 3u);
    int v;
    rb.try_pop(v);
    EXPECT_EQ(rb.size(), 2u);
}

TEST(RingBuffer, Empty_Predicate)
{
    RingBuffer<int, 4> rb;
    EXPECT_TRUE(rb.empty());
    rb.try_push(1);
    EXPECT_FALSE(rb.empty());
}

TEST(RingBuffer, Full_Predicate)
{
    RingBuffer<int, 4> rb;
    for (int i = 0; i < 4; ++i) rb.try_push(i);
    EXPECT_TRUE(rb.full());
    int v;
    rb.try_pop(v);
    EXPECT_FALSE(rb.full());
}

TEST(RingBuffer, WrapAround)
{
    RingBuffer<int, 4> rb;
    // Fill and drain
    rb.try_push(1); rb.try_push(2); rb.try_push(3);
    int v;
    rb.try_pop(v); rb.try_pop(v); rb.try_pop(v);
    // Now push more — this wraps around
    rb.try_push(10); rb.try_push(20); rb.try_push(30);
    rb.try_pop(v); EXPECT_EQ(v, 10);
    rb.try_pop(v); EXPECT_EQ(v, 20);
    rb.try_pop(v); EXPECT_EQ(v, 30);
}

TEST(RingBuffer, Policy_AssertFull_Throws)
{
    RingBuffer<int, 4, AssertFull> rb;
    for (int i = 0; i < 4; ++i) rb.push(i);
    EXPECT_THROW(rb.push(99), std::runtime_error);
}

TEST(RingBuffer, Policy_DropOldest)
{
    RingBuffer<int, 4, DropOldest> rb;
    rb.push(1); rb.push(2); rb.push(3); rb.push(4);
    rb.push(5); // drops 1
    int v;
    rb.try_pop(v); EXPECT_EQ(v, 2);
    rb.try_pop(v); EXPECT_EQ(v, 3);
    rb.try_pop(v); EXPECT_EQ(v, 4);
    rb.try_pop(v); EXPECT_EQ(v, 5);
}

TEST(RingBuffer, SharedPtr_Elements)
{
    RingBuffer<std::shared_ptr<int>, 4> rb;
    auto p = std::make_shared<int>(42);
    EXPECT_EQ(p.use_count(), 1);
    rb.try_push(p);
    EXPECT_EQ(p.use_count(), 2); // in buffer + local
    std::shared_ptr<int> out;
    rb.try_pop(out);
    EXPECT_EQ(*out, 42);
}
