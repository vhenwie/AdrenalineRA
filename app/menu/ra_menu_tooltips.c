#include "ra_menu_tooltips.h"

#include <string.h>

typedef struct {
  const char *name;
  const char *text;
} RaTooltip;

static const RaTooltip g_ra_tooltips[] = {
  { "Account >", "Account, sign-in, and achievement preferences." },
  { "Notifications >", "Choose which achievement and leaderboard messages appear." },
  { "Options >", "Gameplay, sound, and developer preferences." },
  { "Enable Achievements", "Turn RetroAchievements on or off." },
  { "Enable Hardcore Mode", "Disables save-state loading and game plugins "
    "while achievements are active." },
  { "Enable Spectator Mode", "Watch-only: shows achievement events without "
    "submitting unlocks or leaderboard scores." },
  { "Enable Encore Mode", "Treat achievements as locked at the start of every session." },
  { "Session Mode", "Standard: normal play. Spectator: watch only, nothing is "
    "submitted. Encore: achievements you already earned can unlock again." },
  { "Sign In", "Sign in with the entered username and password. Input locks until done." },
  { "Sign Out", "Hold the confirm button to sign out." },
  { "Back", "Return to the previous page." },
  { "Enable Notifications", "Show achievement, session, and status banners." },
  { "Enable Active Badges", "Show the active challenge badge tiles." },
  { "Enable Next Achievements", "Show the next 1 or 3 locked achievements "
    "under the list, so the next goal is always visible." },
  { "Enable Leaderboard Tracking", "Choose how leaderboards are tracked and announced." },
  { "Startup Summary", "Choose when the game session summary is shown." },
  { "Unlock Notifications", "Show a banner when an achievement unlocks." },
  { "Mastery Notifications", "Show a banner when every achievement is completed." },
  { "Progress Indicator", "Show achievement progress updates." },
  { "Leaderboard Start Messages", "Show when a leaderboard attempt starts." },
  { "Leaderboard Submit Messages", "Show when a leaderboard score is submitted." },
  { "Leaderboard Failed Messages", "Show when a leaderboard attempt fails." },
  { "Leaderboard Trackers", "Show active leaderboard tracker status." },
  { "Login Messages", "Show connection lost, restored, and account messages." },
  { "Verbose Messages", "Show diagnostic and server error messages." },
  { "Banner Options >", "Position and spacing for notification banners." },
  { "Active Badge Options >", "Position, size, and progress value for active challenge badges." },
  { "Position", "Choose the screen edge used for notification banners." },
  { "Aligned Padding", "Use the default distance from the screen edge." },
  { "Manual Horizontal Padding", "Set the horizontal distance from the anchor." },
  { "Manual Vertical Padding", "Set the vertical distance from the anchor." },
  { "Automatic Screenshots", "Capture a screenshot when an achievement unlocks." },
  { "Sound Effects >", "Choose notification and unlock sounds." },
  { "Enable Sound Effects", "Play sounds for notifications and achievement events." },
  { "Notification Sound", "Sound for session, status, and information banners." },
  { "Achievement Unlock Sound", "Sound when an achievement unlocks." },
  { "Leaderboard Submit Sound", "Sound when a leaderboard score is submitted." },
  { "Developer Features >", "Advanced diagnostics and exceptions." },
  { "Test Unofficial Achievements", "Include unofficial achievements in the achievement list." },
  { "Enable Logging", "Master switch for every log file. Default No." },
  { "Enable Art Prefetch", "Warm badge art for near-unlock achievements while you play. "
    "Off fetches art when the achievement unlocks." },
  { "Boost In-Game CPU Clock", "Hold 444 MHz in game: helps the remaining "
    "stutters and adds about 33% performance, at the cost of higher heat and "
    "faster battery drain." },
  { "Enable Save State in Hardcore Mode", "Allow saving and deleting states, "
    "but never loading them." },
  { "Achievements >", "List unlocked and locked achievements for the running game." },
  { "Leaderboards >", "List this game's leaderboards and their best entries." },
  { "Network Test", "Test the network, HTTP, SSL, and HTTPS connection stack." },
  { "Account Check", "Verify the signed-in account and available points." }
};

const char *ra_menu_tooltip_for(const char *name) {
  unsigned int i;

  if (name == NULL)
    return NULL;
  if (strncmp(name, "Username:", 9) == 0)
    return "Enter the RetroAchievements username used for sign-in.";
  if (strncmp(name, "Password", 8) == 0)
    return "Enter the password for this sign-in only; it is never stored.";

  for (i = 0; i < sizeof(g_ra_tooltips) / sizeof(g_ra_tooltips[0]); ++i) {
    if (strcmp(name, g_ra_tooltips[i].name) == 0)
      return g_ra_tooltips[i].text;
  }
  return NULL;
}
