#include "shod.h"

struct Config config = {
	/*
	 * except for the foreground, colors fields are strings
	 * containing three elements delimited by colon:
	 * the body color, the color of the light 3D shadow,
	 * and the color of the dark 3D shadow.
	 */

	/* command to spawn when clicking the menu button */
	.menucmd        = "shodmenu",

	/* 0-or-1 flags */
	.floatdialog    = 0,            /* set to 1 to use floating dialog windows */
	.sloppyfocus    = 0,            /* set to 1 to use sloppy container focus */
	.sloppytiles    = 0,            /* set to 1 to use sloppy window focus */
	.honorconfig    = 0,            /* set to 1 to honor configure requests */
	.disablealttab  = 0,            /* set to 1 to disable alt-tab */
	.disablehidden  = 0,            /* set to 1 to disable notification of hidden window as minimized */

	/* general configuration */
	.altkeysym      = XK_Alt_L,     /* XK_Alt_L for Alt, XK_Super_L for Super (aka WinKey) */
	.tabkeysym      = XK_Tab,       /* tab key; you should probably not change this */
	.modifier       = Mod1Mask,     /* Mod1Mask for Alt, Mod4Mask for Super */
	.snap           = 8,            /* proximity of container edges to perform snap attraction */
	.font = "monospace:pixelsize=11", /* font for titles in titlebars */
	.ndesktops      = 10,           /* number of desktops per monitor */
	.movetime       = 32,           /* time (ms) to redraw containers during moving */
	.resizetime     = 64,           /* time (ms) to redraw containers during resizing */

	/* dock configuration */
	.dockwidth      = 64,           /* width of the dock (or its height, if it is horizontal) */
	.dockspace      = 64,           /* size of each dockapp (64 for windowmaker dockapps) */
	.dockgravity    = "E",          /* placement of the dock */

	/* notification configuration */
	.notifgap       = 3,            /* gap, in pixels, between notifications */
	.notifgravity   = "NE",         /* placement of notifications */

	/* title bar */
	.titlewidth = 17,

	/* border */
	.borderwidth = 6,

	/* colors */
	.colors = {
		/* (border)        MIDDLE      LIGHT      DARK    */
		[FOCUSED]     = {"#3465A4",   "#729FCF", "#204A87"},
		[UNFOCUSED]   = {"#555753",   "#888A85", "#2E3436"},
		[URGENT]      = {"#CC0000",   "#EF2929", "#A40000"},

		/* (dock, text)   BACKGROUND   BORDER     FOREGROUND */
		[STYLE_OTHER] = {"#121212",   "#2E3436", "#FFFFFF"},
	},

	/* size of 3D shadow effect, must be less than borderwidth */
	.shadowthickness = 2,

	/* the following are hardcoded rules; use X Resources to set rules without recompiling */
	.rules = (struct Rule []){
		/* class  instance  role                 type          state   desktop*/ 

		/* Although Firefox's PictureInPicture is technically a utility (sub)window, make it a normal one */
		{ NULL,   NULL,     "PictureInPicture",  TYPE_NORMAL,  ABOVE,  0 },

		/* Last rule must be all NULL! */
		{ NULL,   NULL,     NULL,                TYPE_NORMAL,  0,      0 },
	}
};
