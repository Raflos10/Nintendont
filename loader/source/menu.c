/*

Nintendont (Loader) - Playing Gamecubes in Wii mode on a Wii U

Copyright (C) 2013  crediar

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation version 2.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/
#include <gccore.h>
#include "font.h"
#include "exi.h"
#include "global.h"
#include "FPad.h"
#include "Config.h"
#include "update.h"
#include "titles.h"
#include "dip.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ogc/stm.h>
#include <ogc/video.h>
#include <ogc/video_types.h>
#include <ogc/consol.h>
#include <ogc/system.h>
//#include <fat.h>
#include <di/di.h>
#include "menu.h"
#include "../../common/include/CommonConfigStrings.h"

#include "ff_utf8.h"

// Device state.
typedef enum {
	// Device is open and has a "games" directory.
	DEV_OK = 0,
	// Device could not be opened.
	DEV_NO_OPEN = 1,
	// Device was opened but has no "games" directory.
	DEV_NO_GAMES = 2,
} DevState;
static u8 devState = DEV_OK;

/**
 * Print information about the selected device.
 */
static void PrintDevInfo(void);

extern NIN_CFG* ncfg;

u32 Shutdown = 0;
extern char *launch_dir;

inline u32 SettingY(u32 row)
{
	return 127 + 16 * row;
}
void HandleWiiMoteEvent(s32 chan)
{
	Shutdown = 1;
}
void HandleSTMEvent(u32 event)
{
	*(vu32*)(0xCC003024) = 1;

	switch(event)
	{
		default:
		case STM_EVENT_RESET:
			break;
		case STM_EVENT_POWER:
			Shutdown = 1;
			break;
	}
}
int compare_names(const void *a, const void *b)
{
	const gameinfo *da = (const gameinfo *) a;
	const gameinfo *db = (const gameinfo *) b;

	int ret = strcasecmp(da->Name, db->Name);
	if (ret == 0)
	{
		// Names are equal. Check disc number.
		if (da->DiscNumber < db->DiscNumber)
			ret = -1;
		else if (da->DiscNumber > db->DiscNumber)
			ret = 1;
		else
			ret = 0;
	}
	return ret;
}

/**
 * Check if a disc image is valid.
 * @param filename	[in]  Disc image filename. (ISO/GCM)
 * @param discNumber	[in]  Disc number.
 * @param gi		[out] gameinfo struct to store game information if the disc is valid.
 * @return True if the image is valid; false if not.
 */
bool IsDiscImageValid(const char *filename, int discNumber, gameinfo *gi)
{
	// TODO: Handle FST format (sys/boot.bin).
	char gamename[65];		// Game title.
	char buf[0x100];		// Disc header.

	FIL in;
	if (f_open_char(&in, filename, FA_READ|FA_OPEN_EXISTING) != FR_OK)
	{
		// Could not open the disc image.
		return false;
	}

	// Read the disc header
	//gprintf("(%s) ok\n", filename );
	bool ret = false;
	UINT read;
	f_read(&in, buf, 0x100, &read);
	if (read != 0x100)
	{
		// Error reading from the file.
		f_close(&in);
		return false;
	}

	// Check for CISO magic with 2 MB block size.
	// NOTE: CISO block size is little-endian.
	static const uint8_t CISO_MAGIC[8] = {'C','I','S','O',0x00,0x00,0x20,0x00};
	if (!memcmp(buf, CISO_MAGIC, sizeof(CISO_MAGIC)) &&
	    !IsGCGame((u8*)buf))
	{
		// CISO magic is present, and GCN magic isn't.
		// This is most likely a CISO image.
		// Read the actual GCN header.
		f_lseek(&in, 0x8000);
		f_read(&in, buf, 0x100, &read);
		if (read != 0x100)
		{
			// Error reading from the file.
			f_close(&in);
			return false;
		}

		gi->Flags = GIFLAG_FORMAT_CISO;
	}
	else
	{
		// Standard GameCube disc image.
		// TODO: Detect Triforce images and exclude them
		// from size checking?
		if (in.obj.objsize == 1459978240)
		{
			// Full 1:1 GameCube image.
			gi->Flags = GIFLAG_FORMAT_FULL;
		}
		else
		{
			// Shrunken GameCube image.
			gi->Flags = GIFLAG_FORMAT_SHRUNKEN;
		}
	}

	// File is no longer needed.
	f_close(&in);

	if (IsGCGame((u8*)buf))	// Must be GC game
	{
		memcpy(gi->ID, buf, 6); //ID for EXI
		gi->DiscNumber = discNumber;

		// Check if this title is in titles.txt.
		bool isTriforce;
		const char *dbTitle = SearchTitles(gi->ID, &isTriforce);
		if (dbTitle)
		{
			// Title found.
			gi->Name = (char*)dbTitle;
			gi->Flags &= ~GIFLAG_NAME_ALLOC;

			if (isTriforce)
			{
				// Clear the format value if it's "shrunken",
				// since Triforce titles are never the size
				// of a full 1:1 GameCube disc image.
				if ((gi->Flags & GIFLAG_FORMAT_MASK) == GIFLAG_FORMAT_SHRUNKEN)
				{
					gi->Flags &= ~GIFLAG_FORMAT_MASK;
				}
			}
		}
		else
		{
			// Title not found.
			// Use the title from the disc header.
			strncpy(gamename, buf + 0x20, sizeof(gamename)-1);
			gamename[sizeof(gamename)-1] = 0;
			gi->Name = strdup(gamename);
			gi->Flags |= GIFLAG_NAME_ALLOC;
		}

		gi->Path = strdup(filename);
		ret = true;
	}

	return ret;
}

/**
 * Does a filename have a supported file extension?
 * @return True if it does; false if it doesn't.
 */
bool IsSupportedFileExt(const char *filename)
{
	size_t len = strlen(filename);
	if (len >= 5 && filename[len-4] == '.')
	{
		const int extpos = len-3;
		if (!strcasecmp(&filename[extpos], "gcm") ||
		    !strcasecmp(&filename[extpos], "iso") ||
		    !strcasecmp(&filename[extpos], "cso"))
		{
			// File extension is supported.
			return true;
		}
	}
	else if (len >= 6 && filename[len-5] == '.')
	{
		const int extpos = len-4;
		if (!strcasecmp(&filename[extpos], "ciso"))
		{
			// File extension is supported.
			return true;
		}
	}

	// File extension is NOT supported.
	return false;
}

