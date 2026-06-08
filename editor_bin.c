/*
 * editor_bin.c - standalone Unicode Win32 editor for the Sraz (DOS) quiz BINARY
 * data files. Decodes 0011.200 + 0011.100 on open, re-encodes them on save.
 * Single compilation unit; no external tools or text files needed.
 *
 *   build (MSYS2 mingw64):
 *     export PATH="/c/msys64/mingw64/bin:$PATH"
 *     gcc -O2 -mwindows -o editor_bin.exe editor_bin.c -lcomctl32 -lcomdlg32 -lgdi32 -luser32
 *
 * Encoding
 * --------
 * The game stores text in YUSCII (the 7-bit Yugoslav/Croatian ASCII variant),
 * rendered by a custom DOS font. A few byte positions stand for Croatian letters;
 * this editor maps them to/from real Unicode so they display and edit correctly
 * (and the original Croatian data round-trips losslessly):
 *
 *     0x40 @ = Z(caron)   0x5B [ = S(caron)  0x5C \ = Dj   0x5D ] = C(acute)  0x5E ^ = C(caron)
 *     0x60 ` = z(caron)   0x7B { = s(caron)  0x7C | = dj   0x7D } = c(acute)  0x7E ~ = c(caron)
 *
 * Layout of 0011.200
 * ------------------
 *     [0      .. 0x366c)   header / graphics blob (preserved verbatim)
 *     [0x366c .. 0x36bb)   9 category-name records  (Sport..Random)
 *     [0x36bb .. EOF  )    question records, packed Q,A1,A2,A3,A4
 *
 * Each record is a length-prefixed, ciphered string:
 *     plain  = (stored - off + pos + 1) & 0xFF
 *     stored = (plain  + off - pos - 1) & 0xFF      (off = 0x80 question / 0x1E answer & name)
 * The FIRST answer is the correct one (the game shuffles the four on screen).
 *
 * 0011.100 = 8 category-boundary uint32 (LE) + per-question start offsets + EOF.
 * The header blob, the 8 category boundaries, and the spare trailing record are
 * all preserved from the opened file, so unchanged data round-trips byte-for-byte.
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>


#define START   0x36bbL          /* first question record offset                 */
#define CATOFF  0x366cL          /* 9 category-name records sit here, before that */
#define NQ      1000             /* maximum questions                            */
#define NCAT    8                /* categories (the 9th name is "Random")        */
#define SLEN    256              /* string buffer length                         */

/* control / menu ids */
#define IDC_TAB      100
#define IDC_LIST     101
#define IDC_QEDIT    102
#define IDC_A0       110         /* answer edits         110..113 */
#define IDC_N0       120         /* answer number labels 120..123 */
#define IDC_SAVE     130
#define IDC_REVERT   131
#define IDC_RENAME   132
#define IDC_STATUS   140
#define IDC_QLABEL   141
#define IDC_ALABEL   142
#define IDC_CATLABEL 143
#define IDC_CATEDIT  144
#define IDM_OPEN     200
#define IDM_SAVE     201
#define IDM_SAVEAS   202
#define IDM_EXIT     203


typedef struct {
    wchar_t q[SLEN];
    wchar_t a[4][SLEN];
} Question;

static Question      g_q[NQ];
static int           g_count;

static unsigned char g_header[START];               /* graphics/header blob, preserved      */
static unsigned long g_cathdr[NCAT];                /* the 8 category-boundary values       */
static wchar_t       g_catname[NCAT + 1][SLEN];     /* names (8 categories + "Random")      */
static int           g_ncat_names;

static unsigned char g_spare[4096];                 /* spare record after Q1000, kept as-is */
static long          g_sparelen;

static int           g_loaded;
static int           g_dirty;
static int           g_curTab;
static int           g_curIdx = -1;

static wchar_t       g_path200[MAX_PATH];
static wchar_t       g_path100[MAX_PATH];

static HWND  g_main, g_tab, g_list;
static HWND  g_qedit, g_aedit[4], g_anum[4];
static HWND  g_status, g_qlabel, g_alabel, g_catlabel, g_catedit;
static HFONT g_font;

/* generic fallback labels - real category names are read from the file when present */
static const wchar_t *CATS[NCAT] = {
    L"Tab 1", L"Tab 2", L"Tab 3", L"Tab 4",
    L"Tab 5", L"Tab 6", L"Tab 7", L"Tab 8"
};


