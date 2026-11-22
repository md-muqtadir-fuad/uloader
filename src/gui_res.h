/*
 * U-loader window - resource and command IDs (shared by gui.c and gui.rc).
 *
 * The IDM_* commands can also be sent to the main window as WM_COMMAND
 * (wParam = IDM_xxx, lParam = 0); they act on the selected downloads.
 */
#ifndef GUI_RES_H
#define GUI_RES_H

#define IDI_APP             1

#define IDD_ADD             100
#define IDD_SETTINGS        101
#define IDD_BROWSER         102   /* "Add U-loader to your browser" (#13) */

#define IDC_B_EDGE          1201
#define IDC_B_CHROME        1202
#define IDC_B_FOLDER        1203
#define IDC_B_PATH          1204
#define IDC_B_STATUS        1205
#define IDC_B_NOSHOW        1206
#define IDC_B_COPY          1207

/* Add dialog */
#define IDC_LINKS           1001
#define IDC_DIR             1002
#define IDC_BROWSE          1003
#define IDC_CONNS           1004
#define IDC_CONNS_SPIN      1005
#define IDC_QUALITY         1006
#define IDC_MODE            1007
#define IDC_START_NOW       1008
#define IDC_ADD_PAUSED      1009
#define IDC_LINKS_HINT      1010

/* Settings dialog */
#define IDC_S_DIR           1101
#define IDC_S_BROWSE        1102
#define IDC_S_CONNS         1103
#define IDC_S_CONNS_SPIN    1104
#define IDC_S_MAXACT        1105
#define IDC_S_MAXACT_SPIN   1106
#define IDC_S_LIMIT         1107
#define IDC_S_CLIP          1108
#define IDC_S_TRAY          1109
#define IDC_S_CONNECT       1110
#define IDC_S_EXTPATH       1111
#define IDC_S_EXTLINK       1112
#define IDC_S_YTDLP         1113
#define IDC_S_YTSTATUS      1114
#define IDC_S_OUTPUT        1115
#define IDC_S_VERSION       1116
#define IDC_S_CATS          1117   /* sort into folders by type (#1)    */
#define IDC_S_LIMITON       1118   /* total speed limit on/off (#2)     */
#define IDC_S_UPDATES       1119   /* check for updates automatically (#10, #11) */

/* Commands */
#define IDM_ADD             40001
#define IDM_RESUME          40002
#define IDM_PAUSE           40003
#define IDM_PAUSEALL        40004
#define IDM_DELETE          40005
#define IDM_DELETEFILE      40006
#define IDM_OPEN            40007
#define IDM_OPENFOLDER      40008
#define IDM_SETTINGS        40009
#define IDM_COPYLINK        40010
#define IDM_PASTE           40011
#define IDM_SELECTALL       40012
#define IDM_SHOW            40013
#define IDM_EXIT            40014
#define IDM_RESUMEALL       40015
#define IDM_LOGCOPY         40016
#define IDM_LOGCOPYALL      40017
#define IDM_LIMITER         40018   /* speed limiter on/off (#2) */
#define IDM_CHECKUPDATE     40019   /* tray menu: check for updates now (#11) */
#define IDM_UPDATEASK       40020   /* internal: ask "update now?" (#11) */
#define IDM_BROWSERGUIDE    40021   /* "Add U-loader to your browser" window (#13) */

#endif
