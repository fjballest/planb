typedef struct Keysym Keysym;
struct Keysym{
	Rune r;
	uint keysym;
	int isshift;
	int iscontrol;
	int isaltgr;
};

// This mapping really depends on the keyboard (iscontrol and isshift)
//  This one is for a US keyboard.

Keysym ktab[]={
	{L'\n', XK_Return, 0, 0, 0},
	{L'\n', XK_Return, 0, 0, 0},
	{0x7f, XK_Delete, 0, 0, 0},
	{L'\e', XK_Escape, 0, 0, 0},
	{L'\b', XK_BackSpace, 0, 0, 0},
	{L'\t', XK_Tab, 0, 0, 0},
	{L'|', XK_bar, 1, 0, 0},
	{L'@', XK_at, 1, 0, 0},
	{L'^', XK_asciicircum, 0, 0, 0},
	{L'#', XK_numbersign, 1, 0, 0},
	{L'$', XK_dollar, 1, 0, 0},
	{L'%', XK_percent, 1, 0, 0},
	{L'&', XK_ampersand, 1, 0, 0},
	{L'/', XK_slash, 1, 0, 0},
	{L'"', XK_quotedbl, 1, 0, 0},
	{L'(', XK_parenleft, 1, 0, 0},
	{L')', XK_parenright, 1, 0, 0},
	{L'=', XK_equal, 0, 0, 0},
	{L'>', XK_greater, 1, 0, 0},
	{L'<', XK_less, 0, 0, 0},
	{L';', XK_semicolon, 0, 0, 0},
	{L':', XK_colon, 1, 0, 0},
	{L'\\', XK_backslash, 0, 0, 0},
	{L'-', XK_minus , 0, 0, 0},
	{L'?', XK_question, 0, 0, 0},
	{L'[', XK_bracketleft, 1, 0, 0},
	{L']', XK_bracketright, 1, 0, 0},
	{L'[', XK_braceright, 0, 0, 0},
	{L']', XK_braceleft, 0, 0, 0},
	{L'*', XK_asterisk, 1, 0, 0},
	{L'!', XK_exclam, 1, 0, 0},
	{L'+', XK_plus, 1, 0, 0},
	{L' ', XK_space, 0, 0, 0},
	{L'~', XK_asciitilde, 1, 0, 0},
	{L'_', XK_underscore, 1, 0, 0},
	{0x7f, XK_Delete, 0, 0, 0},
	{0xf014, XK_Insert, 0, 0, 0},
	{0xf00e, XK_Up, 0, 0, 0},
	{0xf012, XK_Right, 0, 0, 0},
	{0xf011, XK_Left, 0, 0, 0},
	{0xf800, XK_Down, 0, 0, 0},
	{0xf00f, XK_Page_Up, 0, 0, 0},
	{0xf013, XK_Page_Down, 0, 0, 0},
	{0xff89, XK_Tab, 0, 0, 0},
	{ 0, 0, 0, 0, 0}
};