typedef struct {
    unsigned char b;    /* stored YUSCII byte   */
    wchar_t       w;    /* Unicode code point   */
} YuPair;

static const YuPair YU[10] = {
    { 0x40, 0x017D }, { 0x5B, 0x0160 }, { 0x5C, 0x0110 }, { 0x5D, 0x0106 }, { 0x5E, 0x010C },  /* Z S Dj C C */
    { 0x60, 0x017E }, { 0x7B, 0x0161 }, { 0x7C, 0x0111 }, { 0x7D, 0x0107 }, { 0x7E, 0x010D },  /* z s dj c c */
};

static wchar_t yu_to_w(unsigned char b)
{
    for (int i = 0; i < 10; i++)
        if (YU[i].b == b)
            return YU[i].w;

    return (b >= 0x20 && b < 0x7f) ? (wchar_t)b : L'?';
}

static unsigned char w_to_yu(wchar_t w)
{
    for (int i = 0; i < 10; i++)
        if (YU[i].w == w)
            return YU[i].b;

    if (w >= 0x20 && w < 0x7f) {
        /* a literal @ [ \ ] ^ ` { | } ~ would be shown as a Croatian letter, so drop it */
        for (int i = 0; i < 10; i++)
            if (YU[i].b == (unsigned char)w)
                return '?';
        return (unsigned char)w;
    }

    return '?';
}

static unsigned char *read_all(const wchar_t *path, long *n)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f)
        return NULL;

    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    rewind(f);

    unsigned char *b = (unsigned char *)malloc(*n > 0 ? *n : 1);
    if (b && *n > 0)
        fread(b, 1, *n, f);

    fclose(f);
    return b;
}

static unsigned long u32le(const unsigned char *p)
{
    return  (unsigned long)p[0]
         | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16)
         | ((unsigned long)p[3] << 24);
}

static void put_u32le(FILE *f, unsigned long v)
{
    fputc( v        & 0xff, f);
    fputc((v >>  8) & 0xff, f);
    fputc((v >> 16) & 0xff, f);
    fputc((v >> 24) & 0xff, f);
}

/* decode one record at 'off' into 'dst'; return the offset of the next record. */
static long dec_record(const unsigned char *d, long off, int ans, wchar_t *dst)
{
    int realln = d[off];
    int n      = realln;
    if (n >= SLEN)
        n = SLEN - 1;

    for (int c = 0; c < n; c++) {
        int v = (d[off + 1 + c] - (ans ? 0x1e : 0x80) + c + 1) & 0xff;
        dst[c] = yu_to_w((unsigned char)v);
    }
    dst[n] = 0;

    return off + 1 + realln;
}

/* encode one string into 'out' as [len][ciphered bytes]; return bytes written. */
static long enc_string(const wchar_t *t, int ans, unsigned char *out)
{
    int  off = ans ? 0x1e : 0x80;
    long n   = (long)wcslen(t);
    if (n > 255)
        n = 255;

    out[0] = (unsigned char)n;
    for (long c = 0; c < n; c++) {
        unsigned char yb = w_to_yu(t[c]);
        out[1 + c] = (unsigned char)((yb + off - c - 1) & 0xff);
    }

    return n + 1;
}

