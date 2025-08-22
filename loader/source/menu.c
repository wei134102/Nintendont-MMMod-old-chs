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
#include <sys/param.h>
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
#include <ogc/consol.h>
#include <ogc/system.h>
//#include <fat.h>
#include <di/di.h>

#include "menu.h"
#include "../../common/include/CommonConfigStrings.h"
#include "ff_utf8.h"
#include "ShowGameInfo.h"

#include "mysterio.h"

// Dark gray for grayed-out menu items.
#define DARK_GRAY 0x666666FF

// Device state.
typedef enum {
	// Device is open and has GC titles in "games" directory.
	DEV_OK = 0,
	// Device could not be opened.
	DEV_NO_OPEN = 1,
	// Device was opened but has no "games" directory.
	DEV_NO_GAMES = 2,
	// Device was opened but "games" directory is empty.
	DEV_NO_TITLES = 3,
} DevState;
static u8 devState = DEV_OK;

// Disc format colors.
const u32 DiscFormatColors[8] =
{
	BLACK,		// Full
	0x551A00FF,	// Shrunken (dark brown)
	0x00551AFF,	// Extracted FST
	0x001A55FF,	// CISO
	0x551A55FF,	// Multi-Game
	0x55551AFF,	// Oversized (dark yellow)超过容量 深黄色
	GRAY,		// undefined
	GRAY,		// undefined
};

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
void SetShutdown(void)
{
	Shutdown = 1;
}
void HandleWiiMoteEvent(s32 chan)
{
	SetShutdown();
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
			SetShutdown();
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
		const uint8_t dnuma = (da->Flags & GIFLAG_DISCNUMBER_MASK);
		const uint8_t dnumb = (db->Flags & GIFLAG_DISCNUMBER_MASK);
		if (dnuma < dnumb)
			ret = -1;
		else if (dnuma > dnumb)
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
static bool IsDiscImageValid(const char *filename, int discNumber, gameinfo *gi)
{
	// TODO: Handle FST format (sys/boot.bin).
	u8 buf[0x100];			// Disc header.
	u32 BI2region_addr = 0x458;	// BI2 region code address.

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
	f_read(&in, buf, sizeof(buf), &read);
	if (read != sizeof(buf))
	{
		// Error reading from the file.
		f_close(&in);
		return false;
	}

	// Check for CISO magic with 2 MB block size.
	// NOTE: CISO block size is little-endian.
	static const uint8_t CISO_MAGIC[8] = {'C','I','S','O',0x00,0x00,0x20,0x00};
	if (!memcmp(buf, CISO_MAGIC, sizeof(CISO_MAGIC)) &&
	    !IsGCGame(buf))
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

		// Adjust the BI2 region code address for CISO.
		BI2region_addr = 0x8458;

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
		else if (in.obj.objsize > 1459978240)
		{
			// Oversized GameCube image.
			gi->Flags = GIFLAG_FORMAT_OVER;
		}		
		else
		{
			// Shrunken GameCube image.
			gi->Flags = GIFLAG_FORMAT_SHRUNKEN;
		}
	}

	if (IsGCGame(buf))	// Must be GC game
	{
		// Read the BI2 region code.
		u32 BI2region;
		f_lseek(&in, BI2region_addr);
		f_read(&in, &BI2region, sizeof(BI2region), &read);
		if (read != sizeof(BI2region)) {
			// Error reading from the file.
			f_close(&in);
			return false;
		}

		// Save the region code for later.
		gi->Flags |= ((BI2region & 3) << 3);

		// Save the game ID.
		memcpy(gi->ID, buf, 6); //ID for EXI
		gi->Flags |= (discNumber & 3) << 5;

		// Save the revision number.
		gi->Revision = buf[0x07];

		// Check if this is a multi-game image.
		// Reference: https://gbatemp.net/threads/wit-wiimms-iso-tools-gamecube-disc-support.251630/#post-3088119
		const bool is_multigame = IsMultiGameDisc((const char*)buf);
		if (is_multigame)
		{
			if (gi->Flags == GIFLAG_FORMAT_CISO)
			{
				// Multi-game + CISO is NOT supported.
				ret = false;
			}
			else
			{
				// Multi-game disc.
				char *name = (char*)malloc(65);
				const char *slash_pos = strrchr(filename, '/');
				snprintf(name, 65, "合集游戏 (%s)", (slash_pos ? slash_pos+1 : filename));
				gi->Name = name;
				gi->Flags = GIFLAG_FORMAT_MULTI | GIFLAG_NAME_ALLOC;
			}
		}
		else
		{
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

			if (!dbTitle)
			{
				// Title not found.
				// Use the title from the disc header.

				// Make sure the title in the header is NULL terminated.
				buf[0x20+65] = 0;
				gi->Name = strdup((const char*)&buf[0x20]);
				gi->Flags |= GIFLAG_NAME_ALLOC;
			}
		}

		gi->Path = strdup(filename);
		ret = true;
	}

	f_close(&in);
	return ret;
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
 * - DEV_OK: Device opened and has GC titles in "games/" directory.
 * - DEV_NO_OPEN: Could not open the storage device.
 * - DEV_NO_GAMES: No "games/" directory was found.
 * - DEV_NO_TITLES: "games/" directory contains no GC titles.
 */
static DevState LoadGameList(gameinfo *gi, u32 sz, u32 *pGameCount)
{
	// Create a list of games
	char filename[MAXPATHLEN];	// Current filename.
	u8 buf[0x100];			// Disc header.
	int gamecount = 0;		// Current game count.

	if( isWiiVC )
	{
		// Pseudo game for booting a GameCube disc on Wii VC.
		gi[0].ID[0] = 'D',gi[0].ID[1] = 'I',gi[0].ID[2] = 'S';
		gi[0].ID[3] = 'C',gi[0].ID[4] = '0',gi[0].ID[5] = '1';
		gi[0].Name = "从光驱中读取游戏";
		gi[0].Revision = 0;
		gi[0].Flags = 0;
		gi[0].Path = strdup("di:di");
		gamecount++;
	}
	else if( !IsWiiU() )
	{
		// Pseudo game for booting a GameCube disc on Wii.
		gi[0].ID[0] = 'D',gi[0].ID[1] = 'I',gi[0].ID[2] = 'S';
		gi[0].ID[3] = 'C',gi[0].ID[4] = '0',gi[0].ID[5] = '1';
		gi[0].Name = "从光驱中启动GC游戏";
		gi[0].Revision = 0;
		gi[0].Flags = 0;
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
				// Read the disc header from boot.bin.
				memcpy(&filename[fnlen], "sys/boot.bin", 13);
				if (f_open_char(&in, filename, FA_READ|FA_OPEN_EXISTING) != FR_OK)
					continue;
				//gprintf("(%s) ok\n", filename );
				UINT read;
				f_read(&in, buf, 0x100, &read);
				f_close(&in);
				if (read != 0x100 || !IsGCGame(buf))
					continue;

				// Read the BI2.bin region code.
				memcpy(&filename[fnlen], "sys/bi2.bin", 12);
				if (f_open_char(&in, filename, FA_READ|FA_OPEN_EXISTING) != FR_OK)
					continue;
				//gprintf("(%s) ok\n", filename );
				u32 BI2region;
				f_lseek(&in, 0x18);
				f_read(&in, &BI2region, sizeof(BI2region), &read);
				f_close(&in);
				if (read != sizeof(BI2region))
					continue;

				// Terminate the filename at the game's base directory.
				filename[fnlen] = 0;

				// Make sure the title in the header is NULL terminated.
				buf[0x20+65] = 0;

				memcpy(gi[gamecount].ID, buf, 6); //ID for EXI
				gi[gamecount].Revision = 0;

				// TODO: Check titles.txt?
				gi[gamecount].Name = strdup((const char*)&buf[0x20]);
				gi[gamecount].Flags = GIFLAG_NAME_ALLOC | GIFLAG_FORMAT_FST | ((BI2region & 3) << 3);

				gi[gamecount].Path = strdup(filename);
				gamecount++;
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
	if( gamecount && IsWiiU() && !isWiiVC )
		qsort(gi, gamecount, sizeof(gameinfo), compare_names);
	else if( gamecount > 1 )
		qsort(&gi[1], gamecount-1, sizeof(gameinfo), compare_names);

	// Save the game count.
	if (pGameCount)
		*pGameCount = gamecount;

	if(gamecount == 0)
		return DEV_NO_TITLES;

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

		bool canBeBooted;	// Selected game is bootable.
		bool canShowInfo;	// Can show information for the selected game.
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
 * @param ctx	[in] Menu context.
 * @return True to exit; false to continue.
 */
static bool UpdateGameSelectMenu(MenuCtx *ctx)
{
	u32 i;
	bool clearCheats = false;

	if(FPAD_X(0))
	{
		// Can we show information for the selected game?
		if (ctx->games.canShowInfo)
		{
			// Show game information.
			ShowGameInfo(&ctx->games.gi[ctx->games.posX + ctx->games.scrollX]);
			ctx->redraw = true;
		}
	}

	if (FPAD_Down_Repeat(ctx))
	{
		// Down: Move the cursor down by 1 entry.
		
		// Remove the current arrow.
		PrintFormat(DEFAULT_SIZE, text_color, MENU_POS_X+51*6-8, MENU_POS_Y + 20*6 + ctx->games.posX * 20, " " );

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
		PrintFormat(DEFAULT_SIZE, text_color, MENU_POS_X+51*6-8, MENU_POS_Y + 20*6 + ctx->games.posX * 20, " " );

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
		} else if(ctx->games.listMax) {
			ctx->games.posX = ctx->games.listMax - 1;
		}
		else {
			ctx->games.posX	= 0;
		}

		clearCheats = true;
		ctx->redraw = true;
		ctx->saveSettings = true;
	}

	if (FPAD_Up_Repeat(ctx))
	{
		// Up: Move the cursor up by 1 entry.

		// Remove the current arrow.
		PrintFormat(DEFAULT_SIZE, text_color, MENU_POS_X+51*6-8, MENU_POS_Y + 20*6 + ctx->games.posX * 20, " " );

		// Adjust the scrolling position.
		if (ctx->games.posX <= 0)
		{
			if (ctx->games.scrollX > 0) {
				ctx->games.scrollX--;
			} else {
				// Wraparound.
				if(ctx->games.listMax) {
					ctx->games.posX = ctx->games.listMax - 1;
				}
				else {
					ctx->games.posX	= 0;
				}
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
		PrintFormat(DEFAULT_SIZE, text_color, MENU_POS_X+51*6-8, MENU_POS_Y + 20*6 + ctx->games.posX * 20, " " );

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

	if (FPAD_OK(0) && ctx->games.canBeBooted)
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

		// if (!cColor){// Print the color codes.
		// PrintFormat(DEFAULT_SIZE, DiscFormatColors[0], MENU_POS_X, MENU_POS_Y + 20*3, "Colors  : 1:1");
		// PrintFormat(DEFAULT_SIZE, DiscFormatColors[1], MENU_POS_X+(14*10), MENU_POS_Y + 20*3, "Shrunk");
		// PrintFormat(DEFAULT_SIZE, DiscFormatColors[2], MENU_POS_X+(21*10), MENU_POS_Y + 20*3, "FST");
		// PrintFormat(DEFAULT_SIZE, DiscFormatColors[3], MENU_POS_X+(25*10), MENU_POS_Y + 20*3, "CISO");
		// PrintFormat(DEFAULT_SIZE, DiscFormatColors[4], MENU_POS_X+(30*10), MENU_POS_Y + 20*3, "Multi");
		// }

		PrintFormat(DEFAULT_SIZE, DiscFormatColors[0], MENU_POS_X, MENU_POS_Y + 20*3, "Colors  : 1:1");
		PrintFormat(DEFAULT_SIZE, DiscFormatColors[1], MENU_POS_X+(12*10), MENU_POS_Y + 20*3, "Shrunk");
		PrintFormat(DEFAULT_SIZE, DiscFormatColors[2], MENU_POS_X+(19*10), MENU_POS_Y + 20*3, "FST");
		PrintFormat(DEFAULT_SIZE, DiscFormatColors[3], MENU_POS_X+(23*10), MENU_POS_Y + 20*3, "CISO");
		PrintFormat(DEFAULT_SIZE, DiscFormatColors[4], MENU_POS_X+(28*10), MENU_POS_Y + 20*3, "Multi");
		PrintFormat(DEFAULT_SIZE, DiscFormatColors[5], MENU_POS_X+(34*10), MENU_POS_Y + 20*3, "Over");


		// Starting position.
		int gamelist_y = MENU_POS_Y + 20*5 + 10;

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
			// NOTE: On Wii, DISC01 is GIFLAG_FORMAT_FULL.
			const u32 color = cColor ? text_color : DiscFormatColors[gi->Flags & GIFLAG_FORMAT_MASK];

			const u8 discNumber = ((gi->Flags & GIFLAG_DISCNUMBER_MASK) >> 5);
			if (discNumber == 0)
			{
				// Disc 1.
				//PrintFormat(DEFAULT_SIZE, color, MENU_POS_X, gamelist_y,
				//	    "%50.50s [%.6s]%s",
				//	    gi->Name, gi->ID,
				//	    i == ctx->games.posX ? ARROW_LEFT : " ");

				PrintFormat(DEFAULT_SIZE, color, MENU_POS_X, gamelist_y,
					    "[%.6s]%-s%s",
					    gi->ID, gi->Name,
					    i == ctx->games.posX ? ARROW_LEFT : " ");
			}
			else
			{
				// Disc 2 or higher.
				//PrintFormat(DEFAULT_SIZE, color, MENU_POS_X, gamelist_y,
				//	    "%46.46s (%d) [%.6s]%s",
				//	    gi->Name, discNumber+1, gi->ID,
				//	    i == ctx->games.posX ? ARROW_LEFT : " ");

				PrintFormat(DEFAULT_SIZE, color, MENU_POS_X, gamelist_y,
					    "[%.6s]%-s (%d) %s",
					    gi->ID, gi->Name, discNumber+1,
					    i == ctx->games.posX ? ARROW_LEFT : " ");				
			}
		}

		if(ctx->games.gamecount && (ctx->games.scrollX + ctx->games.posX) >= 0 
			&& (ctx->games.scrollX + ctx->games.posX) < ctx->games.gamecount)
		{
			ctx->games.canBeBooted = true;
			// Can we show information for the selected title?
			if (IsWiiU() && !isWiiVC) {
				// Can show information for all games on WiiU
				ctx->games.canShowInfo = true;
			} else {
				if ((ctx->games.scrollX + ctx->games.posX) == 0) {
					// Cannot show information for DISC01 on Wii and Wii VC.
					ctx->games.canShowInfo = false;
				} else {
					// Can show information for all other games.
					ctx->games.canShowInfo = true;
				}
			}

			if (ctx->games.canShowInfo) {
				// Print the selected game's filename.
				const gameinfo *const gi = &ctx->games.gi[ctx->games.scrollX + ctx->games.posX];
				const int len = strlen(gi->Path);
				const int x = (640 - (len*10)) / 2;

				const u32 color = cColor ? text_color : DiscFormatColors[gi->Flags & GIFLAG_FORMAT_MASK];
				PrintFormat(DEFAULT_SIZE, color, x, MENU_POS_Y + 20*4+5, "%s", gi->Path);
			}
		}
		else
		{
			//invalid title selected
			ctx->games.canBeBooted = false;
			ctx->games.canShowInfo = false;
		}
		// GRRLIB rendering is done by SelectGame().
	}

	return false;
}

/**
 * Get a description for the Settings menu.
 * @param ctx	[in] Menu context.
 * @return Description, or NULL if none is available.
 */
static const char *const *GetSettingsDescription(const MenuCtx *ctx)
{
	if (ctx->settings.settingPart == 0)
	{
		switch (ctx->settings.posX)
		{
			case NIN_CFG_BIT_CHEATS:
			case NIN_CFG_BIT_DEBUGGER:
			case NIN_CFG_BIT_DEBUGWAIT:
				break;

			case NIN_CFG_BIT_MEMCARDEMU: {
				static const char *desc_mcemu[] = {
					" ",
					"使用一个.raw文件来模拟",
					"插槽A当中的记忆卡.",
					"",
					"如果你想使用Wii实机的",
					"真实的记忆卡",
					"可以关闭这个选项.",
					NULL
				};
				return desc_mcemu;
			}

			case NIN_CFG_BIT_CHEAT_PATH:
				break;

			case NIN_CFG_BIT_FORCE_WIDE: {
				static const char *const desc_force_wide[] = {
					" ",
					"强制游戏以16:9的",
					"屏幕比率运行(宽屏)",
					"",
					"不是所有游戏都支持",
					"如果游戏内部就支持",
					"16:9屏幕比率运行",
					"这个选项是无效的",
					"请使用游戏当中的选项",
					"来调整显示模式.",
					NULL
				};
				return desc_force_wide;
			}

			case NIN_CFG_BIT_FORCE_PROG: {
				static const char *const desc_force_prog[] = {
					" ",
					"强制游戏以480p",
					"(逐行扫描)输出.",
					"",
					"需要色差线",
					"或者HDMI线.",
					NULL
				};
				return desc_force_prog;
			}

			case NIN_CFG_BIT_AUTO_BOOT:
				break;

			case NIN_CFG_BIT_REMLIMIT: {
				static const char *desc_remlimit[] = {
					" ",
					"磁盘读取速度限制为",
					"和真实的GC读取速度一致.",
					"",
					"取消限制可以",
					"提高读取速度",
					"但是对于那些",
					"读盘时间敏感的游戏",
					"可能会出现问题.",
					NULL
				};
				return desc_remlimit;
			}

			case NIN_CFG_BIT_OSREPORT:
				break;

			case NIN_CFG_BIT_USB: {	// WiiU Widescreen
				static const char *desc_wiiu_widescreen[] = {
					" ",
					"对于Wii U, Nintendont设置成4:3",
					"会导致屏幕两侧",
					"出现黑边.",
					"如果游戏本来支持",
					"宽屏显示",
					"打开这个选项",
					"使游戏以16:9运行.",
					"",
					"对于Wii实机",
					"这个选项是无效的.",
					NULL
				};
				return desc_wiiu_widescreen;
			}

			case NIN_CFG_BIT_LED: {
				static const char *desc_led[] = {
					" ",
					"使用LED灯作为",
					"磁盘活动的指示.",
					"",
					"LED灯将被打开",
					"在读取或者",
					"写入数据的时候.",
					"",
					"这个选项对于WiiU",
					"是无效的",
					"因为WiiU就没有LED灯.",
					NULL
				};
				return desc_led;
			}

			case NIN_CFG_BIT_LOG:
				break;

			case NIN_SETTINGS_MAX_PADS: {
				static const char *desc_max_pads[] = {
					" ",
					"设置最大的Gc手柄",
					"连接的数量.",
					"",
					"这个选项通常要设置成4",
					"来确保模拟所有借口",
					"",
					"对于WiiU和Wii家庭编辑系统",
					"这个选项是无效的",
					"因为他们的上面",
					"没有GC手柄接口.",
					NULL
				};
				return desc_max_pads;
			}

			case NIN_SETTINGS_LANGUAGE: {
				static const char *desc_language[] = {
					" ",
					"设置系统语言.",
					"",
					"这个选项通常用在",
					"PAL制式的GameCubes上,",
					"因此对于NTSC游戏",
					"是无效的.",
					NULL
				};
				return desc_language;
			}

			case NIN_SETTINGS_VIDEO:
			case NIN_SETTINGS_VIDEOMODE:
				break;

			case NIN_SETTINGS_MEMCARDBLOCKS: {
				static const char *desc_memcard_blocks[] = {
					" ",
					"新做成的记忆卡的",
					"容量大小.",
					"",
					"注意: 超过251大小",
					"的时候,",
					"可能会发生错误.",
					NULL
				};
				return desc_memcard_blocks;
			}

			case NIN_SETTINGS_MEMCARDMULTI: {
				static const char *desc_memcard_multi[] = {
					" ",
					"通常, Nintendont对于",
					"每一个游戏",
					"生成一个记忆卡文件.",
					"",
					"打开这个选项后",
					"所有的USA和PAL制式",
					"的游戏使用一个文件",
					"所有JPN制式的游戏",
					"使用另一个文件.",
					NULL
				};
				return desc_memcard_multi;
			}

			case NIN_SETTINGS_NATIVE_SI: {
				static const char *desc_native_si[] = {
					" ",
					"控制Wii系统可以",
					"使用GBA连接线.",
					"",
					"注意: 打开这个选项",
					"会关闭蓝牙和",
					"USB HID控制器.",
					"",
					"这个选项在WiiU上无效",
					"因为WiiU上面",
					"没有GC手柄接口.",
					NULL
				};
				return desc_native_si;
			}

			default:
				break;
		}
	} else /*if (ctx->settings.settingPart == 1)*/ {
		switch (ctx->settings.posX)
		{
			case 3: {
				// Triforce Arcade Mode
				static const char *desc_tri_arcade[] = {
					" ",
					"对于Triforce的游戏",
					"打开投币的功能.",
					"",
					"使用C摇杆向任意方向按下",
					"来模拟投币.",
					NULL
				};
				return desc_tri_arcade;
			}

			case 4: {
				// Wiimote CC Rumble
				static const char *desc_cc_rumble[] = {
					" ",
					"当使用Wii Classic",
					"或者Wii Classic Pro",
					"手柄的时候",
					"打开Wii手柄的震动.",
					NULL
				};
				return desc_cc_rumble;
			}

			case 5: {
				// Skip IPL
				static const char *desc_skip_ipl[] = {
					" ",
					"跳过读取GameCube的IPL",
					"即使它存在于",
					"磁盘驱动器当中.",
					NULL
				};
				return desc_skip_ipl;
			}

			case 6: {
				// BBA Emulation
				static const char *desc_skip_bba[] = {
					" ",
					"启用BBA Emulation",
					"仅支持以下游戏",
					"的所有区域版本:",
					"",
					"Mario Kart: Double Dash!!",
					"Kirby Air Ride",
					"1080 Avalanche",
					"PSO Episode 1&2",
					"PSO Episode III",
					"Homeland",
					NULL
				};
				return desc_skip_bba;
			}

			case 7: {
				// BBA Network Profile
				static const char *desc_skip_netprof[] = {
					" ",
					"强制使用网络配置文件用于",
					"BBA Emulation,该选项仅适",
					"用于WII,在WIIU上由WIIU菜",
					"单管理.",
					"这意味着你甚至可以使用无",
					"法连接到互联网的配置文件",
					".",
					NULL
				};
				return desc_skip_netprof;
			}

			default:
				break;
		}
	}

	// No description is available.
	return NULL;
}


/**
 * Update the Settings menu.
 * @param ctx	[in] Menu context.
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
			PrintFormat(MENU_SIZE, text_color, MENU_POS_X+30, SettingY(ctx->settings.posX), " " );
		} else {
			PrintFormat(MENU_SIZE, text_color, MENU_POS_X+300, SettingY(ctx->settings.posX), " " );
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
		    (ctx->settings.settingPart == 1 && ctx->settings.posX >= 9))
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
			PrintFormat(MENU_SIZE, text_color, MENU_POS_X+30, SettingY(ctx->settings.posX), " " );
		} else {
			PrintFormat(MENU_SIZE, text_color, MENU_POS_X+300, SettingY(ctx->settings.posX), " " );
		}

		ctx->settings.posX--;

		// Check for wraparound.
		if (ctx->settings.posX < 0)
		{
			ctx->settings.settingPart ^= 1;
			if (ctx->settings.settingPart == 0) {
				ctx->settings.posX = NIN_SETTINGS_LAST - 1;
			} else {
				ctx->settings.posX = 8;
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
					// Maximum native controllers.
					// Not available on Wii U.
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
				PrintFormat(MENU_SIZE, text_color, MENU_POS_X + 50, SettingY(NIN_SETTINGS_MEMCARDBLOCKS), "%29s", "");
				PrintFormat(MENU_SIZE, text_color, MENU_POS_X + 50, SettingY(NIN_SETTINGS_MEMCARDMULTI), "%29s", "");
			}
			ctx->redraw = true;
		}
		else if (ctx->settings.settingPart == 1)
		{
			// Right column.
			switch (ctx->settings.posX) {
				case 2:
					// PAL50 patch.
					ctx->saveSettings = true;
					ncfg->VideoMode ^= (NIN_VID_PATCH_PAL50);
					ctx->redraw = true;
					break;

				case 3:
					// Triforce Arcade Mode.
					ctx->saveSettings = true;
					ncfg->Config ^= (NIN_CFG_ARCADE_MODE);
					ctx->redraw = true;
					break;

				case 4:
					// Wiimote CC Rumble
					ctx->saveSettings = true;
					ncfg->Config ^= (NIN_CFG_CC_RUMBLE);
					ctx->redraw = true;
					break;

				case 5:
					// Skip IPL
					ctx->saveSettings = true;
					ncfg->Config ^= (NIN_CFG_SKIP_IPL);
					ctx->redraw = true;
					break;

				case 6:
					// BBA Emulation
					ctx->saveSettings = true;
					ncfg->Config ^= (NIN_CFG_BBA_EMU);
					ctx->redraw = true;
					break;

				case 7:
					// BBA Network Profile
					ctx->saveSettings = true;
					ncfg->NetworkProfile++;
					ncfg->NetworkProfile &= 3;
					ctx->redraw = true;
					break;

				case 8:
					// Wii U Gamepad Slot
					ctx->saveSettings = true;
					ncfg->WiiUGamepadSlot++;
					if (ncfg->WiiUGamepadSlot > NIN_CFG_MAXPAD) {
						ncfg->WiiUGamepadSlot = 0;
					}
					ctx->redraw = true;
					break;

				default:
					break;
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
				PrintFormat(MENU_SIZE, (IsWiiU() ? text_color : DARK_GRAY), MENU_POS_X+50, SettingY(ListLoopIndex),
					    "%-18s:%s", OptionStrings[ListLoopIndex], (ncfg->Config & (NIN_CFG_WIIU_WIDE)) ? "打开" : "关闭");
			} else {
				u32 item_color = text_color;
				if (IsWiiU() &&
				    (ListLoopIndex == NIN_CFG_BIT_DEBUGGER ||
				     ListLoopIndex == NIN_CFG_BIT_DEBUGWAIT ||
				     ListLoopIndex == NIN_CFG_BIT_LED))
				{
					// These options are only available on Wii.
					item_color = DARK_GRAY;
				}
				PrintFormat(MENU_SIZE, item_color, MENU_POS_X+50, SettingY(ListLoopIndex),
					    "%-18s:%s", OptionStrings[ListLoopIndex], (ncfg->Config & (1 << ListLoopIndex)) ? "打开" : "关闭" );
			}
		}

		// Maximum number of emulated controllers.
		// Not available on Wii U.
		// TODO: Disable on RVL-101?
		PrintFormat(MENU_SIZE, (!IsWiiU() ? text_color : DARK_GRAY), MENU_POS_X+50, SettingY(ListLoopIndex),
			    "%-18s:%d", OptionStrings[ListLoopIndex], (ncfg->MaxPads));
		ListLoopIndex++;

		// Language setting.
		u32 LanIndex = ncfg->Language;
		if (LanIndex < NIN_LAN_FIRST || LanIndex >= NIN_LAN_LAST) {
			LanIndex = NIN_LAN_LAST; //Auto
		}
		PrintFormat(MENU_SIZE, text_color, MENU_POS_X+50, SettingY(ListLoopIndex),
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
		PrintFormat(MENU_SIZE, text_color, MENU_POS_X+50, SettingY(ListLoopIndex),
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
			PrintFormat(MENU_SIZE, text_color, MENU_POS_X+50, SettingY(ListLoopIndex),
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
			PrintFormat(MENU_SIZE, text_color, MENU_POS_X + 50, SettingY(ListLoopIndex),
				    "%-18s:%-4d%s", OptionStrings[ListLoopIndex], MEM_CARD_BLOCKS(MemCardBlocksVal), MemCardBlocksVal > 2 ? "不稳定" : "");
			PrintFormat(MENU_SIZE, text_color, MENU_POS_X + 50, SettingY(ListLoopIndex+1),
				    "%-18s:%-4s", OptionStrings[ListLoopIndex+1], (ncfg->Config & (NIN_CFG_MC_MULTI)) ? "打开" : "关闭");
		}
		ListLoopIndex+=2;

		// Native controllers. (Required for GBA link; disables Bluetooth and USB HID.)
		// TODO: Gray out on RVL-101?
		PrintFormat(MENU_SIZE, (IsWiiU() ? DARK_GRAY : text_color), MENU_POS_X + 50, SettingY(ListLoopIndex),
			    "%-18s:%-4s", OptionStrings[ListLoopIndex],
			    (ncfg->Config & (NIN_CFG_NATIVE_SI)) ? "打开" : "关闭");
		ListLoopIndex++;

		/** Right column **/
		ListLoopIndex = 0; //reset on other side

		// Video width.
		char vidWidth[10];
		if (ncfg->VideoScale < 40 || ncfg->VideoScale > 120) {
			ncfg->VideoScale = 0;
			snprintf(vidWidth, sizeof(vidWidth), "自动");
		} else {
			snprintf(vidWidth, sizeof(vidWidth), "%i", ncfg->VideoScale + 600);
		}

		char vidOffset[10];
		if (ncfg->VideoOffset < -20 || ncfg->VideoOffset > 20) {
			ncfg->VideoOffset = 0;
		}
		snprintf(vidOffset, sizeof(vidOffset), "%i", ncfg->VideoOffset);

		char netProfile[5];
		ncfg->NetworkProfile &= 3;
		if(ncfg->NetworkProfile == 0)
			snprintf(netProfile, sizeof(netProfile), "Auto");
		else
			snprintf(netProfile, sizeof(netProfile), "%i", ncfg->NetworkProfile);

		PrintFormat(MENU_SIZE, text_color, MENU_POS_X + 320, SettingY(ListLoopIndex),
			    "%-18s:%-4s", "显示宽度          ", vidWidth);
		ListLoopIndex++;
		PrintFormat(MENU_SIZE, text_color, MENU_POS_X + 320, SettingY(ListLoopIndex),
			    "%-18s:%-4s", "屏幕位置          ", vidOffset);
		ListLoopIndex++;

		// Patch PAL60.
		PrintFormat(MENU_SIZE, text_color, MENU_POS_X + 320, SettingY(ListLoopIndex),
			    "%-18s:%-4s", "PAL50补丁         ", (ncfg->VideoMode & (NIN_VID_PATCH_PAL50)) ? "打开" : "关闭");
		ListLoopIndex++;

		// Triforce Arcade Mode.
		PrintFormat(MENU_SIZE, text_color, MENU_POS_X + 320, SettingY(ListLoopIndex),
			    "%-18s:%-4s", "TRI街机模式       ", (ncfg->Config & (NIN_CFG_ARCADE_MODE)) ? "打开" : "关闭");
		ListLoopIndex++;

		// Wiimote CC Rumble
		PrintFormat(MENU_SIZE, text_color, MENU_POS_X + 320, SettingY(ListLoopIndex),
			    "%-18s:%-4s", "Wiimote控制器 震动", (ncfg->Config & (NIN_CFG_CC_RUMBLE)) ? "打开" : "关闭");
		ListLoopIndex++;

		// Skip GameCube IPL
		PrintFormat(MENU_SIZE, text_color, MENU_POS_X + 320, SettingY(ListLoopIndex),
			    "%-18s:%-4s", "跳过IPL           ", (ncfg->Config & (NIN_CFG_SKIP_IPL)) ? "是" : "否");
		ListLoopIndex++;

		// BBA Emulation
		PrintFormat(MENU_SIZE, text_color, MENU_POS_X + 320, SettingY(ListLoopIndex),
			    "%-18s:%-4s", "BBA Emulation", (ncfg->Config & (NIN_CFG_BBA_EMU)) ? "打开" : "关闭");
		ListLoopIndex++;

		// BBA Network Profile
		PrintFormat(MENU_SIZE, (IsWiiU() || !(ncfg->Config & (NIN_CFG_BBA_EMU))) ? DARK_GRAY : text_color,
				MENU_POS_X + 320, SettingY(ListLoopIndex),
			    "%-18s:%-4s", "网络配置文件      ", netProfile);
		ListLoopIndex++;

		// Controller slot for the Wii U gamepad.
		if (ncfg->WiiUGamepadSlot < NIN_CFG_MAXPAD) {
			PrintFormat(MENU_SIZE, (IsWiiU() ? text_color : DARK_GRAY), MENU_POS_X+320, SettingY(ListLoopIndex),
					"%-18s:%d", "WiiU Gamepad Slot", (ncfg->WiiUGamepadSlot + 1));
		} else {
			PrintFormat(MENU_SIZE, (IsWiiU() ? text_color : DARK_GRAY), MENU_POS_X+320, SettingY(ListLoopIndex),
			"%-18s:%-4s", "WiiU Gamepad Slot", "None");	
		}
		ListLoopIndex++;

		// Draw the cursor.
		u32 cursor_color = text_color;
		if (ctx->settings.settingPart == 0) 
		{
			if 
			(
				(!IsWiiU() && ctx->settings.posX == NIN_CFG_BIT_USB) ||
				(IsWiiU() && ctx->settings.posX == NIN_CFG_NATIVE_SI)
			)
			{
				// Setting is not usable on this platform.
				// Gray out the cursor, too.
				cursor_color = DARK_GRAY;
			}
			PrintFormat(MENU_SIZE, cursor_color, MENU_POS_X + 30, SettingY(ctx->settings.posX), ARROW_RIGHT);
		} 
		else
		{
			if((IsWiiU() || !(ncfg->Config & (NIN_CFG_BBA_EMU))) && ctx->settings.posX == 7)			
				cursor_color = DARK_GRAY;
			
			if(!IsWiiU() && ctx->settings.posX == 8)
			{
				cursor_color = DARK_GRAY;
			}

			PrintFormat(MENU_SIZE, cursor_color, MENU_POS_X + 300, SettingY(ctx->settings.posX), ARROW_RIGHT);
		}

		// Print a description for the selected option.
		// desc contains pointers to lines, and is
		// terminated with NULL.
		const char *const *desc = GetSettingsDescription(ctx);
		if (desc != NULL)
		{
			int line_num = 9;
			do {
				if (**desc != 0)
				{
					PrintFormat(MENU_SIZE, text_color, MENU_POS_X + 300, SettingY(line_num), *desc);
				}
				line_num++;
			} while (*(++desc) != NULL);
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

		case DEV_NO_TITLES:
			// "games" directory appears to be empty.
			// The list will still be shown, since there's a
			// "Boot GC Disc in Drive" option on Wii.
			gprintf("WARNING: %s:/games/ contains no GC titles.\n", GetRootDevice());
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
		if(Shutdown)
			LoaderShutdown();

		if( FPAD_Start(0) )
		{
			// Go back to the Device Select menu.
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
			if (ctx.menuMode == 0)
			{
				// Game List menu.
				PrintButtonActions("返 回", NULL, "设 置", NULL);
				// If the selected game bootable, enable "Select".
				u32 color = ((ctx.games.canBeBooted) ? text_color : DARK_GRAY);
				PrintFormat(DEFAULT_SIZE, color, MENU_POS_X + 430, MENU_POS_Y + 20*1, "A   : 选 择");
				// If the selected game is not DISC01, enable "Game Info".
				color = ((ctx.games.canShowInfo) ? text_color : DARK_GRAY);
				PrintFormat(DEFAULT_SIZE, color, MENU_POS_X + 430, MENU_POS_Y + 20*3-1, "X/1 : 游戏信息");
			}
			else
			{
				// Settings menu.
				PrintButtonActions("返 回", "选 择", "设 置", "更 新");
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

	if(ctx.games.canBeBooted)
	{
		// Save the selected game to the configuration.
		u32 SelectedGame = ctx.games.posX + ctx.games.scrollX;
		const char* StartChar = gi[SelectedGame].Path + 3;
		if (StartChar[0] == ':') {
			StartChar++;
		}
		strncpy(ncfg->GamePath, StartChar, sizeof(ncfg->GamePath));
		ncfg->GamePath[sizeof(ncfg->GamePath)-1] = 0;
		memcpy(&(ncfg->GameID), gi[SelectedGame].ID, 4);
		DCFlushRange((void*)ncfg, sizeof(NIN_CFG));
	}
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
		if(Shutdown)
			LoaderShutdown();

		if (redraw)
		{
			UseSD = (ncfg->Config & NIN_CFG_USB) == 0;
			PrintInfo();
			PrintButtonActions("退 出", "选 择", NULL, NULL);
			PrintFormat(DEFAULT_SIZE, text_color, MENU_POS_X + 53 * 6 - 8, MENU_POS_Y + 20 * 6, UseSD ? ARROW_LEFT : "");
			PrintFormat(DEFAULT_SIZE, text_color, MENU_POS_X + 53 * 6 - 8, MENU_POS_Y + 20 * 7, UseSD ? "" : ARROW_LEFT);
			PrintFormat(DEFAULT_SIZE, text_color, MENU_POS_X + 47 * 6 - 8, MENU_POS_Y + 20 * 6, " SD  ");
			PrintFormat(DEFAULT_SIZE, text_color, MENU_POS_X + 47 * 6 - 8, MENU_POS_Y + 20 * 7, "USB  ");
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
			ShowMessageScreenAndExit("返回到loader...", 0);
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
	PrintFormat(DEFAULT_SIZE, text_color, x, 232, "%s", msg);
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
	const u32 color = (ret == 0 ? text_color : MAROON);

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
	const char *consoleType = (isWiiVC ? (IsWiiUFastCPU() ? "WiiVC 5x CPU" : "Wii VC") : (IsWiiUFastCPU() ? "WiiU 5x CPU" : (IsWiiU() ? "Wii U" : "Wii")));
#ifdef NIN_SPECIAL_VERSION
	// "Special" version with customizations. (Not mainline!)
	PrintFormat(DEFAULT_SIZE, text_color, MENU_POS_X, MENU_POS_Y + 20*0, "Nintendont Loader v%u.%u" NIN_SPECIAL_VERSION " (%s)",
		    NIN_VERSION>>16, NIN_VERSION&0xFFFF, consoleType);
#else
	PrintFormat(DEFAULT_SIZE, text_color, MENU_POS_X, MENU_POS_Y + 20*0, "Nintendont Loader v%u.%u (%s)",
		    NIN_VERSION>>16, NIN_VERSION&0xFFFF, consoleType);
#endif
	PrintFormat(DEFAULT_SIZE, text_color, MENU_POS_X, MENU_POS_Y + 20*1, "编译日期: " __DATE__ " " __TIME__);
	PrintFormat(DEFAULT_SIZE, text_color, MENU_POS_X, MENU_POS_Y + 20*2, "固件    : %u.%u.%u",
		    *(vu16*)0x80003140, *(vu8*)0x80003142, *(vu8*)0x80003143);
}

/**
 * Print button actions.
 * Call this function after PrintInfo().
 *
 * If any button action is NULL, that button won't be displayed.
 *
 * @param btn_home	[in,opt] Home button action.
 * @param btn_a		[in,opt] A button action.
 * @param btn_b		[in,opt] B button action.
 * @param btn_x1	[in,opt] X/1 button action.
 */
void PrintButtonActions(const char *btn_home, const char *btn_a, const char *btn_b, const char *btn_x1)
{
	if (btn_home) {
		PrintFormat(DEFAULT_SIZE, text_color, MENU_POS_X + 430, MENU_POS_Y + 20*0, "Home: %s", btn_home);
	}
	if (btn_a) {
		PrintFormat(DEFAULT_SIZE, text_color, MENU_POS_X + 430, MENU_POS_Y + 20*1, "A   : %s", btn_a);
	}
	if (btn_b) {
		PrintFormat(DEFAULT_SIZE, text_color, MENU_POS_X + 430, MENU_POS_Y + 20*2, "B   : %s", btn_b);
	}
	if (btn_x1) {
		PrintFormat(DEFAULT_SIZE, text_color, MENU_POS_X + 430, MENU_POS_Y + 20*3-1, "X/1 : %s", btn_x1);
	}
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
				"警告: 无法读取%s FAT设备", s_devType);
			break;
		case DEV_NO_GAMES:
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*4,
				"警告: 目录不存在： %s:/games/ ", GetRootDevice());
			break;
		case DEV_NO_TITLES:
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*4,
				"警告: 目录中不存在游戏： %s:/games/ ", GetRootDevice());
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
void PrintLoadKernelError(LoadKernelError_t iosErr, int err)
{
	ClearScreen();
	PrintInfo();
	PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*4, "加载IOS58失败:");

	switch (iosErr)
	{
		case LKERR_UNKNOWN:
		default:
			// TODO: Add descriptions of more LoadKernel() errors.
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*5, "加载核心方法中发生 %d 错误, 返回 %d.", iosErr, err);
			break;

		case LKERR_ES_GetStoredTMDSize:
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*5, "ES_GetStoredTMDSize() 返回了 %d.", err);
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*7, "这说明IOS58没有被安装.");
			if (IsWiiU())
			{
				// No IOS58 on Wii U should never happen...
				PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*9, "警告: 表明在Wii U上缺少IOS58");
				PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*10, "vWii的设置中，可能存在严重错误.");
			}
			else
			{
				// TODO: Check if we're using System 4.3.
				PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*9, "请更新Wii的系统版本至4.3，");
				PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*10, "然后再次运行Nintendont.");
			}
			break;

		case LKERR_TMD_malloc:
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*5, "无法从IOS58 TMD中分配内存.");
			break;

		case LKERR_ES_GetStoredTMD:
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*5, "ES_GetStoredTMD() 返回了 %d.", err);
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*7, "警告: IOS58可能坏掉了.");
			break;

		case LKERR_IOS_Open_shared1_content_map:
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*5, "IOS_Open(\"/shared1/content.map\") 返回了 %d.", err);
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*7, "通常情况下说明，Nintendont没有被赋予");
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*8, "从AHB启动的权限.");
			// FIXME: Create meta.xml if it isn't there?
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*10, "请确保meta.xml存在于你的Nintendont应用程序目录中");
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*11, "并确保它包含一个");
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*12, "带有<ahb_access/>标签的行.");
			break;

		case LKERR_IOS_Open_IOS58_kernel:
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*5, "IOS_Open(IOS58核心) 返回了 %d.", err);
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*7, "警告: IOS58可能坏掉了.");
			break;

		case LKERR_IOS_Read_IOS58_kernel:
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*5, "IOS_Open(IOS58核心) 返回了 %d.", err);
			PrintFormat(DEFAULT_SIZE, MAROON, MENU_POS_X, MENU_POS_Y + 20*7, "警告: IOS58可能坏掉了.");
			break;
	}
}