/**
 * Get all games from the games/ directory on the selected storage device.
 * On Wii, this also adds a pseudo-game for loading GameCube games from disc.
 *
 * @param gi           [out] Array of gameinfo structs.
 * @param sz           [in]  Maximum number of elements in gi.
 * @param pGameCount   [out] Number of games loaded. (Includes GCN pseudo-game for Wii.)
 *
 * @return DevState value:
 * - DEV_OK: Device opened and has a "games/" directory.
 * - DEV_NO_OPEN: Could not open the storage device.
 * - DEV_NO_GAMES: No "games/" directory was found.
 */
static DevState LoadGameList(gameinfo *gi, u32 sz, u32 *pGameCount)
{
	// Create a list of games
	char filename[MAXPATHLEN];	// Current filename.
	char gamename[65];		// Game title.
	char buf[0x100];		// Disc header.
	int gamecount = 0;		// Current game count.

	if( !IsWiiU() )
	{
		// Pseudo game for booting a GameCube disc on Wii.
		gi[0].ID[0] = 'D',gi[0].ID[1] = 'I',gi[0].ID[2] = 'S';
		gi[0].ID[3] = 'C',gi[0].ID[4] = '0',gi[0].ID[5] = '1';
		gi[0].Name = "Boot GC Disc in Drive";
		gi[0].Flags = 0;
		gi[0].DiscNumber = 0;
		gi[0].Path = strdup("di:di");
		gamecount++;
	}

	DIR pdir;
	snprintf(filename, sizeof(filename), "%s:/games", GetRootDevice());
	if (f_opendir_char(&pdir, filename) != FR_OK)
	{
		// Could not open the "games" directory.

		// Attempt to open the device root.
		snprintf(filename, sizeof(filename), "%s:/", GetRootDevice());
		if (f_opendir_char(&pdir, filename) != FR_OK)
		{
			// Could not open the device root.
			if (pGameCount)
				*pGameCount = 0;
			return DEV_NO_OPEN;
		}

		// Device root opened.
		// This means the device is usable, but it
		// doesn't have a "games" directory.
		f_closedir(&pdir);
		if (pGameCount)
			*pGameCount = gamecount;
		return DEV_NO_GAMES;
	}

	// Process the directory.
	// TODO: chdir into /games/?
	FILINFO fInfo;
	FIL in;
	while (f_readdir(&pdir, &fInfo) == FR_OK && fInfo.fname[0] != '\0')
	{
		/**
		 * Game layout should be:
		 *
		 * ISO/GCM format:
		 * - /games/GAMEID/game.gcm
		 * - /games/GAMEID/game.iso
		 * - /games/GAMEID/disc2.gcm
		 * - /games/GAMEID/disc2.iso
		 * - /games/[anything].gcm
		 * - /games/[anything].iso
		 *
		 * CISO format:
		 * - /games/GAMEID/game.ciso
		 * - /games/GAMEID/game.cso
		 * - /games/GAMEID/disc2.ciso
		 * - /games/GAMEID/disc2.cso
		 * - /games/[anything].ciso
		 *
		 * FST format:
		 * - /games/GAMEID/sys/boot.bin plus other files
		 *
		 * NOTE: 2-disc games currently only work with the
		 * subdirectory layout, and the second disc must be
		 * named either disc2.iso or disc2.gcm.
		 */

		// Skip "." and "..".
		// This will also skip "hidden" directories.
		if (fInfo.fname[0] == '.')
			continue;

		if (fInfo.fattrib & AM_DIR)
		{
			// Subdirectory.

			// Prepare the filename buffer with the directory name.
			// game.iso/disc2.iso will be appended later.
			// NOTE: fInfo.fname[] is UTF-16.
			const char *filename_utf8 = wchar_to_char(fInfo.fname);
			int fnlen = snprintf(filename, sizeof(filename), "%s:/games/%s/",
					     GetRootDevice(), filename_utf8);

			//Test if game.iso exists and add to list
			bool found = false;

			static const char disc_filenames[8][16] = {
				"game.ciso", "game.cso", "game.gcm", "game.iso",
				"disc2.ciso", "disc2.cso", "disc2.gcm", "disc2.iso"
			};

			u32 i;
			for (i = 0; i < 8; i++)
			{
				const u32 discNumber = i / 4;

				// Append the disc filename.
				strcpy(&filename[fnlen], disc_filenames[i]);

				// Attempt to load disc information.
				if (IsDiscImageValid(filename, discNumber, &gi[gamecount]))
				{
					// Disc image exists and is a GameCube disc.
					gamecount++;
					found = true;
					// Next disc number.
					i = (discNumber * 4) + 3;
				}
			}

			// If none of the expected files were found,
			// check for FST format.
			if (!found)
			{
				memcpy(&filename[fnlen], "sys/boot.bin", 13);
				if (f_open_char(&in, filename, FA_READ|FA_OPEN_EXISTING) == FR_OK)
				{
					//gprintf("(%s) ok\n", filename );
					UINT read;
					f_read(&in, buf, 0x100, &read);
					f_close(&in);

					if (read == 0x100 && IsGCGame((u8*)buf))	// Must be GC game
					{
						// Terminate the filename at the game's base directory.
						filename[fnlen] = 0;

						memcpy(gi[gamecount].ID, buf, 6); //ID for EXI
						gi[gamecount].DiscNumber = 0;

						// TODO: Check titles.txt?
						strncpy(gamename, buf + 0x20, sizeof(gamename)-1);
						gamename[sizeof(gamename)-1] = 0;
						gi[gamecount].Name = strdup(gamename);
						gi[gamecount].Flags = GIFLAG_NAME_ALLOC | GIFLAG_FORMAT_FST;

						gi[gamecount].Path = strdup( filename );
						gamecount++;
					}
				}
			}
		}
		else
		{
			// Regular file.

			// Make sure its extension is ".iso" or ".gcm".
			const char *filename_utf8 = wchar_to_char(fInfo.fname);
			if (IsSupportedFileExt(filename_utf8))
			{
				// Create the full pathname.
				snprintf(filename, sizeof(filename), "%s:/games/%s",
					 GetRootDevice(), filename_utf8);

				// Attempt to load disc information.
				// (NOTE: Only disc 1 is supported right now.)
				if (IsDiscImageValid(filename, 0, &gi[gamecount]))
				{
					// Disc image exists and is a GameCube disc.
					gamecount++;
				}
			}
		}

		if (gamecount >= sz)	//if array is full
			break;
	}
	f_closedir(&pdir);

	// Sort the list alphabetically.
	// On Wii, the pseudo-entry for GameCube discs is always
	// kept at the top.
	if( IsWiiU() )
		qsort(gi, gamecount, sizeof(gameinfo), compare_names);
	else if( gamecount > 1 )
		qsort(&gi[1], gamecount-1, sizeof(gameinfo), compare_names);

	// Save the game count.
	if (pGameCount)
		*pGameCount = gamecount;

	return DEV_OK;
}