static int load_binary(const wchar_t *p200, const wchar_t *p100)
{
    long s200, s100;

    g_ncat_names = 0;

    unsigned char *d200 = read_all(p200, &s200);
    unsigned char *d100 = read_all(p100, &s100);
    if (!d200 || !d100 || s200 < START || s100 < NCAT * 4) {
        free(d200);
        free(d100);
        return 0;
    }

    /* the 8 category-boundary values + the header/graphics blob */
    for (int i = 0; i < NCAT; i++)
        g_cathdr[i] = u32le(d100 + i * 4);
    memcpy(g_header, d200, START);

    /* category names sit at CATOFF, just before the questions (answer cipher) */
    for (long o = CATOFF; o < START && g_ncat_names < NCAT + 1; )
        o = dec_record(d200, o, 1, g_catname[g_ncat_names++]);

    for (int c = g_ncat_names; c < NCAT + 1; c++)
        wcscpy(g_catname[c], c < NCAT ? CATS[c] : L"Random");

    /* questions, located through the 0011.100 offset table */
    g_count = (int)g_cathdr[NCAT - 1];
    if (g_count < 0 || g_count > NQ)
        g_count = NQ;

    for (int i = 0; i < g_count; i++) {
        long e = 8 + i;
        if ((e + 1) * 4 > s100) {
            g_count = i;
            break;
        }

        long off = (long)u32le(d100 + e * 4);
        if (off < CATOFF || off >= s200) {      /* questions sit after the names region */
            g_count = i;
            break;
        }

        off = dec_record(d200, off, 0, g_q[i].q);
        for (int a = 0; a < 4 && off < s200; a++)
            off = dec_record(d200, off, 1, g_q[i].a[a]);
    }

    /* keep the spare record group (after the last question) byte-for-byte */
    g_sparelen = 0;
    long n100 = s100 / 4;
    if (8 + g_count < n100) {
        long sp = (long)u32le(d100 + (8 + g_count) * 4);
        long ef = (long)u32le(d100 + (n100 - 1) * 4);
        if (sp >= START && ef > sp && ef <= s200 && (ef - sp) <= (long)sizeof(g_spare)) {
            g_sparelen = ef - sp;
            memcpy(g_spare, d200 + sp, g_sparelen);
        }
    }

    if (g_sparelen == 0) {              /* fallback: 5 empty strings */
        g_sparelen = 5;
        memset(g_spare, 0, 5);
    }

    free(d200);
    free(d100);

    g_loaded = 1;
    g_dirty  = 0;
    return 1;
}

static int save_binary(const wchar_t *p200, const wchar_t *p100)
{
    unsigned char *body = (unsigned char *)malloc(NQ * 1500 + 1024);
    if (!body)
        return 0;

    /* re-encode the 9 category names into the 0x366c region; the questions follow,
       so editing a name simply shifts every question offset. */
    static unsigned char nbuf[4096];
    long nlen = 0;
    for (int c = 0; c < NCAT + 1; c++)
        nlen += enc_string(g_catname[c], 1, nbuf + nlen);

    /* pad with zeros so the questions stay at their original 0x36bb start whenever the
       names fit; this keeps the file layout original-compatible and avoids moving the
       questions before START (which the game/loader keys off). */
    while (nlen < (long)(START - CATOFF))
        nbuf[nlen++] = 0;

    long qbase = CATOFF + nlen;

    /* encode the questions, recording each record's start offset */
    static unsigned long starts[NQ + 2];
    long blen = 0;
    long ns   = 0;

    for (int i = 0; i < g_count; i++) {
        starts[ns++] = (unsigned long)(qbase + blen);
        blen += enc_string(g_q[i].q, 0, body + blen);
        for (int a = 0; a < 4; a++)
            blen += enc_string(g_q[i].a[a], 1, body + blen);
    }

    unsigned long spare_start = (unsigned long)(qbase + blen);
    starts[ns++] = spare_start;                          /* start of the spare record */
    unsigned long eof = spare_start + (unsigned long)g_sparelen;

    /* write 0011.200 = header + names + questions + spare */
    FILE *f = _wfopen(p200, L"wb");
    if (!f) {
        free(body);
        return 0;
    }
    fwrite(g_header, 1, CATOFF,     f);                   /* graphics blob (0 .. 0x366c) */
    fwrite(nbuf,     1, nlen,       f);                   /* category names              */
    fwrite(body,     1, blen,       f);                   /* questions                   */
    fwrite(g_spare,  1, g_sparelen, f);                   /* spare record, verbatim      */
    fclose(f);

    /* write 0011.100 = category boundaries + question starts + EOF */
    f = _wfopen(p100, L"wb");
    if (!f) {
        free(body);
        return 0;
    }
    for (int i = 0; i < NCAT; i++) { put_u32le(f, g_cathdr[i]); }
    for (long k = 0; k < ns; k++) { put_u32le(f, starts[k]); }
    put_u32le(f, eof);
    fclose(f);

    free(body);
    return 1;
}


static int tab_start(int t)
{
    return t == 0 ? 0 : (int)g_cathdr[t - 1];
}

