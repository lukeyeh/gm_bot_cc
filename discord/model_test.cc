// The Discord model by example: ids that cannot be confused, and mentions.

#include "discord/model.h"

#include <cstdint>
#include <string>
#include <type_traits>

#include "gtest/gtest.h"

namespace {

// The compiler keeps the kinds of id apart.
static_assert(!std::is_same_v<discord::UserId, discord::ChannelId>);
static_assert(!std::is_convertible_v<discord::UserId, discord::ChannelId>);
static_assert(!std::is_convertible_v<discord::MessageId, discord::ChannelId>);

// Ids of one kind compare by value.
TEST(IdTest, ComparesByValue) {
  const discord::UserId luke{
      .value = 42,
  };
  const discord::UserId also_luke{
      .value = 42,
  };
  const discord::UserId ada{
      .value = 7,
  };

  EXPECT_EQ(luke, also_luke);
  EXPECT_NE(luke, ada);
}

// A mention is how a message addresses someone, whatever they are called.
TEST(MentionTest, IsDiscordsMarkupForAUser) {
  EXPECT_EQ(discord::Mention(discord::UserId{
                .value = 1234567890123456789,
            }),
            "<@1234567890123456789>");
}

// Options are looked up by name, with a fallback for the ones left out.
TEST(CommandInvokedTest, ReadsOptionsByNameWithAFallback) {
  const discord::CommandInvoked invoked{
      .name = "leaderboard",
      .options =
          {
              discord::OptionValue{
                  .name = "limit",
                  .value = int64_t{5},
              },
              discord::OptionValue{
                  .name = "title",
                  .value = std::string("this week"),
              },
          },
  };

  EXPECT_EQ(invoked.Integer("limit", 10), 5);
  EXPECT_EQ(invoked.Integer("offset", 10), 10);  // Left out.
  EXPECT_EQ(invoked.Integer("title", 10), 10);   // Not an integer.

  EXPECT_EQ(invoked.Text("title"), "this week");
  EXPECT_EQ(invoked.Text("subtitle"), "");  // Left out.
  EXPECT_EQ(invoked.Text("limit"), "");     // Not text.
}

}  // namespace
