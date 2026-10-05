// The GM bot. It watches the GM channels, usually one in each server it
// serves, which are for saying GM and nothing else:
//
//  - A GM there is recorded in the ledger and acknowledged, and the streaks
//    worth announcing are announced.
//  - Anything else there costs its author their streak, and they are told.
//
// Each server has its own streaks, leaderboard and phrases.
//
// It also answers slash commands, anywhere in a server with a GM channel:
// /leaderboard and /streak report on streaks; /gmlist shows which phrases
// count as a GM, and /gmadd and /gmremove, which are for the server's
// administrators unless it decides otherwise, change them.

#ifndef BOT_BOT_H_
#define BOT_BOT_H_

#include "absl/status/status.h"
#include "async/task.h"
#include "bot/config.h"
#include "discord/client.h"
#include "gm/ledger.h"

namespace bot {

// Runs the bot as configured, on the calling thread, until it cannot go on.
// Never returns OK: the status says why it stopped, which is either that it
// could not start (the ledger cannot be opened, Discord cannot be reached or
// does not accept the token) or that Discord has since turned it away.
absl::Status Run(const Config& config);

// The bot's work, given its connection to Discord and its ledger: registers
// its commands in each GM channel's server, then handles events from `client`
// until Discord turns the bot away, and evaluates to why.
//
// Once under way, nothing short of that stops it. A message or a command
// that cannot be handled is logged and passed over, since the next may fare
// better. Failing to register the commands at the start does stop it, which
// is also how a GM channel the bot cannot see comes to light.
Task<absl::Status> Serve(discord::Client& client, gm::Ledger& ledger,
                         const Config& config);

}  // namespace bot

#endif  // BOT_BOT_H_
