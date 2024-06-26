/*
	modded for libretro-frodo
*/

/*
  Hatari - dlgMain.c

  This file is distributed under the GNU Public License, version 2 or at
  your option any later version. Read the file gpl.txt for details.

  The main dialog.
*/

#include "dialog.h"
#include "sdlgui.h"

#define MAINDLG_ABOUT    2
#define MAINDLG_FLOPPYS  3
#define MAINDLG_VIDEO    4
#define MAINDLG_MISC     5
#define MAINDLG_JOYSTICK 6
#define MAINDLG_SOUND    7
#define MAINDLG_LOADCFG  8
#define MAINDLG_SAVECFG  9
#define MAINDLG_NORESET  10
#define MAINDLG_RESET    11
#define MAINDLG_OK       12
#define MAINDLG_QUIT     13
#define MAINDLG_CANCEL   14
#define MAINDLG_SNAPSHOT 15

/* The main dialog: */
static SGOBJ maindlg[] =
{
	{ SGBOX, 0, 0, 0,0, 50,19, NULL },
	{ SGTEXT, 0, 0, 17,1, 16,1, (char*)"Frodo main menu" },
	{ SGBUTTON, SG_EXIT/*0*/, 0, 2,4, 13,1, (char*)"About" },
	{ SGBUTTON, SG_EXIT/*0*/, 0, 17,4, 16,1, (char*)"Floppy" },
	{ SGBUTTON, SG_EXIT/*0*/, 0, 17,8, 16,1, (char*)"Video" },
	{ SGBUTTON, SG_EXIT/*0*/, 0, 35,4, 13,1, (char*)"Misc" },
	{ SGBUTTON, SG_EXIT/*0*/, 0, 2,8, 13,1, (char*)"Joy" },
	{ SGBUTTON, SG_EXIT/*0*/, 0, 35,8, 13,1, (char*)"Sound" },
	{ SGRADIOBUT, 0, 0, 3,15, 15,1, (char*)"No Reset" },
	{ SGRADIOBUT, 0, 0, 3,17, 15,1, (char*)"Reset" },
	{ SGBUTTON, SG_EXIT/*SG_DEFAULT*/, 0, 21,15, 8,3, (char*)"OK" },
	{ SGBUTTON, SG_EXIT/*0*/, 0, 36,15, 10,1, (char*)"Quit" },
	{ SGBUTTON, SG_EXIT/*SG_CANCEL*/, 0, 36,17, 10,1, (char*)"Cancel" },
	{ SGBUTTON, SG_EXIT, 0, 17,12, 16,1, (char*)"Snapshot" },
	{ -1, 0, 0, 0,0, 0,0, NULL }
};

/**
 * This functions sets up the actual font and then displays the main dialog.
 */
int Dialog_MainDlg(bool *bReset, bool *bLoadedSnapshot)
{
   int retbut=0;
   bool bOldMouseVisibility;
   int nOldMouseX, nOldMouseY;
   char *psNewCfg;

   *bReset = false;
   *bLoadedSnapshot = false;

   if (SDLGui_SetScreen())
      return false;

   SDLGui_CenterDlg(maindlg);

   maindlg[MAINDLG_NORESET].state |= SG_SELECTED;
   maindlg[MAINDLG_RESET].state &= ~SG_SELECTED;

   prefs = new Prefs(ThePrefs);

   do
   {       
      retbut = SDLGui_DoDialog(maindlg, NULL);
      switch (retbut)
      {
         case MAINDLG_ABOUT:
            Dialog_AboutDlg();
            break;

         case MAINDLG_FLOPPYS:
            DlgFloppy_Main();
            break;

         case MAINDLG_VIDEO:
            Dialog_VideoDlg();
            break;

         case MAINDLG_MISC:
            Dialog_MiscDlg();
            break;

         case MAINDLG_JOYSTICK:
            Dialog_JoyDlg();
            break;

         case MAINDLG_SOUND:
            Dialog_SoundDlg();
            break;

         case MAINDLG_LOADCFG:
         case MAINDLG_SAVECFG:
            break;

         case MAINDLG_SNAPSHOT:
            Dialog_SnapshotDlg();
            break;

         case MAINDLG_QUIT:
            bQuitProgram = true;
            break;
      }

      if(ThePrefs!=*prefs)
      {
         TheC64->NewPrefs(prefs);
         ThePrefs = *prefs;
      }

      gui_poll_events();
   }
   while (retbut != MAINDLG_OK && retbut != MAINDLG_CANCEL && retbut != SDLGUI_QUIT
         && retbut != SDLGUI_ERROR && !bQuitProgram);


   if (maindlg[MAINDLG_RESET].state & SG_SELECTED)
      *bReset = true;

   delete prefs;

   return (retbut == MAINDLG_OK);
}