// Menu selection context.
typedef struct _MenuCtx
{
	u32 menuMode;		// Menu mode. (0 == games; 1 == settings)
	bool redraw;		// If true, redraw is required.
	bool selected;		// If true, the user selected a game.
	bool saveSettings;	// If true, save settings to nincfg.bin.

	// Counters for key repeat.
	struct {
		u32 Up;
		u32 Down;
		u32 Left;
		u32 Right;
	} held;

	// Games menu.
	struct {
		s32 posX;	// Selected game index.
		s32 scrollX;	// Current scrolling position.
		u32 listMax;	// Maximum number of games to show onscreen at once.

		const gameinfo *gi;	// Game information.
		int gamecount;		// Game count.
	} games;

	// Settings menu.
	struct {
		u32 settingPart;	// 0 == left column; 1 == right column
		s32 posX;		// Selected setting index.
	} settings;
} MenuCtx;

/** Key repeat wrapper functions. **/
#define FPAD_WRAPPER_REPEAT(Key) \
static inline int FPAD_##Key##_Repeat(MenuCtx *ctx) \
{ \
	int ret = 0; \
	if (FPAD_##Key(1)) { \
		ret = (ctx->held.Key == 0 || ctx->held.Key > 10); \
		ctx->held.Key++; \
	} else { \
		ctx->held.Key = 0; \
	} \
	return ret; \
}
FPAD_WRAPPER_REPEAT(Up)
FPAD_WRAPPER_REPEAT(Down)
FPAD_WRAPPER_REPEAT(Left)
FPAD_WRAPPER_REPEAT(Right)

/**
 * Update the Game Select menu.
 * @param ctx		[in] Menu context.
 * @return True to exit; false to continue.
 */
static bool UpdateGameSelectMenu(MenuCtx *ctx)
{
	u32 i;
	bool clearCheats = false;

	if (FPAD_Down_Repeat(ctx))
	{
		// Down: Move the cursor down by 1 entry.
		
		// Remove the current arrow.
		PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X+51*6-8, MENU_POS_Y + 20*6 + ctx->games.posX * 20, " " );

		// Adjust the scrolling position.
		if (ctx->games.posX + 1 >= ctx->games.listMax)
		{
			if (ctx->games.posX + 1 + ctx->games.scrollX < ctx->games.gamecount) {
				// Need to adjust the scroll position.
				ctx->games.scrollX++;
			} else {
				// Wraparound.
				ctx->games.posX	= 0;
				ctx->games.scrollX = 0;
			}
		} else {
			ctx->games.posX++;
		}

		clearCheats = true;
		ctx->redraw = true;
		ctx->saveSettings = true;
	}

	if (FPAD_Right_Repeat(ctx))
	{
		// Right: Move the cursor down by 1 page.

		// Remove the current arrow.
		PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X+51*6-8, MENU_POS_Y + 20*6 + ctx->games.posX * 20, " " );

		// Adjust the scrolling position.
		if (ctx->games.posX == ctx->games.listMax - 1)
		{
			if (ctx->games.posX + ctx->games.listMax + ctx->games.scrollX < ctx->games.gamecount) {
				ctx->games.scrollX += ctx->games.listMax;
			} else if (ctx->games.posX + ctx->games.scrollX != ctx->games.gamecount - 1) {
				ctx->games.scrollX = ctx->games.gamecount - ctx->games.listMax;
			} else {
				ctx->games.posX	= 0;
				ctx->games.scrollX = 0;
			}
		} else {
			ctx->games.posX = ctx->games.listMax - 1;
		}

		clearCheats = true;
		ctx->redraw = true;
		ctx->saveSettings = true;
	}

	if (FPAD_Up_Repeat(ctx))
	{
		// Up: Move the cursor up by 1 entry.

		// Remove the current arrow.
		PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X+51*6-8, MENU_POS_Y + 20*6 + ctx->games.posX * 20, " " );

		// Adjust the scrolling position.
		if (ctx->games.posX <= 0)
		{
			if (ctx->games.scrollX > 0) {
				ctx->games.scrollX--;
			} else {
				// Wraparound.
				ctx->games.posX	= ctx->games.listMax - 1;
				ctx->games.scrollX = ctx->games.gamecount - ctx->games.listMax;
			}
		} else {
			ctx->games.posX--;
		}

		clearCheats = true;
		ctx->redraw = true;
		ctx->saveSettings = true;
	}

	if (FPAD_Left_Repeat(ctx))
	{
		// Left: Move the cursor up by 1 page.

		// Remove the current arrow.
		PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X+51*6-8, MENU_POS_Y + 20*6 + ctx->games.posX * 20, " " );

		// Adjust the scrolling position.
		if (ctx->games.posX == 0)
		{
			if (ctx->games.scrollX - (s32)ctx->games.listMax >= 0) {
				ctx->games.scrollX -= ctx->games.listMax;
			} else if (ctx->games.scrollX != 0) {
				ctx->games.scrollX = 0;
			} else {
				ctx->games.scrollX = ctx->games.gamecount - ctx->games.listMax;
			}
		} else {
			ctx->games.posX = 0;
		}

		clearCheats = true;
		ctx->redraw = true;
		ctx->saveSettings = true;
	}

	if (FPAD_OK(0))
	{
		// User selected a game.
		ctx->selected = true;
		return true;
	}

	if (clearCheats)
	{
		if ((ncfg->Config & NIN_CFG_CHEATS) && (ncfg->Config & NIN_CFG_CHEAT_PATH))
		{
			// If a cheat path being used, clear it because it
			// can't be correct for a different game.
			ncfg->Config = ncfg->Config & ~(NIN_CFG_CHEATS | NIN_CFG_CHEAT_PATH);
		}
	}

	if (ctx->redraw)
	{
		// Redraw the game list.
		// TODO: Only if menuMode or scrollX has changed?

		// Starting position.
		int gamelist_y = MENU_POS_Y + 20*4;
		if (devState != DEV_OK)
		{
			// The warning message overlaps "Boot GC Disc in Drive".
			// Move the list down by one row.
			gamelist_y += 20;
		}

		const gameinfo *gi = &ctx->games.gi[ctx->games.scrollX];
		int gamesToPrint = ctx->games.gamecount - ctx->games.scrollX;
		if (gamesToPrint > ctx->games.listMax) {
			gamesToPrint = ctx->games.listMax;
		}

		for (i = 0; i < gamesToPrint; ++i, gamelist_y += 20, gi++)
		{
			// FIXME: Print all 64 characters of the game name?
			// Currently truncated to 50.

			// Determine color based on disc format.
			static const u32 colors[4] =
			{
				BLACK,		// Full
				0x551A00FF,	// Shrunken (dark brown)
				0x00551AFF,	// Extracted FST
				0x001A55FF,	// CISO
			};

			const u32 color = colors[gi->Flags & GIFLAG_FORMAT_MASK];
			if (gi->DiscNumber == 0)
			{
				// Disc 1.
				PrintFormat(DEFAULT_SIZE, color, MENU_POS_X, gamelist_y,
					    "%50.50s [%.6s]%s",
					    gi->Name, gi->ID,
					    i == ctx->games.posX ? ARROW_LEFT : " ");
			}
			else
			{
				// Disc 2 or higher.
				PrintFormat(DEFAULT_SIZE, color, MENU_POS_X, gamelist_y,
					    "%46.46s (%d) [%.6s]%s",
					    gi->Name, gi->DiscNumber+1, gi->ID,
					    i == ctx->games.posX ? ARROW_LEFT : " ");
			}
		}

		// GRRLIB rendering is done by SelectGame().
	}

	return false;
}

