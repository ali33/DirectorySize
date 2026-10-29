// DirectorySize (C++) — Copyright (c) 2026 META.vn
// Author: Nguyễn Đức Sơn
// SPDX-License-Identifier: GPL-2.0-only
// dirsize-gui: pure Win32 front-end. UI text is Vietnamese.
//
// Threading: the scan runs on the Scanner's own thread. That thread never
// touches the UI; it only PostMessage()s WM_APP_DONE (tagged with the scan's
// generation number) when it ends. All progress is pulled by a 500 ms timer on
// the UI thread via Scanner::snapshot().
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <exception>
#include <memory>
#include <cstring>
#include <string>
#include <vector>

#include "core/scanner.h"
#include "core/size_format.h"
#include "core/version.h"
#include "platform/walker.h"

using namespace dirsize;

namespace {

enum : int {
  IDC_PATH = 101, IDC_BROWSE, IDC_START, IDC_STOP, IDC_PROGRESS, IDC_LIST, IDC_STATUS,
  IDM_ABOUT = 201, IDM_COPY_PATH = 210, IDM_COPY_PATH_SIZE, IDM_OPEN_EXPLORER, IDM_COPY_ALL
};
constexpr UINT WM_APP_DONE = WM_APP + 1;
constexpr UINT_PTR kTimer = 1;       // scan progress
constexpr UINT_PTR kFlashTimer = 2;  // end of a temporary status message
constexpr UINT kTimerMs = 500;
constexpr UINT kFlashMs = 4000;
constexpr size_t kTopN = 100;
constexpr int kIconResId = 1;  // matches "1 ICON" in dirsize-gui.rc

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}
std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

struct Row {
  std::wstring path, size;
};

struct App {
  HWND hwnd = nullptr, path = nullptr, browse = nullptr, start = nullptr, stop = nullptr, progress = nullptr,
       list = nullptr, status = nullptr;
  HFONT font = nullptr;
  HICON icon_big = nullptr, icon_small = nullptr;
  UINT dpi = 96;
  std::unique_ptr<Scanner> scanner;
  std::vector<Row> rows;      // what the ListView shows (index = row)
  std::wstring error_detail;  // inaccessible-directory samples, shown on click
  std::wstring status0;       // current text of status part 0 (without a flash on top)
  bool flashing = false;      // a temporary message currently covers part 0
  bool stopping = false;
  bool menu_open = false;     // context menu showing: the list is frozen
  LPARAM generation = 0;      // id of the current scan; stale WM_APP_DONE are ignored
} g;

int S(int v) { return MulDiv(v, (int)g.dpi, 96); }

UINT window_dpi(HWND h) {
  using Fn = UINT(WINAPI*)(HWND);
  static Fn fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
  if (fn && h) return fn(h);
  HDC dc = GetDC(nullptr);
  UINT d = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
  ReleaseDC(nullptr, dc);
  return d ? d : 96;
}

int metric_for_dpi(int index, UINT dpi) {
  using Fn = int(WINAPI*)(int, UINT);
  static Fn fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
  return fn ? fn(index, dpi) : GetSystemMetrics(index);
}

void set_part(int i, const std::wstring& t) { SendMessageW(g.status, SB_SETTEXTW, (WPARAM)i, (LPARAM)t.c_str()); }

// Part 0 shows the scan state, unless a temporary message ("Đã copy: ...") covers it.
void set_state(const std::wstring& t) {
  g.status0 = t;
  if (!g.flashing) set_part(0, t);
}
void flash(const std::wstring& t, UINT ms = kFlashMs) {
  g.flashing = true;
  set_part(0, t);
  SetTimer(g.hwnd, kFlashTimer, ms, nullptr);
}

void show_error(const std::wstring& msg) {
  set_state(L"Lỗi: " + msg);
  MessageBoxW(g.hwnd, msg.c_str(), L"Directory Size - Lỗi", MB_OK | MB_ICONERROR);
}