static int tab_end(int t)
{
    if (t >= NCAT)                  /* the "Random" tab carries a name but no questions */
        return tab_start(t);
    int e = (int)g_cathdr[t];
    if (e > g_count)
        e = g_count;
    return e;
}

static void strip_crlf(wchar_t *s)
{
    wchar_t *p = s;
    for (; *s; s++)
        if (*s != L'\r' && *s != L'\n')
            *p++ = *s;
    *p = 0;
}

static void set_status(const wchar_t *m)
{
    SetWindowText(g_status, m);
}

static void list_line(int idx, wchar_t *out)
{
    wchar_t t[80];
    wcsncpy(t, g_q[idx].q, 79);
    t[79] = 0;
    if (wcslen(g_q[idx].q) > 70)
        wcscpy(t + 67, L"...");

    /* %ls = wide string (mingw's printf %s is narrow) */
    swprintf(out, 160, L"%04d  %ls", idx + 1, t);
}

static void populate_list(int t)
{
    SendMessage(g_list, LB_RESETCONTENT, 0, 0);

    wchar_t line[160];
    for (int idx = tab_start(t); idx < tab_end(t); idx++) {
        list_line(idx, line);
        SendMessage(g_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
}

static void load_editor(int idx)
{
    g_curIdx = idx;

    if (idx < 0) {
        SetWindowText(g_qedit, L"");
        for (int a = 0; a < 4; a++)
            SetWindowText(g_aedit[a], L"");
        return;
    }

    SetWindowText(g_qedit, g_q[idx].q);
    for (int a = 0; a < 4; a++)
        SetWindowText(g_aedit[a], g_q[idx].a[a]);
}

/* refresh one listbox line without disturbing the current selection */
static void refresh_row(int idx)
{
    int row = idx - tab_start(g_curTab);
    int cnt = (int)SendMessage(g_list, LB_GETCOUNT, 0, 0);
    if (row < 0 || row >= cnt)
        return;

    int sel = (int)SendMessage(g_list, LB_GETCURSEL, 0, 0);

    wchar_t line[160];
    list_line(idx, line);

    SendMessage(g_list, LB_DELETESTRING, row, 0);
    SendMessage(g_list, LB_INSERTSTRING, row, (LPARAM)line);
    SendMessage(g_list, LB_SETCURSEL,    sel, 0);
}

/* write the edit fields back into the model (answer 1 stays the correct one). */
static void commit_editor(void)
{
    if (g_curIdx < 0)
        return;

    Question *q = &g_q[g_curIdx];

    wchar_t qb[SLEN];
    GetWindowText(g_qedit, qb, SLEN);
    strip_crlf(qb);

    wchar_t e[4][SLEN];
    for (int a = 0; a < 4; a++) {
        GetWindowText(g_aedit[a], e[a], SLEN);
        strip_crlf(e[a]);
    }

    int changed = wcscmp(qb, q->q) != 0;
    for (int a = 0; a < 4 && !changed; a++)
        changed = wcscmp(e[a], q->a[a]) != 0;
    if (!changed)
        return;

    wcsncpy(q->q, qb, SLEN - 1);
    q->q[SLEN - 1] = 0;
    for (int a = 0; a < 4; a++) {
        wcsncpy(q->a[a], e[a], SLEN - 1);
        q->a[a][SLEN - 1] = 0;
    }

    g_dirty = 1;
    refresh_row(g_curIdx);
}

static void layout(HWND h)
{
    RECT rc;
    GetClientRect(h, &rc);

    int W = rc.right, H = rc.bottom;
    int m = 10, tabH = 26, btnH = 28, listW = 330;

    int cy = tabH + 6;                          /* top of the content area */
    int ch = H - cy - (btnH + 2 * m);

    MoveWindow(g_tab,  0, 0,  W,     tabH, TRUE);
    MoveWindow(g_list, m, cy, listW, ch,   TRUE);

    int rx = m + listW + m;                     /* right-hand panel */
    int rw = W - rx - m;
    if (rw < 200)
        rw = 200;

    /* category-name row */
    MoveWindow(g_catlabel,                rx,         cy + 4, 62,         18, TRUE);
    MoveWindow(g_catedit,                 rx + 64,    cy,     rw - 64 - 76, 22, TRUE);
    MoveWindow(GetDlgItem(h, IDC_RENAME), rx + rw - 72, cy,   72,         24, TRUE);

    MoveWindow(g_qlabel, rx, cy + 32,  rw, 18, TRUE);
    MoveWindow(g_qedit,  rx, cy + 52,  rw, 60, TRUE);
    MoveWindow(g_alabel, rx, cy + 118, rw, 18, TRUE);

    int ay = cy + 140;
    for (int a = 0; a < 4; a++) {
        MoveWindow(g_anum[a],  rx,      ay + a * 32 + 4, 74,      20, TRUE);
        MoveWindow(g_aedit[a], rx + 78, ay + a * 32,     rw - 78, 24, TRUE);
    }

    int by = H - btnH - m;
    MoveWindow(GetDlgItem(h, IDC_SAVE),   rx,      by, 90, btnH, TRUE);
    MoveWindow(GetDlgItem(h, IDC_REVERT), rx + 98, by, 90, btnH, TRUE);
    MoveWindow(g_status, m, by + 4, listW, 20, TRUE);
}

static HWND mk(HWND p, const wchar_t *cls, const wchar_t *t, DWORD st, int id)
{
    HWND h = CreateWindowEx(0, cls, t, WS_CHILD | WS_VISIBLE | st,
                            0, 0, 10, 10, p, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

static void set_title(void)
{
    wchar_t t[MAX_PATH + 64];
    swprintf(t, MAX_PATH + 64, L"Sraz Binary Editor - %ls%ls",
             g_loaded ? g_path200 : L"(no file)",
             g_dirty  ? L" *"     : L"");
    SetWindowText(g_main, t);
}

static void init_catnames(void)        /* defaults until a file is loaded */
{
    for (int c = 0; c < NCAT; c++)
        wcscpy(g_catname[c], CATS[c]);
    wcscpy(g_catname[NCAT], L"Random");
    g_ncat_names = NCAT + 1;
}

static void build_tabs(void)           /* labels come from g_catname (file names or defaults) */
{
    SendMessage(g_tab, TCM_DELETEALLITEMS, 0, 0);

    for (int c = 0; c <= NCAT; c++) {       /* NCAT question categories + the "Random" name */
        TCITEM ti;
        ti.mask    = TCIF_TEXT;
        ti.pszText = g_catname[c][0] ? g_catname[c] : (LPWSTR)(c < NCAT ? CATS[c] : L"Random");
        SendMessage(g_tab, TCM_INSERTITEM, c, (LPARAM)&ti);
    }
}

static void set_cat_field(void)
{
    SetWindowText(g_catedit, (g_curTab >= 0 && g_curTab <= NCAT) ? g_catname[g_curTab] : L"");
}

static void show_loaded(void)
{
    build_tabs();

    g_curTab = 0;
    SendMessage(g_tab, TCM_SETCURSEL, 0, 0);
    set_cat_field();

    populate_list(0);
    SendMessage(g_list, LB_SETCURSEL, 0, 0);
    load_editor(g_count > 0 ? 0 : -1);

    wchar_t s[64];
    swprintf(s, 64, L"%d questions loaded", g_count);
    set_status(s);
    set_title();
}

/* given a path to 0011.200, build the matching 0011.100 path in the same folder */
static void derive_100(const wchar_t *p200, wchar_t *p100)
{
    wcscpy(p100, p200);

    wchar_t *a = wcsrchr(p100, L'\\');
    wchar_t *b = wcsrchr(p100, L'/');
    if (b > a)
        a = b;

    if (a)
        wcscpy(a + 1, L"0011.100");
    else
        wcscpy(p100, L"0011.100");
}

static void do_open(void)
{
    wchar_t file[MAX_PATH] = L"0011.200";

    OPENFILENAME o;
    ZeroMemory(&o, sizeof o);
    o.lStructSize = sizeof o;
    o.hwndOwner   = g_main;
    o.lpstrFile   = file;
    o.nMaxFile    = MAX_PATH;
    o.lpstrFilter = L"Sraz data (0011.200)\0*.200\0All files\0*.*\0";
    o.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileName(&o))
        return;

    wcscpy(g_path200, file);
    derive_100(file, g_path100);

    if (load_binary(g_path200, g_path100))
        show_loaded();
    else
        MessageBox(g_main, L"Could not load the pair.\nEnsure 0011.100 sits next to 0011.200.",
                   L"Open", MB_ICONWARNING);
}

static void do_saveas(void)
{
    if (!g_loaded) {
        MessageBox(g_main, L"Open a 0011.200 first.", L"Save", MB_ICONINFORMATION);
        return;
    }

    wchar_t file[MAX_PATH] = L"0011.200";

    OPENFILENAME o;
    ZeroMemory(&o, sizeof o);
    o.lStructSize = sizeof o;
    o.hwndOwner   = g_main;
    o.lpstrFile   = file;
    o.nMaxFile    = MAX_PATH;
    o.lpstrFilter = L"Sraz data (0011.200)\0*.200\0All files\0*.*\0";
    o.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (!GetSaveFileName(&o))
        return;

    commit_editor();

    wchar_t p100[MAX_PATH];
    derive_100(file, p100);

    if (save_binary(file, p100)) {
        wcscpy(g_path200, file);
        wcscpy(g_path100, p100);
        g_dirty = 0;
        set_status(L"Saved binary pair");
        set_title();
    } else {
        set_status(L"ERROR writing files");
    }
}

static void do_save(void)
{
    if (!g_loaded) {
        MessageBox(g_main, L"Open a 0011.200 first.", L"Save", MB_ICONINFORMATION);
        return;
    }
    if (!g_path200[0]) {
        do_saveas();
        return;
    }

    commit_editor();

    if (save_binary(g_path200, g_path100)) {
        g_dirty = 0;
        set_status(L"Saved 0011.200 + 0011.100");
        set_title();
    } else {
        set_status(L"ERROR writing files");
    }
}

static void on_create(HWND hwnd)
{
    g_main = hwnd;

    /* a real Unicode-capable UI font (DEFAULT_GUI_FONT is the legacy MS Sans Serif
       and cannot render Croatian glyphs) */
    g_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                         DEFAULT_PITCH | FF_SWISS, L"Tahoma");
    if (!g_font)
        g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    init_catnames();

    /* File menu */
    HMENU bar  = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenu(file, MF_STRING,    IDM_OPEN,   L"&Open 0011.200...");
    AppendMenu(file, MF_STRING,    IDM_SAVE,   L"&Save");
    AppendMenu(file, MF_STRING,    IDM_SAVEAS, L"Save &As...");
    AppendMenu(file, MF_SEPARATOR, 0,          NULL);
    AppendMenu(file, MF_STRING,    IDM_EXIT,   L"E&xit");
    AppendMenu(bar,  MF_POPUP, (UINT_PTR)file, L"&File");
    SetMenu(hwnd, bar);

    /* controls (positioned later by layout()) */
    g_tab      = mk(hwnd, WC_TABCONTROL, L"", 0, IDC_TAB);   /* labelled after the file loads */
    g_list     = mk(hwnd, L"LISTBOX",  L"", LBS_NOTIFY | WS_VSCROLL | WS_BORDER, IDC_LIST);

    g_catlabel = mk(hwnd, L"STATIC", L"Category:", 0, IDC_CATLABEL);
    g_catedit  = mk(hwnd, L"EDIT",   L"", ES_AUTOHSCROLL | WS_BORDER, IDC_CATEDIT);
    mk(hwnd, L"BUTTON", L"Rename", BS_PUSHBUTTON, IDC_RENAME);

    g_qlabel   = mk(hwnd, L"STATIC", L"Question:", 0, IDC_QLABEL);
    g_qedit    = mk(hwnd, L"EDIT",   L"", ES_MULTILINE | ES_AUTOVSCROLL | WS_BORDER | WS_VSCROLL, IDC_QEDIT);
    g_alabel   = mk(hwnd, L"STATIC",
                    L"Answers  (answer 1 is the CORRECT one; the game shuffles all four):",
                    0, IDC_ALABEL);

    static const wchar_t *AN[4] = { L"1 (correct):", L"2:", L"3:", L"4:" };
    for (int a = 0; a < 4; a++) {
        g_anum[a]  = mk(hwnd, L"STATIC", AN[a], 0, IDC_N0 + a);
        g_aedit[a] = mk(hwnd, L"EDIT",   L"", ES_AUTOHSCROLL | WS_BORDER, IDC_A0 + a);
    }

    mk(hwnd, L"BUTTON", L"Save",   BS_PUSHBUTTON, IDC_SAVE);
    mk(hwnd, L"BUTTON", L"Revert", BS_PUSHBUTTON, IDC_REVERT);
    g_status = mk(hwnd, L"STATIC", L"", 0, IDC_STATUS);

    /* auto-load the pair from the current folder, if present */
    if (load_binary(L"0011.200", L"0011.100")) {
        wcscpy(g_path200, L"0011.200");
        wcscpy(g_path100, L"0011.100");
        show_loaded();
    } else {
        build_tabs();
        g_curTab = 0;
        set_cat_field();
        set_status(L"File > Open a 0011.200 to begin");
        set_title();
    }

    layout(hwnd);
}

/* rename the current tab/category from the Category field */
static void on_rename(void)
{
    wchar_t nm[SLEN];
    GetWindowText(g_catedit, nm, SLEN);
    strip_crlf(nm);

    if (!nm[0] || g_curTab < 0 || g_curTab > NCAT)    /* allow the 9th ("Random") name too */
        return;

    wcsncpy(g_catname[g_curTab], nm, SLEN - 1);
    g_catname[g_curTab][SLEN - 1] = 0;

    TCITEM ti;
    ti.mask    = TCIF_TEXT;
    ti.pszText = g_catname[g_curTab];
    SendMessage(g_tab, TCM_SETITEM, g_curTab, (LPARAM)&ti);

    g_dirty = 1;
    set_title();
    set_status(L"Category renamed (Save to write it into the file)");
}

static void on_tab_changed(void)
{
    commit_editor();

    g_curTab = (int)SendMessage(g_tab, TCM_GETCURSEL, 0, 0);
    populate_list(g_curTab);
    SendMessage(g_list, LB_SETCURSEL, 0, 0);
    load_editor(tab_start(g_curTab) < g_count ? tab_start(g_curTab) : -1);
    set_cat_field();
}

static void on_list_select(void)
{
    int row = (int)SendMessage(g_list, LB_GETCURSEL, 0, 0);
    if (row == LB_ERR)
        return;

    int ni = tab_start(g_curTab) + row;
    if (ni != g_curIdx) {
        commit_editor();
        load_editor(ni);
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {

    case WM_CREATE:
        on_create(h);
        return 0;

    case WM_SIZE:
        layout(h);
        return 0;

    case WM_NOTIFY: {
        LPNMHDR n = (LPNMHDR)lp;
        if (n->idFrom == IDC_TAB && n->code == TCN_SELCHANGE)
            on_tab_changed();
        return 0;
    }

    case WM_COMMAND: {
        int id   = LOWORD(wp);
        int code = HIWORD(wp);

        switch (id) {
        case IDC_LIST:
            if (code == LBN_SELCHANGE)
                on_list_select();
            break;

        case IDC_SAVE:
        case IDM_SAVE:
            do_save();
            break;

        case IDM_SAVEAS:
            do_saveas();
            break;

        case IDM_OPEN:
            if (g_dirty &&
                MessageBox(h, L"Discard unsaved changes?", L"Open",
                           MB_OKCANCEL | MB_ICONQUESTION) == IDCANCEL)
                break;
            do_open();
            break;

        case IDC_REVERT:
            load_editor(g_curIdx);
            set_status(L"Reverted current question");
            break;

        case IDC_RENAME:
            on_rename();
            break;

        case IDM_EXIT:
            SendMessage(h, WM_CLOSE, 0, 0);
            break;
        }
        return 0;
    }

    case WM_CLOSE:
        if (g_dirty) {
            int r = MessageBox(h, L"Save changes to the binary files before exiting?",
                               L"Sraz Binary Editor", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL)
                return 0;
            if (r == IDYES)
                do_save();
        }
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cl, int sh)
{
    (void)hp;
    (void)cl;

    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_TAB_CLASSES };
    InitCommonControlsEx(&ic);

    WNDCLASS wc;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hi;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SrazBinEditor";
    RegisterClass(&wc);

    HWND h = CreateWindowEx(0, L"SrazBinEditor", L"Sraz Binary Editor", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 940, 600, NULL, NULL, hi, NULL);
    ShowWindow(h, sh);
    UpdateWindow(h);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessage(h, &msg)) {        /* lets Tab/arrows move between controls */
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return 0;
}