/**
 * Update the Settings menu.
 * @param ctx		[in] Menu context.
 * @return True to exit; false to continue.
 */
static bool UpdateSettingsMenu(MenuCtx *ctx)
{
	if(FPAD_X(0))
	{
		// Start the updater.
		UpdateNintendont();
		ctx->redraw = 1;
	}

	if (FPAD_Down_Repeat(ctx))
	{
		// Down: Move the cursor down by 1 setting.
		if (ctx->settings.settingPart == 0) {
			PrintFormat(MENU_SIZE, BLACK, MENU_POS_X+30, SettingY(ctx->settings.posX), " " );
		} else {
			PrintFormat(MENU_SIZE, BLACK, MENU_POS_X+300, SettingY(ctx->settings.posX), " " );
		}

		ctx->settings.posX++;
		if (ctx->settings.settingPart == 0)
		{
			// Some items are hidden if certain values aren't set.
			if (((ncfg->VideoMode & NIN_VID_FORCE) == 0) &&
			    (ctx->settings.posX == NIN_SETTINGS_VIDEOMODE))
			{
				ctx->settings.posX++;
			}
			if ((!(ncfg->Config & NIN_CFG_MEMCARDEMU)) &&
			    (ctx->settings.posX == NIN_SETTINGS_MEMCARDBLOCKS))
			{
				ctx->settings.posX++;
			}
			if ((!(ncfg->Config & NIN_CFG_MEMCARDEMU)) &&
			    (ctx->settings.posX == NIN_SETTINGS_MEMCARDMULTI))
			{
				ctx->settings.posX++;
			}
		}

		// Check for wraparound.
		if ((ctx->settings.settingPart == 0 && ctx->settings.posX >= NIN_SETTINGS_LAST) ||
		    (ctx->settings.settingPart == 1 && ctx->settings.posX >= 3))
		{
			ctx->settings.posX = 0;
			ctx->settings.settingPart ^= 1;
		}
	
		ctx->redraw = true;

	}
	else if (FPAD_Up_Repeat(ctx))
	{
		// Up: Move the cursor up by 1 setting.
		if (ctx->settings.settingPart == 0) {
			PrintFormat(MENU_SIZE, BLACK, MENU_POS_X+30, SettingY(ctx->settings.posX), " " );
		} else {
			PrintFormat(MENU_SIZE, BLACK, MENU_POS_X+300, SettingY(ctx->settings.posX), " " );
		}

		ctx->settings.posX--;

		// Check for wraparound.
		if (ctx->settings.posX < 0)
		{
			ctx->settings.settingPart ^= 1;
			if (ctx->settings.settingPart == 0) {
				ctx->settings.posX = NIN_SETTINGS_LAST - 1;
			} else {
				ctx->settings.posX = 2;
			}
		}

		if (ctx->settings.settingPart == 0)
		{
			// Some items are hidden if certain values aren't set.
			if ((!(ncfg->Config & NIN_CFG_MEMCARDEMU)) &&
			    (ctx->settings.posX == NIN_SETTINGS_MEMCARDMULTI))
			{
				ctx->settings.posX--;
			}
			if ((!(ncfg->Config & NIN_CFG_MEMCARDEMU)) &&
			    (ctx->settings.posX == NIN_SETTINGS_MEMCARDBLOCKS))
			{
				ctx->settings.posX--;
			}
			if (((ncfg->VideoMode & NIN_VID_FORCE) == 0) &&
			    (ctx->settings.posX == NIN_SETTINGS_VIDEOMODE))
			{
				ctx->settings.posX--;
			}
		}

		ctx->redraw = true;
	}

	if (FPAD_Left_Repeat(ctx))
	{
		// Left: Decrement a setting. (Right column only.)
		if (ctx->settings.settingPart == 1)
		{
			ctx->saveSettings = true;
			switch (ctx->settings.posX)
			{
				case 0:
					// Video width.
					if (ncfg->VideoScale == 0) {
						ncfg->VideoScale = 120;
					} else {
						ncfg->VideoScale -= 2;
						if (ncfg->VideoScale < 40 || ncfg->VideoScale > 120) {
							ncfg->VideoScale = 0; //auto
						}
					}
					ReconfigVideo(rmode);
					ctx->redraw = true;
					break;

				case 1:
					// Screen position.
					ncfg->VideoOffset--;
					if (ncfg->VideoOffset < -20 || ncfg->VideoOffset > 20) {
						ncfg->VideoOffset = 20;
					}
					ReconfigVideo(rmode);
					ctx->redraw = true;
					break;

				default:
					break;
			}
		}
	}
	else if (FPAD_Right_Repeat(ctx))
	{
		// Right: Increment a setting. (Right column only.)
		if (ctx->settings.settingPart == 1)
		{
			ctx->saveSettings = true;
			switch (ctx->settings.posX)
			{
				case 0:
					// Video width.
					if(ncfg->VideoScale == 0) {
						ncfg->VideoScale = 40;
					} else {
						ncfg->VideoScale += 2;
						if (ncfg->VideoScale < 40 || ncfg->VideoScale > 120) {
							ncfg->VideoScale = 0; //auto
						}
					}
					ReconfigVideo(rmode);
					ctx->redraw = true;
					break;

				case 1:
					// Screen position.
					ncfg->VideoOffset++;
					if (ncfg->VideoOffset < -20 || ncfg->VideoOffset > 20) {
						ncfg->VideoOffset = -20;
					}
					ReconfigVideo(rmode);
					ctx->redraw = true;
					break;

				default:
					break;
			}
		}
	}

	if( FPAD_OK(0) )
	{
		// A: Adjust the setting.
		if (ctx->settings.settingPart == 0)
		{
			// Left column.
			ctx->saveSettings = true;
			if (ctx->settings.posX < NIN_CFG_BIT_LAST)
			{
				// Standard boolean setting.
				if (ctx->settings.posX == NIN_CFG_BIT_USB) {
					// USB option is replaced with Wii U widescreen.
					ncfg->Config ^= NIN_CFG_WIIU_WIDE;
				} else {
					ncfg->Config ^= (1 << ctx->settings.posX);
				}
			}
			else switch (ctx->settings.posX)
			{
				case NIN_SETTINGS_MAX_PADS:
					ncfg->MaxPads++;
					if (ncfg->MaxPads > NIN_CFG_MAXPAD) {
						ncfg->MaxPads = 0;
					}
					break;

				case NIN_SETTINGS_LANGUAGE:
					ncfg->Language++;
					if (ncfg->Language > NIN_LAN_DUTCH) {
						ncfg->Language = NIN_LAN_AUTO;
					}
					break;

				case NIN_SETTINGS_VIDEO:
				{
					u32 Video = (ncfg->VideoMode & NIN_VID_MASK);
					switch (Video)
					{
						case NIN_VID_AUTO:
							Video = NIN_VID_FORCE;
							break;
						case NIN_VID_FORCE:
							Video = NIN_VID_FORCE | NIN_VID_FORCE_DF;
							break;
						case NIN_VID_FORCE | NIN_VID_FORCE_DF:
							Video = NIN_VID_NONE;
							break;
						default:
						case NIN_VID_NONE:
							Video = NIN_VID_AUTO;
							break;
					}
					ncfg->VideoMode &= ~NIN_VID_MASK;
					ncfg->VideoMode |= Video;
					if ((Video & NIN_VID_FORCE) == 0) {
						PrintFormat(MENU_SIZE, BLACK, MENU_POS_X+50, SettingY(NIN_SETTINGS_VIDEOMODE), "%29s", "" );
					}
					break;
				}

				case NIN_SETTINGS_VIDEOMODE:
				{
					u32 Video = (ncfg->VideoMode & NIN_VID_FORCE_MASK);
					Video = Video << 1;
					if (Video > NIN_VID_FORCE_MPAL) {
						Video = NIN_VID_FORCE_PAL50;
					}
					ncfg->VideoMode &= ~NIN_VID_FORCE_MASK;
					ncfg->VideoMode |= Video;
					break;
				}

				case NIN_SETTINGS_MEMCARDBLOCKS:
					ncfg->MemCardBlocks++;
					if (ncfg->MemCardBlocks > MEM_CARD_MAX) {
						ncfg->MemCardBlocks = 0;
					}
					break;

				case NIN_SETTINGS_MEMCARDMULTI:
					ncfg->Config ^= (NIN_CFG_MC_MULTI);
					break;

				case NIN_SETTINGS_NATIVE_SI:
					ncfg->Config ^= (NIN_CFG_NATIVE_SI);
					break;

				default:
					break;
			}

			// Blank out the memory card options if MCEMU is disabled.
			if (!(ncfg->Config & NIN_CFG_MEMCARDEMU))
			{
				PrintFormat(MENU_SIZE, BLACK, MENU_POS_X + 50, SettingY(NIN_SETTINGS_MEMCARDBLOCKS), "%29s", "");
				PrintFormat(MENU_SIZE, BLACK, MENU_POS_X + 50, SettingY(NIN_SETTINGS_MEMCARDMULTI), "%29s", "");
			}
			ctx->redraw = true;
		}
		else if (ctx->settings.settingPart == 1)
		{
			// Right column.
			if (ctx->settings.posX == 2)
			{
				// PAL50 patch.
				ctx->saveSettings = true;
				ncfg->VideoMode ^= (NIN_VID_PATCH_PAL50);
				ctx->redraw = true;
			}
		}
	}

	if (ctx->redraw)
	{
		// Redraw the settings menu.
		u32 ListLoopIndex = 0;

		// Standard boolean settings.
		for (ListLoopIndex = 0; ListLoopIndex < NIN_CFG_BIT_LAST; ListLoopIndex++)
		{
			if (ListLoopIndex == NIN_CFG_BIT_USB) {
				// USB option is replaced with Wii U widescreen.
				// FIXME: Gray out on standard Wii.
				PrintFormat(MENU_SIZE, BLACK, MENU_POS_X+50, SettingY(ListLoopIndex),
					    "%-18s:%s", OptionStrings[ListLoopIndex], (ncfg->Config & (NIN_CFG_WIIU_WIDE)) ? "On " : "Off");
			} else {
				PrintFormat(MENU_SIZE, BLACK, MENU_POS_X+50, SettingY(ListLoopIndex),
					    "%-18s:%s", OptionStrings[ListLoopIndex], (ncfg->Config & (1 << ListLoopIndex)) ? "On " : "Off" );
			}
		}

		// Maximum number of emulated controllers.
		PrintFormat(MENU_SIZE, BLACK, MENU_POS_X+50, SettingY(ListLoopIndex),
			    "%-18s:%d", OptionStrings[ListLoopIndex], (ncfg->MaxPads));
		ListLoopIndex++;

		// Language setting.
		u32 LanIndex = ncfg->Language;
		if (LanIndex < NIN_LAN_FIRST || LanIndex >= NIN_LAN_LAST) {
			LanIndex = NIN_LAN_LAST; //Auto
		}
		PrintFormat(MENU_SIZE, BLACK, MENU_POS_X+50, SettingY(ListLoopIndex),
			    "%-18s:%-4s", OptionStrings[ListLoopIndex], LanguageStrings[LanIndex] );
		ListLoopIndex++;

		// Video mode forcing.
		u32 VideoModeIndex;
		u32 VideoModeVal = ncfg->VideoMode & NIN_VID_MASK;
		switch (VideoModeVal)
		{
			case NIN_VID_AUTO:
				VideoModeIndex = NIN_VID_INDEX_AUTO;
				break;
			case NIN_VID_FORCE:
				VideoModeIndex = NIN_VID_INDEX_FORCE;
				break;
			case NIN_VID_FORCE | NIN_VID_FORCE_DF:
				VideoModeIndex = NIN_VID_INDEX_FORCE_DF;
				break;
			case NIN_VID_NONE:
				VideoModeIndex = NIN_VID_INDEX_NONE;
				break;
			default:
				ncfg->VideoMode &= ~NIN_VID_MASK;
				ncfg->VideoMode |= NIN_VID_AUTO;
				VideoModeIndex = NIN_VID_INDEX_AUTO;
				break;
		}
		PrintFormat(MENU_SIZE, BLACK, MENU_POS_X+50, SettingY(ListLoopIndex),
			    "%-18s:%-18s", OptionStrings[ListLoopIndex], VideoStrings[VideoModeIndex] );
		ListLoopIndex++;

		if( ncfg->VideoMode & NIN_VID_FORCE )
		{
			// Video mode selection.
			// Only available if video mode is Force or Force (Deflicker).
			VideoModeVal = ncfg->VideoMode & NIN_VID_FORCE_MASK;
			u32 VideoTestVal = NIN_VID_FORCE_PAL50;
			for (VideoModeIndex = 0; (VideoTestVal <= NIN_VID_FORCE_MPAL) && (VideoModeVal != VideoTestVal); VideoModeIndex++) {
				VideoTestVal <<= 1;
			}

			if ( VideoModeVal < VideoTestVal )
			{
				ncfg->VideoMode &= ~NIN_VID_FORCE_MASK;
				ncfg->VideoMode |= NIN_VID_FORCE_NTSC;
				VideoModeIndex = NIN_VID_INDEX_FORCE_NTSC;
			}
			PrintFormat(MENU_SIZE, BLACK, MENU_POS_X+50, SettingY(ListLoopIndex),
				    "%-18s:%-5s", OptionStrings[ListLoopIndex], VideoModeStrings[VideoModeIndex] );
		}
		ListLoopIndex++;

		// Memory card emulation.
		if ((ncfg->Config & NIN_CFG_MEMCARDEMU) == NIN_CFG_MEMCARDEMU)
		{
			u32 MemCardBlocksVal = ncfg->MemCardBlocks;
			if (MemCardBlocksVal > MEM_CARD_MAX) {
				MemCardBlocksVal = 0;
			}
			PrintFormat(MENU_SIZE, BLACK, MENU_POS_X + 50, SettingY(ListLoopIndex),
				    "%-18s:%-4d%s", OptionStrings[ListLoopIndex], MEM_CARD_BLOCKS(MemCardBlocksVal), MemCardBlocksVal > 2 ? "Unstable" : "");
			PrintFormat(MENU_SIZE, BLACK, MENU_POS_X + 50, SettingY(ListLoopIndex+1),
				    "%-18s:%-4s", OptionStrings[ListLoopIndex+1], (ncfg->Config & (NIN_CFG_MC_MULTI)) ? "On " : "Off");
		}
		ListLoopIndex+=2;

		// Native controllers. (Required for GBA link; disables Bluetooth and USB HID.)
		// FIXME: Gray out on vWii and maybe RVL-101?
		PrintFormat(MENU_SIZE, BLACK, MENU_POS_X + 50, SettingY(ListLoopIndex),
			    "%-18s:%-4s", OptionStrings[ListLoopIndex],
			    (ncfg->Config & (NIN_CFG_NATIVE_SI)) ? "On " : "Off");
		ListLoopIndex++;

		/** Right column **/
		ListLoopIndex = 0; //reset on other side

		// Video width.
		char vidWidth[10];
		if (ncfg->VideoScale < 40 || ncfg->VideoScale > 120) {
			ncfg->VideoScale = 0;
			snprintf(vidWidth, sizeof(vidWidth), "Auto");
		} else {
			snprintf(vidWidth, sizeof(vidWidth), "%i", ncfg->VideoScale + 600);
		}

		char vidOffset[10];
		if (ncfg->VideoOffset < -20 || ncfg->VideoOffset > 20) {
			ncfg->VideoOffset = 0;
		}
		snprintf(vidOffset, sizeof(vidOffset), "%i", ncfg->VideoOffset);

		PrintFormat(MENU_SIZE, BLACK, MENU_POS_X + 320, SettingY(ListLoopIndex),
			    "%-18s:%-4s", "Video Width", vidWidth);
		ListLoopIndex++;
		PrintFormat(MENU_SIZE, BLACK, MENU_POS_X + 320, SettingY(ListLoopIndex),
			    "%-18s:%-4s", "Screen Position", vidOffset);
		ListLoopIndex++;

		// Patch PAL60.
		PrintFormat(MENU_SIZE, BLACK, MENU_POS_X + 320, SettingY(ListLoopIndex),
			    "%-18s:%-4s", "Patch PAL50", (ncfg->VideoMode & (NIN_VID_PATCH_PAL50)) ? "On " : "Off");
		ListLoopIndex++;

		// Draw the cursor.
		if (ctx->settings.settingPart == 0) {
			PrintFormat(MENU_SIZE, BLACK, MENU_POS_X + 30, SettingY(ctx->settings.posX), ARROW_RIGHT);
		} else {
			PrintFormat(MENU_SIZE, BLACK, MENU_POS_X + 300, SettingY(ctx->settings.posX), ARROW_RIGHT);
		}

		// GRRLIB rendering is done by SelectGame().
	}

	return false;
}