void load_icons() {
  if (g.icon_big) DestroyIcon(g.icon_big);
  if (g.icon_small) DestroyIcon(g.icon_small);
  HINSTANCE inst = GetModuleHandleW(nullptr);
  g.icon_big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(kIconResId), IMAGE_ICON, metric_for_dpi(SM_CXICON, g.dpi),
                                 metric_for_dpi(SM_CYICON, g.dpi), LR_DEFAULTCOLOR);
  g.icon_small = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(kIconResId), IMAGE_ICON, metric_for_dpi(SM_CXSMICON, g.dpi),
                                   metric_for_dpi(SM_CYSMICON, g.dpi), LR_DEFAULTCOLOR);
  SendMessageW(g.hwnd, WM_SETICON, ICON_BIG, (LPARAM)g.icon_big);
  SendMessageW(g.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g.icon_small);
}

void apply_font() {
  if (g.font) DeleteObject(g.font);
  g.font = CreateFontW(-MulDiv(9, (int)g.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                       L"Segoe UI");
  for (HWND h : {g.path, g.browse, g.start, g.stop, g.list, g.status})
    SendMessageW(h, WM_SETFONT, (WPARAM)g.font, TRUE);
}

void layout() {
  RECT rc;
  GetClientRect(g.hwnd, &rc);
  int w = rc.right, m = S(10), bh = S(28);
  SendMessageW(g.status, WM_SIZE, 0, 0);
  RECT sr;
  GetWindowRect(g.status, &sr);
  int sh = sr.bottom - sr.top;
  int parts[3] = {w * 40 / 100, w * 72 / 100, -1};
  SendMessageW(g.status, SB_SETPARTS, 3, (LPARAM)parts);

  int bw1 = S(130), bw2 = S(80);
  int x = w - m;
  x -= bw2;
  MoveWindow(g.stop, x, m, bw2, bh, TRUE);
  x -= S(6) + bw2;
  MoveWindow(g.start, x, m, bw2, bh, TRUE);
  x -= S(6) + bw1;
  MoveWindow(g.browse, x, m, bw1, bh, TRUE);
  MoveWindow(g.path, m, m, x - S(6) - m, bh, TRUE);
  MoveWindow(g.progress, m, m + bh + S(8), w - 2 * m, S(8), TRUE);
  int ly = m + bh + S(8) + S(8) + S(8);
  MoveWindow(g.list, m, ly, w - 2 * m, rc.bottom - sh - ly - m, TRUE);

  int sizew = S(130);
  ListView_SetColumnWidth(g.list, 1, sizew);
  ListView_SetColumnWidth(g.list, 0, w - 2 * m - sizew - GetSystemMetrics(SM_CXVSCROLL) - S(6));
}

void set_running(bool running) {
  EnableWindow(g.start, !running);
  EnableWindow(g.browse, !running);
  SendMessageW(g.path, EM_SETREADONLY, running, 0);
  EnableWindow(g.stop, running && !g.stopping);
  // Animated "busy" indicator for as long as a scan is in flight.
  SendMessageW(g.progress, PBM_SETMARQUEE, running, 40);
  ShowWindow(g.progress, running ? SW_SHOW : SW_HIDE);
}

// ---- list selection helpers -------------------------------------------------
std::vector<int> selected_rows() {
  std::vector<int> v;
  for (int i = ListView_GetNextItem(g.list, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(g.list, i, LVNI_SELECTED))
    if ((size_t)i < g.rows.size()) v.push_back(i);
  return v;
}

void select_only(int row) {
  ListView_SetItemState(g.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
  ListView_SetItemState(g.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

// ---- clipboard -------------------------------------------------------------
// Returns an empty string on success, otherwise the reason (shown on screen).
std::wstring put_on_clipboard(const std::wstring& text) {
  bool opened = false;
  for (int attempt = 0; attempt < 10 && !opened; ++attempt) {
    opened = OpenClipboard(g.hwnd) != FALSE;
    if (!opened) Sleep(30);  // another app may be holding the clipboard for a moment
  }
  if (!opened) return L"Không mở được clipboard (đang bị ứng dụng khác giữ), mã " + std::to_wstring(GetLastError());
  std::wstring err;
  EmptyClipboard();
  size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!mem) {
    err = L"Không đủ bộ nhớ để copy";
  } else {
    void* p = GlobalLock(mem);
    if (!p) {
      GlobalFree(mem);
      err = L"Không khoá được bộ nhớ clipboard";
    } else {
      memcpy(p, text.c_str(), bytes);
      GlobalUnlock(mem);
      if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        err = L"SetClipboardData lỗi, mã " + std::to_wstring(GetLastError());
        GlobalFree(mem);  // ownership passes to the system only on success
      }
    }
  }
  CloseClipboard();
  return err;
}

// Copy `text` and tell the user: "Đã copy: <what>" on success, the error otherwise.
void copy_and_report(const std::wstring& text, const std::wstring& what) {
  std::wstring err = put_on_clipboard(text);
  if (err.empty()) flash(L"Đã copy: " + what);
  else flash(L"Lỗi copy: " + err, 8000);
}

std::wstring join_lines(const std::vector<Row>& rs, bool with_size) {
  std::wstring out;
  for (size_t i = 0; i < rs.size(); ++i) {
    if (i) out += L"\r\n";
    out += rs[i].path;
    if (with_size) out += L"\t" + rs[i].size;
  }
  return out;
}

std::wstring describe(const std::vector<Row>& rs) {
  return rs.size() == 1 ? rs[0].path : std::to_wstring(rs.size()) + L" dòng";
}

void open_in_explorer(const std::wstring& path) {
  HINSTANCE r = ShellExecuteW(g.hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if ((INT_PTR)r <= 32) flash(L"Lỗi mở Explorer (mã " + std::to_wstring((INT_PTR)r) + L"): " + path, 8000);
}

// Right-click / Menu key / Shift+F10. `lp` is the WM_CONTEXTMENU lParam (screen coords, or -1 for keyboard).
void on_context_menu(LPARAM lp) {
  if (g.rows.empty()) return;
  POINT pt;
  int row = -1;
  if ((int)(short)LOWORD(lp) == -1 && (int)(short)HIWORD(lp) == -1) {
    // Keyboard: use the focused row (or first selected) and anchor the menu at it.
    row = ListView_GetNextItem(g.list, -1, LVNI_FOCUSED);
    if (row < 0) row = ListView_GetNextItem(g.list, -1, LVNI_SELECTED);
    if (row < 0) return;
    RECT ir;
    ListView_GetItemRect(g.list, row, &ir, LVIR_LABEL);
    pt = {ir.left + S(16), ir.bottom};
    ClientToScreen(g.list, &pt);
  } else {
    pt = {(int)(short)LOWORD(lp), (int)(short)HIWORD(lp)};
    POINT c = pt;
    ScreenToClient(g.list, &c);
    LVHITTESTINFO hi = {};
    hi.pt = c;
    ListView_SubItemHitTest(g.list, &hi);
    row = hi.iItem;
    if (row < 0) return;  // clicked on empty space
  }
  if ((size_t)row >= g.rows.size()) return;

  // Freeze the list and pin what the user acted on BEFORE the menu opens: the
  // 500 ms refresh could otherwise reorder rows under the menu.
  g.menu_open = true;
  bool row_selected = (ListView_GetItemState(g.list, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
  if (!row_selected) select_only(row);
  std::vector<Row> picked;
  for (int i : selected_rows()) picked.push_back(g.rows[(size_t)i]);
  std::vector<Row> all = g.rows;
  if (picked.empty()) picked.push_back(g.rows[(size_t)row]);

  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, IDM_COPY_PATH, L"Copy đường dẫn");
  AppendMenuW(m, MF_STRING, IDM_COPY_PATH_SIZE, L"Copy đường dẫn + dung lượng");
  AppendMenuW(m, MF_STRING | (picked.size() == 1 ? 0 : MF_GRAYED), IDM_OPEN_EXPLORER, L"Mở trong Explorer");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, IDM_COPY_ALL, L"Copy tất cả các dòng đang hiện");
  SetMenuDefaultItem(m, IDM_COPY_PATH, FALSE);
  SetForegroundWindow(g.hwnd);
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g.hwnd, nullptr);
  DestroyMenu(m);
  g.menu_open = false;

  switch (cmd) {
    case IDM_COPY_PATH: copy_and_report(join_lines(picked, false), describe(picked)); break;
    case IDM_COPY_PATH_SIZE: copy_and_report(join_lines(picked, true), describe(picked)); break;
    case IDM_OPEN_EXPLORER: open_in_explorer(picked[0].path); break;
    case IDM_COPY_ALL: copy_and_report(join_lines(all, true), std::to_wstring(all.size()) + L" dòng"); break;
    default: break;
  }
}

void refresh(bool final_update);

void on_copy_key() {  // Ctrl+C with the list focused
  std::vector<Row> picked;
  for (int i : selected_rows()) picked.push_back(g.rows[(size_t)i]);
  if (picked.empty()) return;
  copy_and_report(join_lines(picked, false), describe(picked));
}

void refresh(bool final_update) {
  if (!g.scanner) return;
  SizeSnapshot s = g.scanner->snapshot(kTopN);
  if (!g.menu_open) {
    // The rows reorder as sizes change. Remember the selection by PATH and restore
    // it on the new rows so it keeps pointing at the same directories.
    std::vector<std::wstring> sel;
    std::wstring focus;
    for (int i : selected_rows()) sel.push_back(g.rows[(size_t)i].path);
    int fi = ListView_GetNextItem(g.list, -1, LVNI_FOCUSED);
    if (fi >= 0 && (size_t)fi < g.rows.size()) focus = g.rows[(size_t)fi].path;

    g.rows.clear();
    g.rows.reserve(s.top.size());
    for (const auto& e : s.top) g.rows.push_back({widen(e.path), widen(format_size(e.bytes))});
    ListView_SetItemCountEx(g.list, (int)g.rows.size(), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    if (!sel.empty() || !focus.empty()) {
      ListView_SetItemState(g.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
      for (size_t i = 0; i < g.rows.size(); ++i) {
        UINT st = 0;
        for (const auto& p : sel)
          if (p == g.rows[i].path) st |= LVIS_SELECTED;
        if (g.rows[i].path == focus) st |= LVIS_FOCUSED;
        if (st) ListView_SetItemState(g.list, (int)i, st, LVIS_SELECTED | LVIS_FOCUSED);
      }
    }
    InvalidateRect(g.list, nullptr, FALSE);
  }

  std::wstring cnt = widen(format_count(s.dirs)) + L" thư mục · " + widen(format_count(s.files)) + L" tệp · " +
                     widen(format_size(s.total_bytes));
  set_part(1, cnt);
  if (s.inaccessible) {
    set_part(2, widen(format_count(s.inaccessible)) + L" thư mục không truy cập được (bấm để xem)");
    std::wstring d;
    for (const auto& e : s.error_samples) d += widen(e) + L"\r\n";
    if (s.inaccessible > s.error_samples.size()) d += L"...\r\n";
    g.error_detail = d;
  } else {
    set_part(2, L"Không có lỗi truy cập");
    g.error_detail.clear();
  }
  if (!final_update) {
    wchar_t b[64];
    swprintf(b, 64, L"Đang quét... %.1f giây", (double)g.scanner->elapsed().count() / 1000.0);
    set_state(g.stopping ? L"Đang dừng..." : b);
  }
}

void on_start() {
  if (g.scanner && g.scanner->running()) return;
  int len = GetWindowTextLengthW(g.path);
  std::wstring buf((size_t)len + 1, L'\0');
  GetWindowTextW(g.path, &buf[0], len + 1);
  buf.resize((size_t)len);
  std::string err;
  HWND hwnd = g.hwnd;
  g.stopping = false;
  if (!g.scanner) g.scanner.reset(new Scanner(make_default_walker(WalkOptions{})));
  LPARAM gen = ++g.generation;
  if (!g.scanner->start(narrow(buf), [hwnd, gen](ScanState st) { PostMessageW(hwnd, WM_APP_DONE, (WPARAM)st, gen); }, err)) {
    show_error(widen(err));
    return;
  }
  g.rows.clear();
  ListView_SetItemCountEx(g.list, 0, 0);
  set_running(true);
  set_state(L"Đang quét...");
  SetTimer(g.hwnd, kTimer, kTimerMs, nullptr);
}

void on_done(ScanState st) {
  KillTimer(g.hwnd, kTimer);
  g.scanner->join();
  set_running(false);
  refresh(true);
  g.stopping = false;
  wchar_t b[96];
  double sec = (double)g.scanner->elapsed().count() / 1000.0;
  if (st == ScanState::Failed) {
    show_error(widen(g.scanner->error()));
  } else if (st == ScanState::Stopped) {
    swprintf(b, 96, L"Đã dừng (kết quả một phần) - %.1f giây", sec);
    set_state(b);
  } else {
    swprintf(b, 96, L"Hoàn tất! - %.1f giây", sec);
    set_state(b);
  }
}

void show_about() {
  std::wstring t = widen(about_text());
  MessageBoxW(g.hwnd, t.c_str(), L"Giới thiệu", MB_OK | MB_ICONINFORMATION);
}

void on_browse() {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
    show_error(L"Không mở được hộp thoại chọn thư mục");
    return;
  }
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  dlg->SetTitle(L"Chọn thư mục cần tính dung lượng");
  wchar_t cur[4096];
  GetWindowTextW(g.path, cur, 4096);
  IShellItem* init = nullptr;
  if (SUCCEEDED(SHCreateItemFromParsingName(cur, nullptr, IID_PPV_ARGS(&init)))) {
    dlg->SetFolder(init);
    init->Release();
  }
  if (SUCCEEDED(dlg->Show(g.hwnd))) {
    IShellItem* it = nullptr;
    if (SUCCEEDED(dlg->GetResult(&it))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
        SetWindowTextW(g.path, p);
        CoTaskMemFree(p);
      }
      it->Release();
    }
  }
  dlg->Release();
}

LRESULT handle_message(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
    case WM_CREATE: {
      g.hwnd = h;
      g.dpi = window_dpi(h);
      DWORD sty = WS_CHILD | WS_VISIBLE;
      g.path = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"C:\\", sty | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, h,
                               (HMENU)(INT_PTR)IDC_PATH, nullptr, nullptr);
      g.browse = CreateWindowExW(0, L"BUTTON", L"Chọn thư mục...", sty | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, h,
                                 (HMENU)(INT_PTR)IDC_BROWSE, nullptr, nullptr);
      g.start = CreateWindowExW(0, L"BUTTON", L"Tính", sty | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, h,
                                (HMENU)(INT_PTR)IDC_START, nullptr, nullptr);
      g.stop = CreateWindowExW(0, L"BUTTON", L"Dừng", sty | WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED, 0, 0, 0, 0, h,
                               (HMENU)(INT_PTR)IDC_STOP, nullptr, nullptr);
      g.progress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | PBS_MARQUEE, 0, 0, 0, 0, h,
                                   (HMENU)(INT_PTR)IDC_PROGRESS, nullptr, nullptr);
      // Multiple selection allowed (no LVS_SINGLESEL) so several paths can be copied at once.
      g.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr, sty | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS,
                               0, 0, 0, 0, h, (HMENU)(INT_PTR)IDC_LIST, nullptr, nullptr);
      ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
      g.status = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, sty | SBARS_SIZEGRIP, 0, 0, 0, 0, h,
                                 (HMENU)(INT_PTR)IDC_STATUS, nullptr, nullptr);
      LVCOLUMNW c = {};
      c.mask = LVCF_TEXT | LVCF_FMT | LVCF_WIDTH;
      c.fmt = LVCFMT_LEFT;
      c.cx = 400;
      c.pszText = const_cast<LPWSTR>(L"Đường dẫn");
      ListView_InsertColumn(g.list, 0, &c);
      c.fmt = LVCFMT_RIGHT;
      c.cx = 120;
      c.pszText = const_cast<LPWSTR>(L"Dung lượng");
      ListView_InsertColumn(g.list, 1, &c);
      apply_font();
      load_icons();
      HMENU bar = CreateMenu(), help = CreatePopupMenu();
      AppendMenuW(help, MF_STRING, IDM_ABOUT, L"Giới thiệu...");
      AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"Trợ giúp");
      SetMenu(h, bar);
      SendMessageW(h, DM_SETDEFID, IDC_START, 0);
      set_state(L"Sẵn sàng");
      set_part(1, L"0 thư mục");
      set_part(2, L"");
      layout();
      return 0;
    }
    case WM_SIZE:
      if (g.status) layout();
      return 0;
    case WM_GETMINMAXINFO: {
      auto* mm = (MINMAXINFO*)lp;
      mm->ptMinTrackSize.x = S(560);
      mm->ptMinTrackSize.y = S(320);
      return 0;
    }
    case WM_DPICHANGED: {
      g.dpi = HIWORD(wp);
      const RECT* r = (const RECT*)lp;
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      apply_font();
      load_icons();
      layout();
      return 0;
    }
    case WM_CONTEXTMENU:
      if ((HWND)wp == g.list) {
        on_context_menu(lp);
        // The list kept refreshing underneath only in non-frozen state; catch up now.
        if (g.scanner) refresh(!g.scanner->running());
        return 0;
      }
      break;
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDC_BROWSE: on_browse(); return 0;
        case IDM_ABOUT: show_about(); return 0;
        case IDC_START:
        case IDOK: on_start(); return 0;
        case IDC_STOP:
          if (g.scanner && g.scanner->running()) {
            g.stopping = true;
            g.scanner->request_stop();
            EnableWindow(g.stop, FALSE);
            set_state(L"Đang dừng...");
          }
          return 0;
        case IDCANCEL:
          return 0;
      }
      break;
    case WM_TIMER:
      if (wp == kTimer) {
        // A WM_TIMER can still be queued after KillTimer: only refresh a live scan.
        if (g.scanner && g.scanner->running()) refresh(false);
      } else if (wp == kFlashTimer) {
        KillTimer(h, kFlashTimer);
        g.flashing = false;
        set_part(0, g.status0);
      }
      return 0;
    case WM_APP_DONE:
      if (lp == g.generation) on_done((ScanState)wp);  // ignore a finish message from an older scan
      return 0;
    case WM_NOTIFY: {
      auto* nh = (NMHDR*)lp;
      if (nh->idFrom == IDC_LIST && nh->code == LVN_GETDISPINFOW) {
        auto* di = (NMLVDISPINFOW*)lp;
        if (di->item.iItem >= 0 && (size_t)di->item.iItem < g.rows.size() && (di->item.mask & LVIF_TEXT) &&
            di->item.cchTextMax > 0) {
          const Row& r = g.rows[(size_t)di->item.iItem];
          const std::wstring& t = di->item.iSubItem == 0 ? r.path : r.size;
          wcsncpy(di->item.pszText, t.c_str(), (size_t)di->item.cchTextMax - 1);
          di->item.pszText[di->item.cchTextMax - 1] = 0;
        }
        return 0;
      }
      if (nh->idFrom == IDC_LIST && nh->code == LVN_KEYDOWN) {
        auto* kd = (NMLVKEYDOWN*)lp;
        if (kd->wVKey == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) on_copy_key();
        return 0;
      }
      if (nh->idFrom == IDC_STATUS && (nh->code == NM_CLICK || nh->code == NM_DBLCLK) && !g.error_detail.empty()) {
        MessageBoxW(h, g.error_detail.c_str(), L"Thư mục không truy cập được (mẫu)", MB_OK | MB_ICONWARNING);
        return 0;
      }
      break;
    }
    case WM_CLOSE:
      if (g.scanner && g.scanner->running()) {
        ShowWindow(h, SW_HIDE);  // do not look frozen while the worker winds down
        g.scanner->request_stop();
      }
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      KillTimer(h, kTimer);
      KillTimer(h, kFlashTimer);
      g.scanner.reset();  // requests stop and joins before the window goes away
      if (g.font) DeleteObject(g.font);
      if (g.icon_big) DestroyIcon(g.icon_big);
      if (g.icon_small) DestroyIcon(g.icon_small);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, m, wp, lp);
}

// Exceptions must not cross the Win32 callback boundary: show them on screen.
LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  try {
    return handle_message(h, m, wp, lp);
  } catch (const std::exception& e) {
    if (g.status) flash(L"Lỗi nội bộ: " + widen(e.what()), 10000);
  } catch (...) {
    if (g.status) flash(L"Lỗi nội bộ không xác định", 10000);
  }
  return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES};
  InitCommonControlsEx(&icc);

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = wndproc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(kIconResId), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                               GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
  wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(kIconResId), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                 GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
  wc.lpszClassName = L"DirSizeMain";
  RegisterClassExW(&wc);

  HWND h = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Directory Size", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                           CW_USEDEFAULT, 960, 640, nullptr, nullptr, inst, nullptr);
  if (!h) return 1;
  // Size the window for the current DPI (CreateWindow's 960x640 is in device pixels).
  UINT d = window_dpi(h);
  SetWindowPos(h, nullptr, 0, 0, MulDiv(960, (int)d, 96), MulDiv(640, (int)d, 96), SWP_NOMOVE | SWP_NOZORDER);
  ShowWindow(h, show);
  UpdateWindow(h);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(h, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  CoUninitialize();
  return (int)msg.wParam;
}
