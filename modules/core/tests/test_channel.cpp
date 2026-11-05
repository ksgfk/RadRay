#include <radray/channel.h>

#include <thread>

#include <gtest/gtest.h>

namespace radray {
namespace {

TEST(ChannelTest, BoundedTryWriteFailsWhenFull) {
    BoundedChannel<int> channel{2};

    EXPECT_TRUE(channel.TryWrite(1));
    EXPECT_TRUE(channel.TryWrite(2));
    EXPECT_FALSE(channel.TryWrite(3));
    EXPECT_EQ(channel.Size(), 2u);
}

TEST(ChannelTest, BoundedReadsInWriteOrder) {
    BoundedChannel<int> channel{4};
    channel.TryWrite(1);
    channel.TryWrite(2);
    int first = 0;
    int second = 0;

    ASSERT_TRUE(channel.TryRead(first));
    ASSERT_TRUE(channel.TryRead(second));

    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 2);
    EXPECT_FALSE(channel.TryRead(first));
}

TEST(ChannelTest, BoundedCompleteRejectsWritesButDrainsQueuedMessages) {
    BoundedChannel<int> channel{4};
    channel.TryWrite(7);

    channel.Complete();

    int value = 0;
    EXPECT_TRUE(channel.IsCompleted());
    EXPECT_FALSE(channel.TryWrite(8));
    EXPECT_FALSE(channel.WaitWrite(8));
    EXPECT_TRUE(channel.WaitRead(value));
    EXPECT_EQ(value, 7);
    EXPECT_FALSE(channel.WaitRead(value));
}

TEST(ChannelTest, BoundedWaitWriteBlocksUntilReaderMakesRoom) {
    BoundedChannel<int> channel{1};
    channel.TryWrite(1);
    std::thread writer{[&channel] { EXPECT_TRUE(channel.WaitWrite(2)); }};

    int first = 0;
    int second = 0;
    ASSERT_TRUE(channel.WaitRead(first));
    ASSERT_TRUE(channel.WaitRead(second));
    writer.join();

    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 2);
}

TEST(ChannelTest, BoundedCompleteWakesBlockedWriter) {
    BoundedChannel<int> channel{1};
    channel.TryWrite(1);
    std::thread writer{[&channel] { EXPECT_FALSE(channel.WaitWrite(2)); }};

    channel.Complete();
    writer.join();

    EXPECT_EQ(channel.Size(), 1u);
}

TEST(ChannelTest, UnboundedAcceptsWritesUntilComplete) {
    UnboundedChannel<int> channel;
    for (int i = 0; i < 100; ++i) ASSERT_TRUE(channel.TryWrite(i));

    channel.Complete();

    int value = -1;
    EXPECT_FALSE(channel.WaitWrite(100));
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(channel.WaitRead(value));
        EXPECT_EQ(value, i);
    }
    EXPECT_FALSE(channel.WaitRead(value));
}

TEST(ChannelTest, UnboundedCompleteWakesBlockedReader) {
    UnboundedChannel<int> channel;
    std::thread reader{[&channel] {
        int value = 0;
        EXPECT_FALSE(channel.WaitRead(value));
    }};

    channel.Complete();
    reader.join();
}

TEST(ChannelTest, UnboundedWaitReadReceivesValueFromAnotherThread) {
    UnboundedChannel<int> channel;
    std::thread writer{[&channel] { channel.TryWrite(5); }};

    int value = 0;
    EXPECT_TRUE(channel.WaitRead(value));
    writer.join();

    EXPECT_EQ(value, 5);
}

}  // namespace
}  // namespace radray