/**
 * Select a game from the specified device.
 * @return Bitfield indicating the user's selection:
 * - 0 == go back
 * - 1 == game selected
 * - 2 == go back and save settings (UNUSED)
 * - 3 == game selected and save settings
 */
static int SelectGame(void)
{
	// Depending on how many games are on the storage device,
	// this could take a while.
	ShowLoadingScreen();

	// Load the game list.
	u32 gamecount = 0;
	gameinfo gi[MAX_GAMES];

	devState = LoadGameList(&gi[0], MAX_GAMES, &gamecount);
	switch (devState)
	{
		case DEV_OK:
			// Game list loaded successfully.
			break;

		case DEV_NO_GAMES:
			// No "games" directory was found.
			// The list will still be shown, since there's a
			// "Boot GC Disc in Drive" option on Wii.
			gprintf("WARNING: %s:/games/ was not found.\n", GetRootDevice());
			break;

		case DEV_NO_OPEN:
		default:
		{
			// Could not open the device at all.
			// The list won't be shown, since a storage device
			// is required for various functionality, but the
			// user will be able to go back to the previous menu.
			const char *s_devType = (UseSD ? "SD" : "USB");
			gprintf("No %s FAT device found.\n", s_devType);
			break;
		}
	}

	// Initialize the menu context.
	MenuCtx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.menuMode = 0;	// Start in the games list.
	ctx.redraw = true;	// Redraw initially.
	ctx.selected = false;	// Set to TRUE if the user selected a game.
	ctx.saveSettings = false;

	// Initialize ctx.games.
	ctx.games.listMax = gamecount;
	if (ctx.games.listMax > 15) {
		ctx.games.listMax = 15;
	}
	ctx.games.gi = gi;
	ctx.games.gamecount = gamecount;

	// Set the default game to the game that's currently set
	// in the configuration.
	u32 i;
	for (i = 0; i < gamecount; ++i)
	{
		if (strcasecmp(strchr(gi[i].Path,':')+1, ncfg->GamePath) == 0)
		{
			if (i >= ctx.games.listMax) {
				// Need to adjust the scroll position.
				ctx.games.posX    = ctx.games.listMax - 1;
				ctx.games.scrollX = i - ctx.games.listMax + 1;
			} else {
				// Game is on the first page.
				// No scroll position adjustment is required.
				ctx.games.posX = i;
			}
			break;
		}
	}

	while(1)
	{
		VIDEO_WaitVSync();
		FPAD_Update();

		if( FPAD_Start(1) )
		{
			// Go back to the Settings menu.
			ctx.selected = false;
			break;
		}

		if( FPAD_Cancel(0) )
		{
			// Switch menu modes.
			ctx.menuMode = !ctx.menuMode;
			memset(&ctx.held, 0, sizeof(ctx.held));

			if (ctx.menuMode == 1)
			{
				// Reset the settings position.
				ctx.settings.posX = 0;
				ctx.settings.settingPart = 0;
			}

			ctx.redraw = 1;
		}

		bool ret = false;
		if (ctx.menuMode == 0) {
			// Game Select menu.
			ret = UpdateGameSelectMenu(&ctx);
		} else {
			// Settings menu.
			ret = UpdateSettingsMenu(&ctx);
		}

		if (ret)
		{
			// User has exited the menu.
			break;
		}

		if (ctx.redraw)
		{
			// Redraw the header.
			PrintInfo();
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X + 430, MENU_POS_Y + 20*0, "Home: Go Back");
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X + 430, MENU_POS_Y + 20*1, "A   : %s", ctx.menuMode ? "Modify" : "Select");
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X + 430, MENU_POS_Y + 20*2, "B   : %s", ctx.menuMode ? "Game List" : "Settings ");
			if (ctx.menuMode)
			{
				PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X + 430, MENU_POS_Y + 20*3, "X/1 : Update");
			}

			if (ctx.menuMode == 0 ||
			    (ctx.menuMode == 1 && devState == DEV_OK))
			{
				// FIXME: If devState != DEV_OK,
				// the device info overlaps with the settings menu.
				PrintDevInfo();
			}

			// Render the screen.
			GRRLIB_Render();
			Screenshot();
			ClearScreen();
			ctx.redraw = false;
		}
	}

	// Save the selected game to the configuration.
	u32 SelectedGame = ctx.games.posX + ctx.games.scrollX;
	const char* StartChar = gi[SelectedGame].Path + 3;
	if (StartChar[0] == ':') {
		StartChar++;
	}
	strncpy(ncfg->GamePath, StartChar, sizeof(ncfg->GamePath));
	memcpy(&(ncfg->GameID), gi[SelectedGame].ID, 4);
	DCFlushRange((void*)ncfg, sizeof(NIN_CFG));

	// Free allocated memory in the game list.
	for (i = 0; i < gamecount; ++i)
	{
		if (gi[i].Flags & GIFLAG_NAME_ALLOC)
			free(gi[i].Name);
		free(gi[i].Path);
	}

	if (!ctx.selected)
	{
		// No game selected.
		return 0;
	}

	// Game is selected.
	// TODO: Return an enum.
	return (ctx.saveSettings ? 3 : 1);
}

/**
 * Select the source device and game.
 * @return TRUE to save settings; FALSE if no settings have been changed.
 */
bool SelectDevAndGame(void)
{
	// Select the source device. (SD or USB)
	bool SaveSettings = false;
	bool redraw = true;	// Need to draw the menu the first time.
	while (1)
	{
		VIDEO_WaitVSync();
		FPAD_Update();

		if (redraw)
		{
			UseSD = (ncfg->Config & NIN_CFG_USB) == 0;
			PrintInfo();
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X + 430, MENU_POS_Y + 20*0, "Home: Exit");
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X + 430, MENU_POS_Y + 20*1, "A   : Select");
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X + 53 * 6 - 8, MENU_POS_Y + 20 * 6, UseSD ? ARROW_LEFT : "");
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X + 53 * 6 - 8, MENU_POS_Y + 20 * 7, UseSD ? "" : ARROW_LEFT);
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X + 47 * 6 - 8, MENU_POS_Y + 20 * 6, " SD  ");
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X + 47 * 6 - 8, MENU_POS_Y + 20 * 7, "USB  ");
			redraw = false;

			// Render the screen here to prevent a blank frame
			// when returning from SelectGame().
			GRRLIB_Render();
			ClearScreen();
		}

		if (FPAD_OK(0))
		{
			// Select a game from the specified device.
			int ret = SelectGame();
			if (ret & 2) SaveSettings = true;
			if (ret & 1) break;
			redraw = true;
		}
		else if (FPAD_Start(0))
		{
			ShowMessageScreenAndExit("Returning to loader...", 0);
		}
		else if (FPAD_Down(0))
		{
			ncfg->Config = ncfg->Config | NIN_CFG_USB;
			redraw = true;
		}
		else if (FPAD_Up(0))
		{
			ncfg->Config = ncfg->Config & ~NIN_CFG_USB;
			redraw = true;
		}
	}

	return SaveSettings;
}

/**
 * Show a single message screen.
 * @param msg Message.
 */
void ShowMessageScreen(const char *msg)
{
	const int len = strlen(msg);
	const int x = (640 - (len*10)) / 2;

	ClearScreen();
	PrintInfo();
	PrintFormat(DEFAULT_SIZE, BLACK, x, 232, "%s", msg);
	GRRLIB_Render();
	ClearScreen();
}

/**
 * Show a single message screen and then exit to loader..
 * @param msg Message.
 * @param ret Return value. If non-zero, text will be printed in red.
 */
void ShowMessageScreenAndExit(const char *msg, int ret)
{
	const int len = strlen(msg);
	const int x = (640 - (len*10)) / 2;
	const u32 color = (ret == 0 ? BLACK : MAROON);

	ClearScreen();
	PrintInfo();
	PrintFormat(DEFAULT_SIZE, color, x, 232, "%s", msg);
	ExitToLoader(ret);
}

/**
 * Print Nintendont version and system hardware information.
 */
void PrintInfo(void)
{
#ifdef NIN_SPECIAL_VERSION
	// "Special" version with customizations. (Not mainline!)
	PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*0, "Nintendont Loader v%u.%u" NIN_SPECIAL_VERSION " (%s)",
		    NIN_VERSION>>16, NIN_VERSION&0xFFFF, IsWiiU() ? "Wii U" : "Wii");
#else
	PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*0, "Nintendont Loader v%u.%u (%s)",
		    NIN_VERSION>>16, NIN_VERSION&0xFFFF, IsWiiU() ? "Wii U" : "Wii");
#endif
	PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*1, "Built   : " __DATE__ " " __TIME__);
	PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*2, "Firmware: %u.%u.%u",
		    *(vu16*)0x80003140, *(vu8*)0x80003142, *(vu8*)0x80003143);
}

/**
 * Print information about the selected device.
 */
static void PrintDevInfo(void)
{
	// Device type.
	const char *s_devType = (UseSD ? "SD" : "USB");

	// Device state.
	// NOTE: If this is showing a message, the game list
	// will be moved down by 1 row, which usually isn't
	// a problem, since it will either be empty or showing
	// "Boot GC Disc in Drive".
	switch (devState) {
		case DEV_NO_OPEN:
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*4,
				"WARNING: %s FAT device could not be opened.", s_devType);
			break;
		case DEV_NO_GAMES:
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*4,
				"WARNING: %s:/games/ was not found.", GetRootDevice());
			break;
		default:
			break;
	}
}

void ReconfigVideo(GXRModeObj *vidmode)
{
	if(ncfg->VideoScale >= 40 && ncfg->VideoScale <= 120)
		vidmode->viWidth = ncfg->VideoScale + 600;
	else
		vidmode->viWidth = 640;
	vidmode->viXOrigin = (720 - vidmode->viWidth) / 2;

	if(ncfg->VideoOffset >= -20 && ncfg->VideoOffset <= 20)
	{
		if((vidmode->viXOrigin + ncfg->VideoOffset) < 0)
			vidmode->viXOrigin = 0;
		else if((vidmode->viXOrigin + ncfg->VideoOffset) > 80)
			vidmode->viXOrigin = 80;
		else
			vidmode->viXOrigin += ncfg->VideoOffset;
	}
	VIDEO_Configure(vidmode);
}

/**
 * Print a LoadKernel() error message.
 *
 * This function does NOT force a return to loader;
 * that must be handled by the caller.
 * Caller must also call UpdateScreen().
 *
 * @param iosErr IOS loading error ID.
 * @param err Return value from the IOS function.
 */
void PrintLoadKernelError(LoadKernelError_t iosErr, s32 err)
{
	ClearScreen();
	PrintInfo();
	PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*4, "Failed to load IOS58 from NAND:");

	switch (iosErr)
	{
		case LKERR_UNKNOWN:
		default:
			// TODO: Add descriptions of more LoadKernel() errors.
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*5, "LoadKernel() error %d occurred, returning %d.", iosErr, err);
			break;

		case LKERR_ES_GetStoredTMDSize:
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*5, "ES_GetStoredTMDSize() returned %d.", err);
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*7, "This usually means IOS58 is not installed.");
			if (IsWiiU())
			{
				// No IOS58 on Wii U should never happen...
				PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*9, "WARNING: On Wii U, a missing IOS58 may indicate");
				PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*10, "something is seriously wrong with the vWii setup.");
			}
			else
			{
				// TODO: Check if we're using System 4.3.
				PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*9, "Please update to Wii System 4.3 and try running");
				PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*10, "Nintendont again.");
			}
			break;

		case LKERR_TMD_malloc:
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*5, "Unable to allocate memory for the IOS58 TMD.");
			break;

		case LKERR_ES_GetStoredTMD:
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*5, "ES_GetStoredTMD() returned %d.", err);
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*7, "WARNING: IOS58 may be corrupted.");
			break;

		case LKERR_IOS_Open_shared1_content_map:
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*5, "IOS_Open(\"/shared1/content.map\") returned %d.", err);
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*7, "This usually means Nintendont was not started with");
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*8, "AHB access permissions.");
			// FIXME: Create meta.xml if it isn't there?
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*10, "Please ensure that meta.xml is present in your Nintendont");
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*11, "application directory and that it contains a line");
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*12, "with the tag <ahb_access/> .");
			break;

		case LKERR_IOS_Open_IOS58_kernel:
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*5, "IOS_Open(IOS58 kernel) returned %d.", err);
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*7, "WARNING: IOS58 may be corrupted.");
			break;

		case LKERR_IOS_Read_IOS58_kernel:
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*5, "IOS_Read(IOS58 kernel) returned %d.", err);
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*7, "WARNING: IOS58 may be corrupted.");
			break;
	}
}
